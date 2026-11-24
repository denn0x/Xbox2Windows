"""Draw through the startup shim using explicit, default and incompatible texture views."""
from pathlib import Path
import sys
import tempfile

from startup_address_srv_check import draw_texture


def check(directory, consumer, backend):
    source = """Texture1D<float4> tex : register(t0);
float4 main(float4 p : SV_Position) : SV_Target {
    float4 value = tex.Load(int2(uint(p.x) % 4, 0));
    return float4(value.r, value.a, 0, 1);
}"""
    actual = draw_texture(directory, consumer, backend, source, {"X2W_TEST_1D_SRV": "1"}, "tex1d")
    if actual is None:
        return 77
    row = [(7 + 3*i, 24 + 56*i, 0, 255) for i in range(3)] + [(0, 0, 0, 255)]
    expected = bytes(value for pixel in row * 8 for value in pixel)
    if actual != expected:
        raise AssertionError(f"Expected {expected.hex()}, got {actual.hex()}")
    print(f"{backend}: explicit/default 1D views and a mismatched 2D view, all 32 pixels matched")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
