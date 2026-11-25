"""Heap (bindless) textures are uploaded: X2W_HEAP_TEXTURE_UPLOAD=1 (src/startup_stubs/d3d12_draw.inl, LOCAL textures).

A heap draw (manifest `heap 1`, tests/startup_draw_uav.cpp mode heaptex) reads texel (x, y) of an 8x4 RGBA8 texture
that only the title's view heap references (no table binds it); its title memory holds (10 + 20x, 30 + 40y, 7, 255).
With the switch the draw returns those texels for columns 0 and 1 and the log names the heap texture upload. Negative
control: without the switch the texture is never uploaded (texture_view(..., prepare = false)) and the texels are not
those values.

With the layout check (third argument): entry 0 is a memory SRV (slot 70) of a console-swizzled BC1 surface written at
addrlib's addresses (layout 0x105, 9 mips; tests/gfx10_layout_check.cpp --write-tiled bc1_256_9mips_4k_s --addrlib),
so X2W_MEMORY_SRV_DETILE reaches a heap draw. The shader checks all 4096 blocks of mip 0. With both switches every
block matches. With DETILE alone (control) the texture is published but never uploaded, so the blocks mismatch:
DETILE needs HEAP_TEXTURE_UPLOAD for heap draws.

Heap draws need the console's 32-byte view slots: with another increment (WARP) the draws are skipped and the check
reports 77.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from startup_draw_uav_check import run_mode

CLEAR = (0, 0, 255, 255)


def expected():
    return bytes(c for y in range(4) for x in range(8)
                 for c in ((10 + 20 * x, 30 + 40 * y, 7, 255) if x < 2 else CLEAR))


def expected_detile():
    return bytes(c for y in range(4) for x in range(8) for c in ((255, 255, 0, 255) if x < 2 else CLEAR))


def surface(directory, layout_check):
    path = directory / "bc1_256_9mips_4k_s.bin"
    result = subprocess.run([layout_check, "--write-tiled", "bc1_256_9mips_4k_s", path, "--addrlib"], capture_output=True, text=True)
    if result.returncode == 0:
        return path, "addrlib"
    if result.returncode != 77:
        raise AssertionError(f"--write-tiled --addrlib failed ({result.returncode}): {result.stdout}{result.stderr}")
    subprocess.run([layout_check, "--write-tiled", "bc1_256_9mips_4k_s", path], check=True, capture_output=True)
    return path, "gfx10_layout.h (plumbing only: built without addrlib)"


def check(directory, consumer, backend, layout_check=None):
    results = {}
    for mode in ("heaptex", "heaptex-off"):
        got = run_mode(directory, consumer, backend, mode)
        if got is None:
            print("no existing-heap support or no adapter: skip")
            return 77
        results[mode] = got
    _, image, log = results["heaptex"]
    increment = int(re.search(r"^view increment (\d+)$", log, re.M).group(1))
    if increment != 32:
        if "heap draw skipped: descriptor increment is not 32" not in log:
            raise AssertionError(f"increment {increment}: the heap draw was not skipped")
        print(f"{backend}: heap texture upload NOT exercised (view increment {increment}; heap draws skipped as required)")
        return 77
    if image != expected():
        raise AssertionError(f"switch on: pixels {image.hex()}\n expected {expected().hex()}")
    for needle in ("X2W_HEAP_TEXTURE_UPLOAD=1", "x2w-draw heap textures: frame", "x2w-draw-rt first heap draw"):
        if needle not in log:
            raise AssertionError(f"switch on: log lacks {needle!r}\n{log[-4000:]}")
    _, off_image, off_log = results["heaptex-off"]
    if off_image[:8] == expected()[:8] or "x2w-draw heap textures" in off_log or "X2W_HEAP_TEXTURE_UPLOAD" in off_log:
        raise AssertionError(f"switch off: the heap texture was uploaded: {off_image[:16].hex()}")
    print(f"{backend}: heap-only texture uploaded and sampled (16 texels exact); switch off: not uploaded "
          f"(columns 0-1 read {off_image[:8].hex()})")
    if not layout_check:
        return 0
    path, source = surface(directory, layout_check)
    extra = {"X2W_TEST_HEAPTEX_SURFACE": str(path)}
    _, image, log = run_mode(directory, consumer, backend, "heaptex-detile", extra)
    if image != expected_detile() or "x2w-memsrv detiled memory SRV" not in log or "x2w-draw heap textures: frame" not in log:
        raise AssertionError(f"detile + heap upload: pixels {image[:64].hex()} (expected every block to match)\n{log[-3000:]}")
    _, control, control_log = run_mode(directory, consumer, backend, "heaptex-detile-noupload", extra)
    if control[:8] == expected_detile()[:8] or "x2w-memsrv detiled memory SRV" not in control_log:
        raise AssertionError(f"detile without heap upload: pixels {control[:16].hex()} (expected mismatches)")
    print(f"{backend}: memory SRV 0x105 BC1 9 mips through the heap: all 4096 mip-0 blocks match with DETILE + "
          f"HEAP_TEXTURE_UPLOAD ({source}); DETILE alone: published, not uploaded, mismatches")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
