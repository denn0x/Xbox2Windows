"""Draw a translucent source over a known background through a device slot 57 pipeline whose state
block A carries CB_BLENDn_CONTROL words, and read every pixel back.

usage: startup_slot57_blend_draw_check.py <startup_stream_consumer> <warp|hardware>

The background is opaque blue and the source is red with alpha 0.5. With SRC_ALPHA / INV_SRC_ALPHA
in target 0's word the drawn half must read (128, 0, 128, 191); unblended it reads (255, 0, 0, 128). Each channel may be off by one
(0.5 lands between two UNORM values); the two results are 127 apart in red.
Controls: the same word with ENABLE clear, and the same word four bytes before and after +0xD0.
With two targets bound, a word the decoder refuses for target 1 must leave the pipeline on the
path it had before (no blend state from the registers, both targets unblended), not draw target 1
opaque next to a blended target 0.

Not covered: CB_TARGET_MASK is not located, so every case writes all four channels.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from startup_stream_check import container, gfx2hlsl
from startup_stream_draw_check import vertex, vop1

BLEND = 0x40000504    # ENABLE, COLOR_SRCBLEND SRC_ALPHA (4), COLOR_DESTBLEND ONE_MINUS_SRC_ALPHA (5)
REFUSED = 0x4000050B  # the same with BOTH_SRC_ALPHA (11) as the source factor: no D3D12 form
CLEAR, SOURCE, BLENDED = (0, 0, 255, 255), (255, 0, 0, 128), (128, 0, 128, 191)


def pixel(targets):
    # v0 = 0.0, v1 = 1.0, v2 = 0.5. Export (v1, v0, v0, v2) to each target; the last export is marked done.
    words = [vop1(1, 0, 128), vop1(1, 1, 242), vop1(1, 2, 240)]
    for target in range(targets):
        last = target == targets - 1
        words.extend([0xf800000f | target << 4 | (0x1800 if last else 0), 0x02000001])
    return container(0, words + [0xbf810000])


def close(actual, expected):
    return all(abs(a - e) <= 1 for a, e in zip(actual, expected))


def halves(pixels, name):
    """The drawn (left) colour of an 8x4 RGBA target; the right half must still be the background."""
    assert len(pixels) == 8 * 4 * 4, f"{name}: {len(pixels)} bytes"
    drawn = None
    for y in range(4):
        for x in range(8):
            texel = tuple(pixels[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4])
            if x >= 4:
                assert texel == CLEAR, f"{name}: background at ({x}, {y}) is {texel}"
            elif drawn is None:
                drawn = texel
            else:
                assert texel == drawn, f"{name}: ({x}, {y}) is {texel}, (0, 0) is {drawn}"
    return drawn


def check(directory, consumer, backend):
    cache = directory / "cache"
    cache.mkdir()
    vs, root, output = (directory / name for name in ("vs", "root", "pixels"))
    shaders = {1: directory / "ps1", 2: directory / "ps2"}
    vs.write_bytes(vertex())
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1",
               X2W_SHADER_CACHE=str(cache))
    env.setdefault("X2W_D3D12_DEBUG", "0")
    for targets, ps in shaders.items():
        ps.write_bytes(pixel(targets))
        subprocess.run([consumer, vs, ps, root], env=env, check=True)
    subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], env=env, check=True)

    def draw(name, targets, spec):
        mode = "slot57-mrt" if targets == 2 else "slot57"
        result = subprocess.run([consumer, vs, shaders[targets], root, backend, output, mode, spec], env=env,
                                capture_output=True, text=True)
        if backend == "hardware" and result.returncode == 77:
            print("No D3D12 feature-level 12 hardware adapter")
            sys.exit(77)
        if result.returncode or "-> drawn;" not in result.stderr:
            raise RuntimeError(f"{name}: draw failed on {backend}: {result.returncode}\n{result.stderr}")
        source = [line for line in result.stderr.splitlines() if "creation slot 57" in line and "blend from" in line]
        assert len(source) == 1, f"{name}: {source}"
        pixels = output.read_bytes()
        assert len(pixels) == targets * 128, f"{name}: {len(pixels)} bytes"
        colours = [halves(pixels[t * 128:(t + 1) * 128], f"{name} target {t}") for t in range(targets)]
        from_registers = "blend from colour-blend registers at +0xd0" in source[0]
        print(f"{backend} {name}: {' '.join(str(c) for c in colours)}; {source[0].split('blend from ')[1].split(':')[0]}")
        return colours, from_registers, result.stderr

    # One target: the measured word blends; the three controls do not.
    blended, used, _ = draw("blend word at +0xD0", 1, f"d0:{BLEND:x}")
    assert used and close(blended[0], BLENDED), f"blended draw read {blended[0]}, expected {BLENDED}"
    for name, spec in (("ENABLE clear", f"d0:{BLEND & ~0x40000000:x}"), ("word at +0xCC", f"cc:{BLEND:x}"),
                       ("word at +0xD4", f"d4:{BLEND:x}")):
        colours, used, _ = draw(name, 1, spec)
        assert used, f"{name}: the registers were not used"
        assert close(colours[0], SOURCE), f"{name}: read {colours[0]}, expected the unblended source {SOURCE}"
        assert not close(colours[0], blended[0]), f"{name}: same pixel as the blended draw"

    # Two targets: each target follows its own word.
    colours, used, _ = draw("two targets, both words", 2, f"d0:{BLEND:x},d4:{BLEND:x}")
    assert used and close(colours[0], BLENDED) and close(colours[1], BLENDED), colours
    colours, used, _ = draw("two targets, target 1 ENABLE clear", 2, f"d0:{BLEND:x},d4:0")
    assert used and close(colours[0], BLENDED) and close(colours[1], SOURCE), colours
    # A word the decoder refuses for the bound second target: the registers are not used at all, so
    # the pipeline draws as it did before this path existed, and the refusal is logged.
    colours, used, log = draw("two targets, target 1 refused", 2, f"d0:{BLEND:x},d4:{REFUSED:x}")
    assert not used, "a block with a refused target supplied the blend state"
    assert "colour-blend registers not used: BOTH_SRC_ALPHA" in log and "refused targets 0x2" in log, log[-600:]
    assert all(close(c, SOURCE) for c in colours), f"refused block drew {colours}, expected both targets unblended"
    print(f"startup_slot57_blend_draw: PASS on {backend} (blended {blended[0]}, 3 single-target controls, per-target words, "
          f"refused second target leaves the registers unused; write mask not covered)")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
