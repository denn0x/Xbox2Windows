"""Decoded fixed-function state on translated slot 57 draws (src/startup_stubs/pipeline_state_decode.h,
apply_draw_state in d3d12_draw.inl), switched by X2W_DRAW_STATE_DEPTH / _CULL / _BLEND.

usage: startup_draw_state_check.py <startup_draw_state> <warp|hardware>

The same program runs five times: switches off (the default), all on, on without X2W_DRAW_STATE_DEPTH_CLEAR, all
on with X2W_DRAW_STATE_DEPTH_UNCLEARED=1, and all on with X2W_DRAW_STATE=0. List slot 51 (ClearDepthStencilView)
is forwarded only with _DEPTH_CLEAR. A depth target that no forwarded depth clear initialised gets no depth or
stencil test (run 100: the test against undefined depth hid 2D draws); _DEPTH_UNCLEARED=1 keeps the test, and the
"uncleared" case (desktop depth 0, LESS) then loses its near triangle (blue).
Off and master-off must draw as before: no depth test (the far red triangle drawn second covers the near
green one), no culling, every channel written. On: DB_DEPTH_CONTROL LESS + write keeps the near green,
PA_SU_SC_MODE_CNTL culls (FACE 0 taken as counter-clockwise front faces: the test triangle, NDC (-1,-1)
(3,-1) (-1,3), is counter-clockwise on screen, so CULL_FRONT and FACE 1 + CULL_BACK remove it and CULL_BACK
keeps it), CB_TARGET_MASK 0x3 keeps the blue of
the clear. Negative control: a DB_DEPTH_CONTROL the decoder refuses (bits 30-31) draws as before in every
mode. The left half (scissor) is drawn, the right half must stay the clear colour.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from startup_stream_check import gfx2hlsl
from startup_stream_depth_check import vertex
from startup_stream_draw_check import pixel

GREEN, RED, BLUE, CYAN = (0, 255, 0, 255), (255, 0, 0, 255), (0, 0, 255, 255), (0, 255, 255, 255)
SWITCHES = ("X2W_DRAW_STATE", "X2W_DRAW_STATE_DEPTH", "X2W_DRAW_STATE_CULL", "X2W_DRAW_STATE_BLEND", "X2W_DRAW_STATE_CULL_FLIP",
            "X2W_DRAW_STATE_DEPTH_CLEAR", "X2W_DRAW_STATE_DEPTH_UNCLEARED")
GROUPS = {"X2W_DRAW_STATE_DEPTH": "1", "X2W_DRAW_STATE_CULL": "1", "X2W_DRAW_STATE_BLEND": "1"}
MODES = {
    "off": {},
    "on": dict(GROUPS, X2W_DRAW_STATE_DEPTH_CLEAR="1"),
    "on-noclear": GROUPS,
    "on-uncleared": dict(GROUPS, X2W_DRAW_STATE_DEPTH_CLEAR="1", X2W_DRAW_STATE_DEPTH_UNCLEARED="1"),
    "master-off": dict(GROUPS, X2W_DRAW_STATE="0", X2W_DRAW_STATE_DEPTH_CLEAR="1"),
}
BEFORE = {"uncleared": GREEN, "guest_clear_stencil_only": GREEN, "depth": RED, "depth_refused": RED, "no_state": RED,
          "cull_back": GREEN, "cull_front": GREEN, "cull_back_face1": GREEN, "mask_rg": GREEN, "guest_clear": GREEN,
          "guest_clear0": GREEN}
DECODED = dict(BEFORE, depth=GREEN, cull_front=BLUE, cull_back_face1=BLUE, mask_rg=CYAN, guest_clear0=BLUE)
EXPECTED = {
    "off": BEFORE,
    "on": DECODED,
    # Nothing forwards the guest clears, so no depth target counts as initialised and every depth test stays off.
    "on-noclear": dict(DECODED, depth=RED, guest_clear0=GREEN),
    # Negative control (run 100): testing against the never-cleared depth of 0 hides the near triangle.
    "on-uncleared": dict(DECODED, uncleared=BLUE, guest_clear_stencil_only=BLUE),
    "master-off": BEFORE,
}
CLEARS = ["flags 0x2 depth 1 ", "flags 0x1 depth 1 ", "flags 0x1 depth 1 ", "flags 0x1 depth 1 ", "flags 0x1 depth 0 "]


def left(pixels, name):
    assert len(pixels) == 8 * 4 * 4, f"{name}: {len(pixels)} bytes"
    drawn = None
    for y in range(4):
        for x in range(8):
            texel = tuple(pixels[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4])
            if x >= 4:
                assert texel == BLUE, f"{name}: outside the scissor ({x}, {y}) is {texel}"
            elif drawn is None:
                drawn = texel
            else:
                assert texel == drawn, f"{name}: ({x}, {y}) is {texel}, (0, 0) is {drawn}"
    return drawn


def check(directory, program, backend):
    cache = directory / "cache"
    cache.mkdir()
    vs_near, vs_far, ps_green, ps_red, root = (directory / n for n in ("vs_near", "vs_far", "ps_green", "ps_red", "root"))
    vs_near.write_bytes(vertex(0x3e800000))  # z = 0.25
    vs_far.write_bytes(vertex(0x3f400000))   # z = 0.75
    ps_green.write_bytes(pixel(True))
    ps_red.write_bytes(pixel(False))
    base = {k: v for k, v in os.environ.items() if k not in SWITCHES}
    base.update(X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1", X2W_SHADER_CACHE=str(cache))
    base.setdefault("X2W_D3D12_DEBUG", "1")
    args = [str(program), str(vs_near), str(vs_far), str(ps_green), str(ps_red), str(root), backend]
    request = subprocess.run(args, env=base, capture_output=True, text=True)
    if backend == "hardware" and request.returncode == 77:
        print("No D3D12 feature-level 12 hardware adapter")
        return 77
    if request.returncode:
        raise RuntimeError(f"request run failed: {request.returncode}\n{request.stderr}")
    subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], env=base, check=True)

    for mode, switches in MODES.items():
        out = directory / mode
        out.mkdir()
        run = subprocess.run(args + [str(out)], env=dict(base, **switches), capture_output=True, text=True)
        if run.returncode or "-> drawn;" not in run.stderr:
            raise RuntimeError(f"{mode}: draw failed on {backend}: {run.returncode}\n{run.stderr[-3000:]}")
        # The cases clear depth to 0 and to 1, so the debug layer's clear-value performance warnings (820 mismatching
        # clear value, 821 no clear value at creation) are expected; every other warning or error fails.
        errors = [line for line in run.stderr.splitlines()
                  if re.search(r"x2w-d3d12-debug.*sev=[012]\b", line) and not re.search(r"sev=2 id=82[01] ", line)]
        assert not errors, f"{mode}: debug layer: {errors[:4]}"
        on = mode.startswith("on")
        expected = EXPECTED[mode]
        decoded_psos = run.stderr.count(", decoded state")
        switch_line = [line for line in run.stderr.splitlines() if "x2w-draw-state switches:" in line]
        assert switch_line, f"{mode}: no switch line"
        assert ("depth 1 cull 1" in switch_line[0]) == on, f"{mode}: {switch_line[0]}"
        clears = [line for line in run.stderr.splitlines() if "x2w-render ClearDepthStencilView" in line]
        if mode in ("on", "on-uncleared"):
            assert len(clears) == len(CLEARS) and all(c in line for c, line in zip(CLEARS, clears)), clears
        else:
            assert not clears, f"{mode}: slot 51 forwarded: {clears}"
        assert (decoded_psos > 0) == on, f"{mode}: {decoded_psos} pipelines with decoded state"
        assert "decoded state refused" not in run.stderr, f"{mode}: a decoded PSO failed creation"
        got = {name: left((out / name).read_bytes(), f"{mode} {name}") for name in expected}
        print(f"{backend} {mode}: " + ", ".join(f"{n} {c}" for n, c in got.items()) + f"; PSOs with decoded state {decoded_psos}")
        wrong = {name: (got[name], colour) for name, colour in expected.items() if got[name] != colour}
        assert not wrong, f"{mode}: drew / expected {wrong}"
    print(f"startup_draw_state: PASS on {backend} (off and X2W_DRAW_STATE=0 draw as before; on: depth LESS keeps near green, "
          f"CULL_FRONT and FACE 1 + CULL_BACK cull the counter-clockwise triangle, CULL_BACK keeps it, target mask 0x3 keeps "
          f"blue; refused depth word draws as before; slot 51 depth clear forwarded only with X2W_DRAW_STATE_DEPTH_CLEAR=1, "
          f"a clear to 0 makes LESS fail; no depth test against a target no forwarded depth clear initialised, "
          f"X2W_DRAW_STATE_DEPTH_UNCLEARED=1 restores it and hides the near triangle)")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
