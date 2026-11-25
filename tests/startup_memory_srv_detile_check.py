"""Memory SRVs of console-swizzled textures detiled by the GFX10 layout (X2W_MEMORY_SRV_DETILE=1, LOCAL textures).

Each case writes a synthetic surface with tests/gfx10_layout_check.cpp --write-tiled: every element carries a pattern
of its own coordinates and is stored at the address AMD addrlib gives (--addrlib; without addrlib in the build, at
gfx10_layout.h's own address, which checks the plumbing only and is reported as such). One memory SRV of the bytes
(address not announced by the XG stand-in) is drawn on a 64x64 target whose pixel shader loads every element of
every level and slice, compares it with the pattern, and writes red = no mismatch, green = elements checked.

Cases: the run 102 refusal classes (layout 0x105 BC1 mip chain, 0x10a BC1 array mip chain, 0x100 R16G16B16A16_FLOAT)
and the 17^3 RGBA16F 3D LUT (0x105, 5 mips).
Negative controls: the switch off (the call is refused, the view stays null, mismatches); the 0x105 bytes described as
0x109 (wrong layout, mismatches). Declined (DECLINED): refused with the switch on; the census class, the decline
counters and the class's first-call line name why detiling declined.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from startup_address_srv_check import draw_texture

SIDE = 64
PIXELS = SIDE * SIDE
BC1_UNORM, BC3_UNORM, RGBA16F = 71, 77, 10
TEX2D, TEX3D = 3, 4

# name, layout, dimension, format, width, height, depth or array size, mips (pixels; BC shapes are 4x4 blocks in the layout check)
CASES = [
    ("bc1_256_9mips_4k_s", 0x105, TEX2D, BC1_UNORM, 256, 256, 1, 9),
    ("bc1_256x128x2_8mips_64k_d", 0x10A, TEX2D, BC1_UNORM, 256, 128, 2, 8),
    # BC3 0x105 256x256 is a tiled_bc_srv description; the tiled paths take single mips only, so the chain is detiled
    ("bc3_256_9mips_4k_s", 0x105, TEX2D, BC3_UNORM, 256, 256, 1, 9),
    ("rgba16f_17cubed_3d_5mips", 0x105, TEX3D, RGBA16F, 17, 17, 17, 5),
    ("rgba16f_100x60_3mips_linear", 0x100, TEX2D, RGBA16F, 100, 60, 1, 3),
]
# Refused with the switch on, and the decline the census names.
DECLINED = [
    (("bc1_132_8mips_4k_s", 0x105, TEX2D, BC1_UNORM, 132, 132, 1, 8), "bc mip chain"),
]


def levels(case):
    """(mip, slice, width, height, depth) in elements, D3D extents (floor per level; BC blocks round up)."""
    _, _, dimension, fmt, width, height, depth, mips = case
    out = []
    for mip in range(mips):
        w, h = max(1, width >> mip), max(1, height >> mip)
        if fmt in (BC1_UNORM, BC3_UNORM):
            w, h = (w + 3) // 4, (h + 3) // 4
        if dimension == TEX3D:
            out.append((mip, 0, w, h, max(1, depth >> mip)))
        else:
            out.extend((mip, s, w, h, 1) for s in range(depth))
    return out


def view_kind(case):
    return "tex3d" if case[2] == TEX3D else "tex2da" if case[6] > 1 else "tex2d"


def shader(case):
    kind = view_kind(case)
    bc = case[3] in (BC1_UNORM, BC3_UNORM)
    bc3 = case[3] == BC3_UNORM
    declaration = {"tex2d": "Texture2D<float4>", "tex2da": "Texture2DArray<float4>", "tex3d": "Texture3D<float4>"}[kind]
    body = []
    for mip, s, w, h, d in levels(case):
        count = w * h * d
        if kind == "tex3d":
            load = f"t.Load(int4(x, y, z, {mip}))"
        elif kind == "tex2da":
            load = f"t.Load(int4({'4 * x, 4 * y' if bc else 'x, y'}, {s}, {mip}))"
        else:
            load = f"t.Load(int3({'4 * x, 4 * y' if bc else 'x, y'}, {mip}))"
        if bc:
            alpha = f" || uint(round(c.a * 255)) != {16 * (mip + 8 * s) + 5}u" if bc3 else ""
            test = (f"float4 c = {load}; uint id = (uint(round(c.r * 31)) << 11) | (uint(round(c.g * 63)) << 5) | "
                    f"uint(round(c.b * 31)); if (id != (x | (y << 6) | ({mip + 8 * s}u << 12)){alpha}) bad++;")
        else:
            test = f"if (any({load} != float4(x, y, z, {mip + 1}))) bad++;"
        body.append(f"  [loop] for (uint i = p; i < {count}u; i += {PIXELS}u) {{ uint x = i % {w}u, y = (i / {w}u) % {h}u, "
                    f"z = i / {w * h}u; {test} n++; }}")
    return (f"{declaration} t : register(t0);\n"
            "float4 main(float4 q : SV_Position) : SV_Target {\n"
            f"  uint p = uint(q.y) * {SIDE}u + uint(q.x), bad = 0, n = 0;\n" + "\n".join(body) + "\n"
            "  return float4(bad ? 0 : 1, n / 255.0, 0, 1);\n}\n")


def write_surface(layout_check, case, path):
    result = subprocess.run([layout_check, "--write-tiled", case[0], path, "--addrlib"], capture_output=True, text=True)
    if result.returncode == 0:
        return "addrlib"
    if result.returncode != 77:
        raise AssertionError(f"{case[0]}: --write-tiled --addrlib failed ({result.returncode}): {result.stdout}{result.stderr}")
    subprocess.run([layout_check, "--write-tiled", case[0], path], check=True, capture_output=True)
    return "gfx10_layout.h (plumbing only: built without addrlib)"


def run(directory, consumer, backend, case, surface, switch, layout=None):
    directory.mkdir()
    name, case_layout, dimension, fmt, width, height, depth, mips = case
    modes = {"X2W_TEST_DETILE_SRV": str(surface), "X2W_TRAP_LOG": str(directory / "trap.log"),
             "X2W_TEST_DETILE_DESC": f"{layout or case_layout:x} {dimension} {fmt} {width} {height} {depth} {mips}",
             "X2W_TEXTURE_CENSUS": "1", "X2W_TEXTURE_CENSUS_EVERY": "1"}
    if switch:
        modes["X2W_MEMORY_SRV_DETILE"] = "1"
    pixels = draw_texture(directory, consumer, backend, shader(case), modes, view_kind(case))
    if pixels is None:
        return None
    if len(pixels) != PIXELS * 4:
        raise AssertionError(f"{name}: {len(pixels)} bytes read back, expected {PIXELS * 4}")
    bad = sum(1 for i in range(PIXELS) if pixels[4 * i] != 255)
    checked = sum(pixels[4 * i + 1] for i in range(PIXELS))
    return bad, checked, (directory / "trap.log").read_text(errors="replace")


def census_line(log, prefix):
    lines = [line for line in log.splitlines() if line.startswith("x2w-texcensus " + prefix)]
    return lines[-1] if lines else ""


def check(root, consumer, backend, layout_check):
    source = None
    for index, case in enumerate(CASES):
        name = case[0]
        surface = root / f"{name}.bin"
        source = write_surface(layout_check, case, surface)
        expected = sum(w * h * d for _, _, w, h, d in levels(case))
        result = run(root / f"on{index}", consumer, backend, case, surface, True)
        if result is None:
            return 77
        bad, checked, log = result
        if checked != expected:
            raise AssertionError(f"{name}: {checked} elements checked by the shader, expected {expected}")
        if bad:
            raise AssertionError(f"{name}: {bad} of {PIXELS} pixels saw a mismatching element\n" + "\n".join(
                line for line in log.splitlines() if "slot70" in line or "memsrv" in line))
        if f"layout 0x{case[1]:x}" not in log or "x2w-memsrv detiled memory SRV" not in log:
            raise AssertionError(f"{name}: no detiled memory SRV line")
        # Two calls, both detiled; one desktop texture (the second call reuses it).
        if "stand-in buffer 0, detiled 2, refused 0" not in census_line(log, "slot70 calls"):
            raise AssertionError(f"{name}: slot-70 census {census_line(log, 'slot70 calls')!r}")
        if "detiled 1)" not in census_line(log, "draw textures"):
            raise AssertionError(f"{name}: draw census {census_line(log, 'draw textures')!r}")
        print(f"{backend}: {name} layout 0x{case[1]:x}: {expected} elements, every one matches ({source})")
    # Negative control 1: the switch off. The calls are refused and the null view reads zeros.
    case = CASES[0]
    bad, checked, log = run(root / "off", consumer, backend, case, root / f"{case[0]}.bin", False)
    refusals = [line for line in log.splitlines() if "Device::slot70" in line and "reason=" in line]
    if not bad or len(refusals) != 2 or "x2w-memsrv detiled" in log:
        raise AssertionError(f"switch off: {bad} mismatching pixels, {len(refusals)} refusals (expected mismatches and 2)")
    # Negative control 2: the 0x105 bytes described as 0x109 (64 KiB standard; the fixture memory has 1 MiB of slack, so
    # the larger surface is readable): detiled, but with the wrong addresses.
    bad_layout, checked_layout, log = run(root / "wrong", consumer, backend, case, root / f"{case[0]}.bin", True, 0x109)
    if not bad_layout or "x2w-memsrv detiled memory SRV" not in log:
        raise AssertionError(f"wrong layout: {bad_layout} mismatching pixels (expected some), detile line "
                             f"{'x2w-memsrv detiled memory SRV' in log}")
    print(f"{backend}: controls: switch off: {bad} of {PIXELS} pixels mismatch, 2 refusals; "
          f"0x105 bytes as 0x109: {bad_layout} of {PIXELS} pixels mismatch")
    # Refusals that the switch declines name the reason (census class, decline counters, the class's first call).
    for case, decline in DECLINED:
        surface = root / f"{case[0]}.bin"
        write_surface(layout_check, case, surface)
        bad_declined, _, log = run(root / f"declined_{case[0]}", consumer, backend, case, surface, True)
        classes = [line for line in log.splitlines() if line.startswith("x2w-texcensus   2 x ")]
        counters = census_line(log, "detile declines of refused texture calls")
        first = [line for line in log.splitlines() if "new refusal class" in line]
        if (not bad_declined or "x2w-memsrv detiled" in log or not classes or f"detile: {decline}" not in classes[-1]
                or f"{decline} 2" not in counters or len(first) != 1 or f"detile: {decline};" not in first[0]):
            raise AssertionError(f"{case[0]}: declined control: {bad_declined} mismatches, classes {classes[-1:]}, counters {counters!r}, "
                                 f"first-call lines {first}")
        print(f"{backend}: {case[0]}: refused, detile declined ({decline}): {classes[-1].removeprefix('x2w-texcensus   ')}")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
