"""X2W_PS_QUAD_DIVERGENT=1 in tools/startup/gfx2hlsl.py: quad reads and image_sample_b where a lane may skip them.

Off (the default), a quad read (DPP quad_perm) on a path some lane avoids and an image_sample_b after a branch are
refused, as before. On, both are lowered in forward control flow. That is exact only where the four lanes of each
quad take the same path: D3D leaves quad reads and implicit derivatives undefined for lanes outside the control
flow, while the hardware reads inactive lanes (DPP fi:1). Loops stay refused either way, and a pipeline that used
the switch says `quaddivergent 1` in its manifest; one that did not use it has no such line even with the switch on.

Fixtures: startup_quad_dpp_check's stage with s_cbranch_execz over the first quad read, and its backward-branch
loop; startup_image_sample_b_check's stage with an s_cbranch_scc0 before the sample, and a loop form of it. The
environment variable is read when the stage is built, so the test sets and clears it around each pipeline.
"""
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import startup_quad_dpp_check as Q  # noqa: E402
import startup_image_sample_b_check as B  # noqa: E402

FLAG = 'X2W_PS_QUAD_DIVERGENT'


def build(make, value):
    saved = os.environ.pop(FLAG, None)
    if value is not None:
        os.environ[FLAG] = value
    try:
        return make()
    finally:
        os.environ.pop(FLAG, None)
        if saved is not None:
            os.environ[FLAG] = saved


def refused(make, value, text):
    try:
        build(make, value).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted with %s=%s, expected %r' % (FLAG, value, text))


def main():
    skip = Q.program(prefix=struct.pack('<I', 0xbf880002))  # s_cbranch_execz over the first quad read
    loop = Q.INTERP + Q.vop2(5, 4, 2, 2, 0) + struct.pack('<I', 0xbf84fffb) + Q.vop1(1, 5, 4, 0x55) \
        + Q.vop1(1, 6, 4, 0xaa) + Q.vop2(3, 7, 2, 2, Q.PERM_X, neg0=1) + Q.EXPORT
    assert G.decode(skip)[2][3].startswith('s_cbranch_execz'), G.decode(skip)[2]
    # quad reads: off (unset, '0') refused as before; on lowered and marked
    for value in (None, '0', 'yes'):
        refused(lambda: Q.mesh_pipeline(skip), value, 'quad read (DPP) at 0xc is skipped on a path to')
    pipe = build(lambda: Q.mesh_pipeline(skip), '1')
    body = pipe.ps.body()
    assert '        uint dpp0 = QuadReadLaneAt(v2, 0u);' in body and '    if (pc == 1) {' in body, body
    assert pipe.ps.quad_divergent_used and 'quaddivergent 1' in pipe.manifest().split('\n'), pipe.manifest()
    refused(lambda: Q.mesh_pipeline(loop), '1', 'in a program with a loop')
    straight = build(lambda: Q.mesh_pipeline(Q.program()), '1')
    assert not straight.ps.quad_divergent_used and 'quaddivergent' not in straight.manifest()
    assert 'quaddivergent' not in build(lambda: Q.mesh_pipeline(Q.program()), None).manifest()

    # image_sample_b after a branch
    branched = B.program(0x25, prefix_branch=True)
    refused(lambda: G.Pipeline(B.VS_WORDS, branched, B.root_blob()), None,
            'image_sample_b outside straight-line code with every lane active')
    pipe = build(lambda: G.Pipeline(B.VS_WORDS, branched, B.root_blob()), '1')
    assert 't = r0.SampleBias(q0, float2(F(v1), F(v2)), F(v0));' in pipe.hlsl_ps(), pipe.hlsl_ps()
    assert 'quaddivergent 1' in pipe.manifest().split('\n'), pipe.manifest()
    # a loop: the scc0 branch before the sample made backward (to offset 0)
    words = bytearray(branched)
    words[0x2c:0x30] = struct.pack('<I', 0xbf840000 | (-12 & 0xffff))  # s_cbranch_scc0 to 0x0
    assert G.decode(bytes(words))[9][3] == 's_cbranch_scc0 label_0x0', G.decode(bytes(words))[9]
    refused(lambda: G.Pipeline(B.VS_WORDS, bytes(words), B.root_blob()), '1',
            'image_sample_b outside straight-line code with every lane active')
    plain = build(lambda: G.Pipeline(B.VS_WORDS, B.program(0x25), B.root_blob()), '1')
    assert 'quaddivergent' not in plain.manifest()
    print('%s: off (unset, 0, yes) refuses as before; 1 lowers the skipped quad read and the branched '
          'image_sample_b with `quaddivergent 1`; loops refused; no marker when unused' % FLAG)


if __name__ == '__main__':
    main()
