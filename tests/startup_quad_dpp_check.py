"""DPP16 quad permutations in a mesh pipeline's pixel stage (gfx2hlsl Stage.quad_dpp).

Synthetic words (VOP2 DPP16: op [30:25], vdst [24:17], vsrc1 [16:9], src0 = 250; second dword: src0 VGPR [7:0],
dpp_ctrl [16:8], fi 18, bound_ctrl 19, src0 neg 20 / abs 21, bank_mask [27:24], row_mask [31:28]; VOP1 DPP16 the same
with 0x3f in [31:25] and op [16:9]); names asserted from the decoder. The program is the coarse-derivative idiom Mesa
ACO emits (aco_select_nir_intrinsics.cpp ddx_coarse / ddy_coarse):

    v_interp_p1/p2_f32 v2, attr0.x
    v_subrev_f32_dpp v4, v2, v2 quad_perm:[0,0,0,0]     v4 = v2 - v2[lane 0 of the quad]
    v_mov_b32_dpp    v5, v4 quad_perm:[1,1,1,1]         v5 = v4[lane 1] = ddx_coarse
    v_mov_b32_dpp    v6, v4 quad_perm:[2,2,2,2]         v6 = v4[lane 2] = ddy_coarse
    v_add_f32_dpp    v7, -v2, v2 quad_perm:[1,0,3,2]    v7 = v2 - v2[lane ^ 1]
    exp mrt0 v5, v6, v7, v2 done vm

The emitted HLSL is run on a model 2x2 quad (QuadReadLaneAt / QuadReadAcrossX of D3D: lane 0 top-left, 1 top-right,
2 bottom-left, 3 bottom-right) and compared with the reference in binary32. The mesh stage is a stub (this checks the
pixel stage only). With dxc present the pixel HLSL also compiles as ps_6_0.

Controls: the draw pipeline (fxc ps_5_1, no quad reads) refuses `v_subrev_f32_dpp` as before; v_sub_f32_dpp (the
operands the other way round) gives the negated result on the model; row_mirror, fi:0, a branch around the DPP and
a loop are refused; with quad_ops off the mesh pipeline refuses as before the change.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_image_3d_check import VS_WORDS, root_blob  # noqa: E402

PERM_X = 0xb1  # quad_perm:[1,0,3,2]


def vop2(op, d, s1, s0, ctrl, fi=1, bc=1, neg0=0):
    return struct.pack('<II', op << 25 | d << 17 | s1 << 9 | 250,
                       s0 | ctrl << 8 | fi << 18 | bc << 19 | neg0 << 20 | 0xf << 24 | 0xf << 28)


def vop1(op, d, s0, ctrl, fi=1, bc=1):
    return struct.pack('<II', 0x3f << 25 | d << 17 | op << 9 | 250, s0 | ctrl << 8 | fi << 18 | bc << 19 | 0xf << 24 | 0xf << 28)


INTERP = struct.pack('<II', 0xc8080000, 0xc8090001)        # v_interp_p1_f32 v2, v0, attr0.x; p2 v2, v1
EXPORT = struct.pack('<III', 0xf800180f, 5 | 6 << 8 | 7 << 16 | 2 << 24, 0xbf810000)


def program(first_op=5, ctrl0=0, fi=1, prefix=b''):
    return (INTERP + prefix + vop2(first_op, 4, 2, 2, ctrl0, fi=fi) + vop1(1, 5, 4, 0x55) + vop1(1, 6, 4, 0xaa)
            + vop2(3, 7, 2, 2, PERM_X, neg0=1) + EXPORT)


class StubMesh:
    def __init__(self, pipe, code):
        self.exports = {'pos': ['0'] * 4, 'param0': ['0'] * 4}
        self.exported_prim = True

    def body(self):
        return []


def mesh_pipeline(ps, quad_ops=True):
    saved_stage = M.MeshStage
    M.MeshStage = StubMesh
    try:
        if not quad_ops:
            # the negative control: the pipeline's `self.quad_ops = True` has no effect (reads stay False)
            M.MeshPipeline.quad_ops = property(lambda self: False, lambda self, v: None)
        return M.MeshPipeline(b'', ps, root_blob())
    finally:
        M.MeshStage = saved_stage
        if not quad_ops:
            del M.MeshPipeline.quad_ops


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def F(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def U(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def run_quad(lines, attr):
    """Run the pixel body's statements on 4 lanes; attr[lane] is the float i.param0.x."""
    env = [{'ip0x': attr[k]} for k in range(4)]
    quad = {'QuadReadLaneAt': lambda vals, k: [vals[k]] * 4,
            'QuadReadAcrossX': lambda vals: [vals[k ^ 1] for k in range(4)]}
    for line in lines:
        line = line.strip()
        m = re.fullmatch(r'uint (dpp\d+) = (QuadRead\w+)\((v\d+)(?:, (\d)u)?\);', line)
        if m:
            vals = [e[m.group(3)] for e in env]
            out = quad[m.group(2)](vals, int(m.group(4))) if m.group(4) else quad[m.group(2)](vals)
            for k in range(4):
                env[k][m.group(1)] = out[k]
            continue
        m = re.fullmatch(r'(v\d+|o\.mrt0) = (.*);', line)
        assert m, line
        expr = m.group(2).replace('U(i.param0.x)', 'U(ip0x)')
        expr = re.sub(r'float4\((.*)\)', r'(\1)', expr)
        for k in range(4):
            env[k][m.group(1)] = eval(expr, {'F': F, 'U': U, '__builtins__': {}}, env[k])
    return [e['o.mrt0'] for e in env]


