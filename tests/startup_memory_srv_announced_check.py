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


def run(directory, consumer, backend, switch):
    modes = {"X2W_TEST_ANNOUNCED_SRV": "1", "X2W_TRAP_LOG": str(directory / "trap.log")}
    if switch:
        modes["X2W_MEMORY_SRV_ANNOUNCED"] = "1"
    pixels = draw_texture(directory, consumer, backend, HLSL, modes)
    return pixels, (directory / "trap.log").read_text(errors="replace") if pixels is not None else ""


def check(root, consumer, backend):
    on_dir, off_dir = root / "on", root / "off"
    on_dir.mkdir()
    off_dir.mkdir()
    actual, log = run(on_dir, consumer, backend, True)
    if actual is None:
        return 77
    if actual != expected_on():
        raise AssertionError(f"switch on: actual {actual.hex()}\n expected {expected_on().hex()}")
    if "X2W_MEMORY_SRV_ANNOUNCED=1" not in log:
        raise AssertionError("switch on: no X2W_MEMORY_SRV_ANNOUNCED line")
    refusals = [line for line in log.splitlines() if "Device::slot70" in line and "reason=" in line]
    if len(refusals) != 2 or not all("layout=0x105" in line for line in refusals):
        raise AssertionError("switch on: expected the two unannounced calls refused:\n" + "\n".join(refusals))
    off, off_log = run(off_dir, consumer, backend, False)
    if off != bytes(4 * 8 * 4):
        raise AssertionError(f"switch off: expected null views (all zero), got {off.hex()}")
    off_refusals = [line for line in off_log.splitlines() if "Device::slot70" in line and "reason=" in line]
    if len(off_refusals) != 8:
        raise AssertionError(f"switch off: {len(off_refusals)} refusal lines, expected 8")
    print(f"{backend}: announced 0x105 views drawn (2 mips, 32 RGBA pixels exact), unannounced refused; "
          "switch off: 8 refusals, null views")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
