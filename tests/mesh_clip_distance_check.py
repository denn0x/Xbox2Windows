"""A mesh-stage pos1 export as SV_ClipDistance0, from the pipeline's PA_CL_VS_OUT_CNTL (req_<key>.vsout).

Hand-assembled NGG mesh program (no cache bytes): v1 = 1.0, v2 = 0; m0 = 3 vertices | 1 primitive << 12;
s_sendmsg 0x9 (GS_ALLOC_REQ); exp pos0 v1, v2, v2, v1; exp pos1 v1, v2, off, off (or v1, off, off, off) done;
exp param0; exp prim v2 done. On gfx10 the position exports are numbered in order (Mesa
src/amd/common/nir/ac_nir_prerast_utils.c ac_nir_export_position: target V_008DFC_SQ_EXP_POS + exp_num, pos0 first,
then the misc vector only when point size / edge flag / layer / viewport / shading rate is written, then the clip/cull
distance vectors 0..3 and 4..7 with write mask = the enabled distances). PA_CL_VS_OUT_CNTL (Mesa
src/amd/registers/gfx10.json) says which: VS_OUT_MISC_VEC_ENA (21) clear and VS_OUT_CCDIST0_VEC_ENA (22) set make
pos1 distances 0..3; CLIP_DIST_ENA_i [7:0], CULL_DIST_ENA_i [15:8]. The two values measured in the mesh pipelines that
export pos1 (regblob .vsregs u32 at +0xec, all 203 such programs): 0x01400101 (one distance, export mask x) and
0x01400f0f (four, mask xyzw), clip = cull in both; G.clip_vector_width lowers that as SV_ClipDistance alone.

Checks: no PA_CL_VS_OUT_CNTL -> refused (negative control: the clip output is only emitted from the register state);
0x01400f0f with a two-channel export, cull-only, a misc vector and a second distance vector -> refused; 0x01400101
with mask x -> float clip0 : SV_ClipDistance0, 0x01400303 with mask xy -> float2, in the mesh output struct and the
pixel input struct (identical, so the stages link), written under the lane's exec bit; dxc ms_6_5 + ps_6_0 compile
both when dxc is installed (skipped with a message otherwise).
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_image_3d_check import root_blob  # noqa: E402


def mesh(pos1_word0, pos1_word1):
    return struct.pack('<14I',
                       0x7e0202f2,               # v_mov_b32 v1, 1.0
                       0x7e040280,               # v_mov_b32 v2, 0
                       0xb07c1003,               # s_movk_i32 m0, 0x1003
                       0xbf900009,               # s_sendmsg 0x9
                       0xf80000cf, 0x01020201,   # exp pos0 v1, v2, v2, v1
                       pos1_word0, pos1_word1,   # exp pos1 ... done
                       0xf800020f, 0x01020201,   # exp param0 v1, v2, v2, v1
                       0xf8000941, 0x00000002,   # exp prim v2 done
                       0xbf810000, 0xbf810000)   # s_endpgm (twice: padding)


MS_XY = mesh(0xf80008d3, 0x00000201)      # exp pos1 v1, v2, off, off done
MS_X = mesh(0xf80008d1, 0x00000001)       # exp pos1 v1, off, off, off done
PS = struct.pack('<4I', 0x7e0002f2,       # v_mov_b32 v0, 1.0
                 0xf800180f, 0x00000000,  # exp mrt0 v0, v0, v0, v0 done vm
                 0xbf810000)


def refused(code, cntl, why):
    try:
        M.MeshPipeline(code, PS, root_blob(), vs_out_cntl=cntl).hlsl_ms()
    except G.Unsupported as error:
        assert why in str(error), (why, str(error))
    else:
        raise AssertionError('accepted PA_CL_VS_OUT_CNTL %r' % cntl)


def accepted(code, cntl, decl, write):
    pipe = M.MeshPipeline(code, PS, root_blob(), vs_out_cntl=cntl)
    ms, ps = pipe.hlsl_ms(), pipe.hlsl_ps()
    assert '    %s clip0 : SV_ClipDistance0;' % decl in ms, ms
    assert '    %s clip0 : SV_ClipDistance0;' % decl in ps, ps
    assert 'if (exec & 1) { %s }' % write in ms, ms
    # the pixel input struct and the mesh output struct list the same fields in the same order (linkage)
    def fields(text, name):
        body = text.split('struct %s {' % name, 1)[1].split('};', 1)[0]
        return [l.strip() for l in body.strip().splitlines()]
    out_fields = fields(ms, 'MSOut')
    ps_struct = next(n for n in ('PSIn', 'VSOut', 'PSInput') if 'struct %s {' % n in ps)
    assert out_fields == fields(ps, ps_struct), (out_fields, fields(ps, ps_struct))
    return pipe


def main():
    text = [x[3] for x in G.decode(MS_XY)]
    assert text[3] == 's_sendmsg 0x9' and text[5] == 'exp pos1 v1, v2, off, off done', text
    assert [x[3] for x in G.decode(MS_X)][5] == 'exp pos1 v1, off, off, off done'
    refused(MS_XY, None, 'pos1 without PA_CL_VS_OUT_CNTL')         # negative control: no register state
    refused(MS_XY, 0x01400f0f, 'PA_CL_VS_OUT_CNTL')                 # four distances enabled, two written
    refused(MS_XY, 0x01400300, 'PA_CL_VS_OUT_CNTL')                 # cull distances only
    refused(MS_XY, 0x01400303 | 1 << 21, 'PA_CL_VS_OUT_CNTL')       # misc vector first: pos1 is not distances
    refused(MS_XY, 0x01400303 | 1 << 23, 'PA_CL_VS_OUT_CNTL')       # a second distance vector
    accepted(MS_X, 0x01400101, 'float', 'o.clip0 = F(v1);')
    accepted(MS_XY, 0x01400303, 'float2', 'o.clip0 = float2(F(v1), F(v2));')
    compiled = 'dxc not found'
    if os.path.isfile(M.DXC):
        with tempfile.TemporaryDirectory() as d:
            for key, (code, cntl) in enumerate(((MS_X, 0x01400101), (MS_XY, 0x01400303))):
                r = M.translate_to(d, key, code, PS, root_blob(), vs_out_cntl=cntl)
                assert r == 'ok', r
            r = M.translate_to(d, 9, MS_XY, PS, root_blob())
            assert r.startswith('unsupported:') and 'pos1' in r, r
            # a request: req_<key>.{a,b,root} and the optional .vsout beside it
            base = os.path.join(d, 'req_%016x' % 5)
            for ext, data in (('.a', PS), ('.b', MS_XY), ('.root', root_blob())):
                open(base + ext, 'wb').write(data)
            r = M.translate_request(d, 5)
            assert r.startswith('unsupported:') and 'pos1' in r, r
            open(base + '.vsout', 'w', newline='\n').write('pa_cl_vs_out_cntl 1 01400303\n')
            r = M.translate_request(d, 5)
            assert r == 'ok', r
            assert 'SV_ClipDistance0' in open(os.path.join(d, '%016x.ms.hlsl' % 5)).read()
        compiled = 'dxc ms_6_5 + ps_6_0 ok'
    print('mesh clip distance: pos1 refused without PA_CL_VS_OUT_CNTL; 0x01400101 -> float, 0x01400303 -> float2 '
          'SV_ClipDistance0 in both stage structs; 4 bad states refused; %s' % compiled)


if __name__ == '__main__':
    main()
