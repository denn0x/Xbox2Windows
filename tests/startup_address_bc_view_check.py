"""Draw 2×2 4 KiB tiled BC3/BC7 textures from guest addresses, including swapped tiles."""
from pathlib import Path
import struct
import sys
import tempfile

from bc7_draw_check import make_block as bc7_block
from bc7_tiles_check import block_index
from startup_address_srv_check import draw_texture

WIDTH, HEIGHT = 128, 128


def bc3_block(x, y):
    # Literal DXT5 endpoint selection, avoiding interpolation tolerances.
    red, green, blue = bool(x & 16), bool(y & 16), bool((x ^ y) & 1)
    alpha = (7 * x + 13 * y) % 256
    color = (0xf800 if red else 0) | (0x07e0 if green else 0) | (0x001f if blue else 0)
    block = struct.pack("<BB6xHHI", alpha, 0, color, 0, 0)
    return block, bytes((255 * red, 255 * green, 255 * blue, alpha)) * 16


def make_surface(encode):
    tiled = bytearray(4 * 4096)
    rgba = bytearray(128 * 128 * 4)
    for y in range(32):
        for x in range(32):
            block, colors = encode(x, y)
            tile = (y // 16) * 2 + x // 16
            start = tile * 4096 + 16 * block_index(x % 16, y % 16)
            tiled[start:start + 16] = block
            for row in range(4):
                start = 4 * ((y * 4 + row) * 128 + x * 4)
                rgba[start:start + 16] = colors[row * 16:row * 16 + 16]
    return tiled, rgba


def check(directory, consumer, backend, fmt):
    tiled, rgba = make_surface(bc3_block if fmt == "BC3" else bc7_block)
    source = directory / "tiled.bin"
    source.write_bytes(tiled)
    hlsl = ("Texture2D<float4> t : register(t0);\n"
            "float4 main(float4 p : SV_Position) : SV_Target {\n"
            f"  return t.Load(int3(uint(p.x) % {WIDTH}, uint(p.y), 0));\n}}\n")
    modes = {"X2W_TEST_BC_VIEW": str(source)}
    if fmt == "BC3":
        modes["X2W_TEST_BC3"] = "1"
    actual = draw_texture(directory, consumer, backend, hlsl, modes)
    if actual is None:
        return 77
    if len(actual) != WIDTH * 4 * HEIGHT * 4:
        raise AssertionError(f"{fmt} readback length {len(actual)}")
    different = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            normal = 4 * (y * 128 + x)
            swapped = 4 * (y * 128 + (x ^ 64))
            different += rgba[normal:normal + 4] != rgba[swapped:swapped + 4]
            for panel, source_index in enumerate((normal, swapped, swapped, normal)):
                pos = 4 * ((y * 4 + panel) * WIDTH + x)
                pixel = actual[pos:pos + 4]
                expect = rgba[source_index:source_index + 4]
                if pixel != expect:
                    raise AssertionError(f"{fmt} panel {panel} ({x},{y}) actual {pixel.hex()} expected {expect.hex()}")
    if different == 0:
        raise AssertionError("Swapping the tiles did not change the reference image")
    print(f"{backend} {fmt}: {4 * WIDTH * HEIGHT} RGBA pixels exact; exchanged tiles differ in {different}/{WIDTH * HEIGHT}")
    return 0


if __name__ == "__main__":
    for fmt in ("BC3", "BC7"):
        with tempfile.TemporaryDirectory() as temporary:
            result = check(Path(temporary), *sys.argv[1:], fmt)
            if result:
                sys.exit(result)
