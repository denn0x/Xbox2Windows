"""A vertex-stage pos1 export as SV_ClipDistance0, from the request's PA_CL_VS_OUT_CNTL (req_<key>.vsout).

Synthetic code: v4 = 1.0, v5 = 0; exp pos0 v4, v5, v5, v4; exp pos1 v4, v5, off, off done. PA_CL_VS_OUT_CNTL fields
from Mesa src/amd/registers/gfx10.json: with VS_OUT_MISC_VEC_ENA (21) clear and VS_OUT_CCDIST0_VEC_ENA (22) set,
the export after pos0 is the clip/cull distance vector 0..3; CLIP_DIST_ENA_i [7:0], CULL_DIST_ENA_i [15:8].
0x01400303 (clip and cull 0, 1; CCDIST0; MISC_SIDE_BUS) is the value measured for every pos1-exporting request in
the run-84 dumps. Checks: no .vsout -> refused as before; the accepted value lowers to SV_ClipDistance0 (float2) in
both stage interfaces and the rectangle-list corner, and FXC compiles it; misc vector, cull-only, a channel
mismatch, CCDIST1 and a malformed file are refused.
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from startup_image_3d_check import PS_WORDS, root_blob  # noqa: E402

VS = struct.pack('<7I', 0x7e0802f2, 0x7e0a0280,            # v_mov_b32 v4, 1.0 ; v_mov_b32 v5, 0
                 0xf80000cf, 0x04050504,                    # exp pos0 v4, v5, v5, v4
                 0xf80008d3, 0x00000504,                    # exp pos1 v4, v5, off, off done
                 0xbf810000)
GOOD = 0x01400303


def refused(cntl, why):
    try:
        G.Pipeline(VS, PS_WORDS, root_blob(), vs_out_cntl=cntl).hlsl_vs()
    except G.Unsupported as error:
        assert why in str(error), (why, str(error))
    else:
        raise AssertionError('accepted PA_CL_VS_OUT_CNTL %r' % cntl)


def main():
    text = [x[3] for x in G.decode(VS)]
    assert text[2] == 'exp pos0 v4, v5, v5, v4' and text[3] == 'exp pos1 v4, v5, off, off done', text
    refused(None, 'export target pos1')
    pipe = G.Pipeline(VS, PS_WORDS, root_blob(), vs_out_cntl=GOOD)
    vs, ps = pipe.hlsl_vs(), pipe.hlsl_ps()
    for line in ('    float2 clip0 : SV_ClipDistance0;', 'o.clip0 = float2(F(v4), F(v5));',
                 '    o.clip0 = p.clip0 + r.clip0 - q.clip0;'):
        assert line in vs, (line, vs)
    assert '    float2 clip0 : SV_ClipDistance0;' in ps, ps
    with tempfile.TemporaryDirectory() as d:
        result = G.translate_to(d, 1, VS, PS_WORDS, root_blob(), vs_out_cntl=GOOD)
        assert result == 'ok', result
        assert os.path.getsize(os.path.join(d, '%016x.vs.cso' % 1)) > 0
        for content, ok in (('pa_cl_vs_out_cntl 1 01400303\n', True), ('pa_cl_vs_out_cntl 1 1400303\n', False)):
            p = os.path.join(d, 'x.vsout')
            open(p, 'w', newline='\n').write(content)
            try:
                assert G.read_vs_out(p) == GOOD and ok
            except G.Unsupported as error:
                assert not ok and 'invalid vertex output control file' in str(error), str(error)
    refused(GOOD | 1 << 21, 'PA_CL_VS_OUT_CNTL')            # misc vector first: pos1 is not the distances
    refused(0x00400300, 'PA_CL_VS_OUT_CNTL')                 # cull distances only
    refused(0x00400307, 'PA_CL_VS_OUT_CNTL')                 # three clip distances, two channels written
    refused(GOOD | 1 << 23, 'PA_CL_VS_OUT_CNTL')            # a second distance vector
    refused(GOOD & ~(1 << 22), 'PA_CL_VS_OUT_CNTL')         # no distance vector enabled
    print('clip distance: pos1 refused without .vsout; 0x01400303 -> SV_ClipDistance0 float2, FXC ok; 5 refusals')


if __name__ == '__main__':
    main()
