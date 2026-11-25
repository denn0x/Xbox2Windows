"""image_sample_d (sample with explicit derivatives) in tools/startup/gfx2hlsl.py.

The fixture is the source-built pixel stage of startup_image_sample_l_check.py (v0 = 0.5, v1 = 0.25, v2 = 1.0,
v3 = 2.0; texture from descriptor table 0, all-zero constant sampler). Its MIMG opcode is set to 0x22, which the
decoder names image_sample_d; the 2D address is then six dwords, v[0:5].

LLVM IntrinsicsAMDGPU.td orders the address as GradientArgs and then CoordSliceArgs (AMDGPUDimProfile.AddrArgs).
For 2D, GradientArgs are dsdh, dtdh, dsdv, dtdv (AMDGPUDimProps: d<c>dh for each coordinate, then d<c>dv). So v0,
v1 are the horizontal (ddx) gradient, v2, v3 the vertical (ddy) one, and v4, v5 are u and v:
SampleGrad(q0, float2(F(v4), F(v5)), float2(F(v0), F(v1)), float2(F(v2), F(v3))).

SampleGrad needs no implicit derivatives, so unlike Sample and SampleBias it is lowered after a branch too.
Controls: without the handler the stage refuses `image_sample_d` as before; a16 is refused (the gradients would be
packed too). The pixel HLSL compiles with dxc ps_6_0 and, when found, fxc ps_5_1.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_image_sample_b_check import program, ps_text, refused  # noqa: E402

EXPECTED = 't = r0.SampleGrad(q0, float2(F(v4), F(v5)), float2(F(v0), F(v1)), float2(F(v2), F(v3)));'


def compile_ps(text):
    results = []
    fxc = os.path.join(os.path.dirname(M.DXC), 'fxc.exe')
    for tool, args in ((M.DXC, ['-nologo', '-T', 'ps_6_0']), (fxc, ['/nologo', '/T', 'ps_5_1'])):
        if not os.path.isfile(tool):
            results.append('%s not found' % os.path.basename(tool))
            continue
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'ps.hlsl')
            open(path, 'w').write(text)
            r = subprocess.run([tool] + args + ['-E' if tool == M.DXC else '/E', 'main',
                               '-Fo' if tool == M.DXC else '/Fo', path + '.cso', path], capture_output=True, text=True)
            assert r.returncode == 0, (tool, r.stdout, r.stderr)
            results.append('%s %s ok' % (os.path.basename(tool)[:3], args[-1]))
    return ', '.join(results)


def main():
    code = program(0x22)
    asm = G.decode(code)[10][3]
    assert asm == 'image_sample_d v[4:7], v[0:5], s[16:23], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D', asm
    ps = ps_text(code)
    assert EXPECTED in ps, ps
    for comp, reg in zip('xyzw', range(4, 8)):
        assert 'v%d = U(t.%s);' % (reg, comp) in ps, ps
    # after a branch (not straight-line): still SampleGrad, explicit derivatives
    branched = ps_text(program(0x22, prefix_branch=True))
    assert EXPECTED in branched, branched
    compiled = compile_ps(ps)
    # controls
    saved = G.Stage.op_image_sample_d
    del G.Stage.op_image_sample_d
    try:
        refused(code, 'image_sample_d')
    finally:
        G.Stage.op_image_sample_d = saved
    a16 = bytearray(code)
    a16[0x34:0x38] = struct.pack('<I', struct.unpack_from('<I', a16, 0x34)[0] | 1 << 30)  # A16 (second dword bit 30)
    assert G.decode(bytes(a16))[10][3].endswith(' a16'), G.decode(bytes(a16))[10][3]
    refused(bytes(a16), 'image_sample_d form')
    print('image_sample_d: SampleGrad(uv = v4,v5; ddx = v0,v1; ddy = v2,v3), also after a branch; %s; controls: '
          'refused without the handler, with a16' % compiled)


if __name__ == '__main__':
    main()
