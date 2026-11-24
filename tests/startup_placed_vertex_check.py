"""Check live placed-buffer vertex views through a translated draw and a queue fence."""
from pathlib import Path
import sys
import tempfile

from startup_address_srv_check import draw_texture


def check(directory, consumer, backend):
    shader = """ByteAddressBuffer vertices : register(t0);
float4 main(float4 p : SV_Position) : SV_Target {
    return float4(float(vertices.Load(0)) / 255.0, 0, 0, 1);
}
"""
    actual = draw_texture(directory, consumer, backend, shader, {"X2W_TEST_PLACED_VERTEX": "1"}, binding="srv 0 buf vb 0")
    if actual is None:
        return 77
    first = (directory / "pixels.first").read_bytes()
    for name, image, low, high, reused in (("initial", first, 41, 135, 41), ("replaced", actual, 49, 153, 179)):
        row = b"".join(bytes((red, 0, 0, 255)) for red in (low, high, 0, reused) * 2)
        if image != row * 4:
            raise AssertionError(f"{backend} {name}: {image[:32].hex()} expected {row.hex()}")
    print(f"{backend}: 64 pixels exact across registration, last DWORD, rejected range, release and replacement")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