def refused(ps, text, build=mesh_pipeline):
    try:
        build(ps).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return str(error)
    raise AssertionError('accepted: expected a refusal with %r' % text)


def main():
    ps = program()
    names = [x[2] for x in G.decode(ps)]
    assert names[2:7] == ['v_subrev_f32_dpp', 'v_mov_b32_dpp', 'v_mov_b32_dpp', 'v_add_f32_dpp', 'exp'], names
    assert G.decode(ps)[5][3].startswith('v_add_f32_dpp v7, -v2, v2 quad_perm:[1,0,3,2]'), G.decode(ps)[5][3]
    pipe = mesh_pipeline(ps)
    hlsl = pipe.hlsl_ps()
    body = pipe.ps.body()
    assert '    uint dpp0 = QuadReadLaneAt(v2, 0u);' in body and '    v4 = U(F(v2) - F(dpp0));' in body, body
    assert '    uint dpp3 = QuadReadAcrossX(v2);' in body and '    v7 = U((-F(dpp3)) + F(v2));' in body, body
    quads = [(0.25, 1.5, -3.0, 7.75), (100.0, 100.0, 100.0, 100.0), (1e-3, 2e-3, 4e-3, 8e-3), (-0.0, 0.0, 5.0, -5.0)]
    for a in quads:
        a = [f32(x) for x in a]
        got = run_quad(body, a)
        for k in range(4):
            want = (f32(a[1] - a[0]), f32(a[2] - a[0]), f32(a[k] - a[k ^ 1]), a[k])
            assert got[k] == want, (a, k, got[k], want)
    # operands the other way round (v_sub_f32_dpp: D = S0' - S1) is the negated derivative on the model
    sub = run_quad(mesh_pipeline(program(first_op=4)).ps.body(), (0.25, 1.5, -3.0, 7.75))
    assert sub[0][0] == -1.25 and sub[0][1] == 3.25, sub[0]

    compiled = 'dxc not found'
    if os.path.isfile(M.DXC):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'ps.hlsl')
            open(path, 'w').write(hlsl)
            r = subprocess.run([M.DXC, '-nologo', '-T', 'ps_6_0', '-E', 'main', '-Fo', path + '.cso', path],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            compiled = 'dxc ps_6_0 ok'

    # controls
    try:
        G.Pipeline(VS_WORDS, ps, root_blob()).hlsl_ps()
    except G.Unsupported as error:
        assert str(error) == 'v_subrev_f32_dpp', str(error)
    else:
        raise AssertionError('the fxc draw pipeline accepted a quad read')
    refused(ps, 'v_subrev_f32_dpp', lambda p: mesh_pipeline(p, quad_ops=False))
    refused(program(ctrl0=0x140), 'v_subrev_f32_dpp with row_mirror (not a quad permutation)')
    refused(program(fi=0), 'v_subrev_f32_dpp with row_mask:0xf bank_mask:0xf bound_ctrl:1')
    # s_cbranch_execz +1 instruction... over the first DPP: a lane with exec off would skip the quad read
    skip = struct.pack('<I', 0xbf880002)
    refused(program(prefix=skip), 'is skipped on a path to')
    # a backward branch (s_cbranch_scc0 to the program start) makes a loop
    loop = INTERP + vop2(5, 4, 2, 2, 0) + struct.pack('<I', 0xbf84fffb) + vop1(1, 5, 4, 0x55) + vop1(1, 6, 4, 0xaa) \
        + vop2(3, 7, 2, 2, PERM_X, neg0=1) + EXPORT
    assert G.decode(loop)[3][3].startswith('s_cbranch_scc0'), G.decode(loop)[3]
    refused(loop, 'in a program with a loop')
    print('quad dpp: coarse ddx/ddy and AcrossX match binary32 on %d model quads; %s; 7 controls (fxc refuses, '
          'sub reversed, quad_ops off, row_mirror, fi:0, skipped, loop)' % (len(quads), compiled))


if __name__ == '__main__':
    main()
