"""Heap (bindless) textures are uploaded: X2W_HEAP_TEXTURE_UPLOAD=1 (src/startup_stubs/d3d12_draw.inl, LOCAL textures).

A heap draw (manifest `heap 1`, tests/startup_draw_uav.cpp mode heaptex) reads texel (x, y) of an 8x4 RGBA8 texture
that only the title's view heap references (no table binds it); its title memory holds (10 + 20x, 30 + 40y, 7, 255).
With the switch the draw returns those texels for columns 0 and 1 and the log names the heap texture upload. Negative
control: without the switch the texture is never uploaded (texture_view(..., prepare = false)) and the texels are not
those values. Heap draws need the console's 32-byte view slots: with another increment (WARP) both draws are skipped
and the check reports 77.
"""
from pathlib import Path
import re
import sys
import tempfile

from startup_draw_uav_check import run_mode


def expected():
    return bytes(c for y in range(4) for x in range(8)
                 for c in ((10 + 20 * x, 30 + 40 * y, 7, 255) if x < 2 else (0, 0, 255, 255)))


def check(directory, consumer, backend):
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
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
