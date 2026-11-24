"""Check DSV binding (slot 50), view_slot<21, 2>, and stream DEPTH_STENCIL/DEPTH_STENCIL_FORMAT."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from startup_stream_check import container, gfx2hlsl
from startup_stream_draw_check import pixel, vop1, vop2


def vertex(z_bits):
    words = [vop2(0x1a, 1, 129, 0), vop2(0x1b, 1, 130, 1), vop2(0x1b, 2, 130, 0)]
    for v in (1, 2):
        words.extend([vop1(6, v, 256 + v), vop2(8, v, 244, v), vop2(3, v, 243, v)])
    words.extend([vop1(1, 3, 255), z_bits, vop1(1, 4, 242), 0xf80008cf, 0x04030201, 0xbf810000])
    return container(1, words)


def check(directory, consumer, backend):
    cache = directory / "cache"
    cache.mkdir()
    vs_front, vs_back, ps_green, ps_red, root, ps_empty = (
        directory / name for name in ("vs_front", "vs_back", "ps_green", "ps_red", "root", "ps_empty")
    )
    out_pos, out_neg, out_depthonly, out_readonly, out_rtv_depth, out_dsv_target, out_bounds = (
        directory / name for name in ("pixels_pos", "pixels_neg", "pixels_depthonly", "pixels_readonly",
                                      "pixels_rtv_as_depth", "pixels_dsv_as_target", "pixels_bounds")
    )
    vs_front.write_bytes(vertex(0x3e800000))  # z = 0.25f
    vs_back.write_bytes(vertex(0x3f400000))   # z = 0.75f
    ps_green.write_bytes(pixel(True))
    ps_red.write_bytes(pixel(False))
    ps_empty.write_bytes(container(0, [0xbf810000]))  # exports nothing: the depth-only pass declares no SV_Target0

    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1",
               X2W_D3D12_DEBUG="1", X2W_SHADER_CACHE=str(cache))
    base_args = [consumer, str(vs_front), str(vs_back), str(ps_green), str(ps_red), str(root), backend, str(ps_empty)]
    req = subprocess.run(base_args, env=env, capture_output=True, text=True)
    if backend == "hardware" and req.returncode == 77:
        print("No D3D12 feature-level 12 hardware adapter")
        return 77
    if req.returncode:
        raise RuntimeError(f"request run failed on {backend}: {req.returncode}\n{req.stderr}")
    subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], env=env, check=True)

    run = subprocess.run(base_args + [str(out_pos), str(out_neg), str(out_depthonly), str(out_readonly),
                                      str(out_rtv_depth), str(out_dsv_target), str(out_bounds)],
                         env=env, capture_output=True, text=True)
    print(run.stdout, end="")
    for line in run.stderr.splitlines():
        if "debug layer:" in line or "x2w-d3d12-debug " in line:
            print(line)
    if run.returncode or "-> drawn;" not in run.stderr:
        raise RuntimeError(f"depth draw failed on {backend}: {run.returncode}\n{run.stderr}")
    assert not re.search(r"x2w-d3d12-debug.*sev=[012]\b", run.stderr), run.stderr

    green = bytes((0, 255, 0, 255))
    red = bytes((255, 0, 0, 255))
    clear = bytes((0, 0, 255, 255))
    expected_pos = (green * 4 + clear * 4) * 4
    expected_neg = (red * 4 + clear * 4) * 4
    expected_depthonly = clear * 32

    assert out_pos.read_bytes() == expected_pos, f"pos: {out_pos.read_bytes().hex()}"
    assert out_neg.read_bytes() == expected_neg, f"neg: {out_neg.read_bytes().hex()}"
    assert out_depthonly.read_bytes() == expected_depthonly, f"depthonly: {out_depthonly.read_bytes().hex()}"
    # A read-only DSV tests but never writes: the far red triangle is not occluded by the near green one.
    assert out_readonly.read_bytes() == expected_neg, f"readonly: {out_readonly.read_bytes().hex()}"
    # Wrong descriptor kind and DepthBoundsTestEnable refuse the draw instead of drawing something else.
    for name, path in (("rtv as depth", out_rtv_depth), ("dsv as target", out_dsv_target), ("depth bounds", out_bounds)):
        assert path.read_bytes() == clear * 32, f"{name}: {path.read_bytes().hex()}"
    print(f"{backend}: depth LESS occluded far red (green), DepthEnable=FALSE overwrote (red), count=0 depth-only occluded far red (clear)")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
