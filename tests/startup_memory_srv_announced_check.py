"""Memory SRVs of console-swizzled (layout 0x105) textures whose memory the XG stand-in filled.

X2W_MEMORY_SRV_ANNOUNCED=1: views of announced addresses are drawn from the linear (desktop footprint) bytes,
both mips; a texture with the same description and bytes that XG did not announce is refused. Without the
switch every call is refused and the draws read a null view (negative control).
"""
from pathlib import Path
import sys
import tempfile

from startup_address_srv_check import draw_texture

HLSL = ("Texture2D<float4> t : register(t0);\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "  uint2 q = uint2(p.xy);\n"
        "  if (q.y < 2) return t.Load(int3(q.x % 2, q.y, 0));\n"
        "  return t.Load(int3(0, 0, 1));\n}\n")


def texel(which, mip, x, y):
    if mip:
        return (9, 99, 199, 255) if which == 1 else (5, 6, 7, 255)
    return (100 + 20 * x + 50 * y, 50, 150, 255) if which == 1 else (10 + 40 * x + 80 * y, 200, 30, 255)


def expected_on():
    # Columns publish A B C A B C A A; C is refused, so the previous view (B) stays.
    effective = (0, 1, 1, 0, 1, 1, 0, 0)
    return bytes(c for y in range(4) for x in range(8)
                 for c in (texel(effective[x], 0, x % 2, y) if y < 2 else texel(effective[x], 1, 0, 0)))


def run(directory, consumer, backend, switch, census=False):
    # draw_texture sets X2W_D3D12_DEVICE_NOOP=1 (as title runs do): device slot 61, which the test calls last, is
    # the no-op unless the census replaces it.
    modes = {"X2W_TEST_ANNOUNCED_SRV": "1", "X2W_TRAP_LOG": str(directory / "trap.log")}
    if switch:
        modes["X2W_MEMORY_SRV_ANNOUNCED"] = "1"
    if census:
        modes["X2W_TEXTURE_CENSUS"] = "1"
        modes["X2W_TEXTURE_CENSUS_EVERY"] = "1"
    pixels = draw_texture(directory, consumer, backend, HLSL, modes)
    return pixels, (directory / "trap.log").read_text(errors="replace") if pixels is not None else ""


def last_census(log, prefix):
    lines = [line for line in log.splitlines() if line.startswith("x2w-texcensus " + prefix)]
    return lines[-1] if lines else ""


def check_census(log, label, slot70, refusal_classes, draws):
    """The last cumulative X2W_TEXTURE_CENSUS lines of a run (X2W_TEXTURE_CENSUS_EVERY=1)."""
    want_calls = f"slot70 calls 8 (threshold, cumulative): {slot70}"
    if want_calls not in last_census(log, "slot70 calls"):
        raise AssertionError(f"{label}: last slot-70 census line {last_census(log, 'slot70 calls')!r}, expected {want_calls!r}")
    lines = log.splitlines()
    head = max(i for i, line in enumerate(lines) if line.startswith("x2w-texcensus slot70 refusals"))
    classes = [line.removeprefix("x2w-texcensus   ") for line in lines[head + 1:head + 1 + len(refusal_classes)]]
    if classes != refusal_classes:
        raise AssertionError(f"{label}: refusal classes {classes}, expected {refusal_classes}")
    want_draws = f"draw textures binding (cumulative): {draws}"
    if want_draws not in last_census(log, "draw textures"):
        raise AssertionError(f"{label}: last draw census line {last_census(log, 'draw textures')!r}, expected {want_draws!r}")
    words = " ".join(f"{0x61000000 + i:08x}" for i in range(16))
    slot61 = [line for line in lines if line.startswith("x2w-texcensus Device::slot61")]
    if len(slot61) != 1 or "r8=0x5150 r9=0x1 words " + words not in slot61[0]:
        raise AssertionError(f"{label}: slot-61 lines {slot61}")


def check(root, consumer, backend):
    on_dir, off_dir, census_dir = root / "on", root / "off", root / "census"
    on_dir.mkdir()
    off_dir.mkdir()
    census_dir.mkdir()
    actual, log = run(on_dir, consumer, backend, True, census=True)
    if actual is None:
        return 77
    if actual != expected_on():
        raise AssertionError(f"switch on: actual {actual.hex()}\n expected {expected_on().hex()}")
    if "X2W_MEMORY_SRV_ANNOUNCED=1" not in log:
        raise AssertionError("switch on: no X2W_MEMORY_SRV_ANNOUNCED line")
    refusals = [line for line in log.splitlines() if "Device::slot70" in line and "reason=" in line]
    if len(refusals) != 2 or not all("layout=0x105" in line for line in refusals):
        raise AssertionError("switch on: expected the two unannounced calls refused:\n" + "\n".join(refusals))
    unannounced = "2 x unsupported_description dim 3 layout 0x105 fmt 28 mips>1 not-announced nonzero"
    check_census(log, "switch on, census", "linear 0, announced-linear 6, tiled/bc64 0, buffer 0, stand-in buffer 0, refused 2",
                 [unannounced], "real 8 (linear memory srv 8, tiled address 0, other 0), null; real by format 28:8")
    # Census without the announced switch: all 8 calls refused (6 at announced addresses), 8 null bindings.
    census_off, census_log = run(census_dir, consumer, backend, False, census=True)
    if census_off != bytes(4 * 8 * 4):
        raise AssertionError(f"census, switch off: expected null views (all zero), got {census_off.hex()}")
    check_census(census_log, "census, switch off", "linear 0, announced-linear 0, tiled/bc64 0, buffer 0, stand-in buffer 0, refused 8",
                 ["6 x unsupported_description dim 3 layout 0x105 fmt 28 mips>1 announced nonzero", unannounced],
                 "real 0 (linear memory srv 0, tiled address 0, other 0), null cause2:8; real by format")
    off, off_log = run(off_dir, consumer, backend, False)
    if off != bytes(4 * 8 * 4):
        raise AssertionError(f"switch off: expected null views (all zero), got {off.hex()}")
    off_refusals = [line for line in off_log.splitlines() if "Device::slot70" in line and "reason=" in line]
    if len(off_refusals) != 8:
        raise AssertionError(f"switch off: {len(off_refusals)} refusal lines, expected 8")
    if "x2w-texcensus" in off_log:
        raise AssertionError("census off: x2w-texcensus lines logged")
    print(f"{backend}: announced 0x105 views drawn (2 mips, 32 RGBA pixels exact), unannounced refused; "
          "switch off: 8 refusals, null views; census counts exact (8 real / 8 null, refusal classes, slot 61), "
          "census off: no census lines, slot 61 no-op")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
