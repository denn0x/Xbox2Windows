"""image_get_lod on a 2D texture in tools/startup/gfx2hlsl.py.

image_get_lod returns [0] the LOD a sample would use, clamped, and [1] the computed LOD before the clamp (Mesa
nir_texop_lod, emitted by ACO as image_get_lod; GLSL textureQueryLod x/y). Lowered as HLSL CalculateLevelOfDetail /
CalculateLevelOfDetailUnclamped, which take implicit derivatives like Sample, so the stage must be straight-line
with every lane active, or X2W_PS_QUAD_DIVERGENT=1 (forward control flow; marked in the manifest).

Fixture: the recorded c_lz stage of startup_image_sample_c_check with the MIMG opcode set to 0x60 (image_get_lod;
the decoder reads it back with a 2-register address) and, for the second form, dmask 0x3.
Checks: dmask 0x1 and 0x3 lower to the two calls on the sampler; controls (refused): dmask 0x4, a 2D array, the
stage after a branch without the switch (accepted with it, `quaddivergent 1`), and without op_image_get_lod
(negative control: refused as before).
  draw <dxc> [fxc]: compile the dmask 0x3 stage with dxc ps_6_0 (and fxc ps_5_1).
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import startup_image_sample_c_check as C  # noqa: E402
from startup_sampler_heap_check import VS_WORDS, root_blob  # noqa: E402

AT = 0x34  # the MIMG instruction of the c_lz fixture


def stage(dmask=1, dim=1, branch=False):
    w = bytearray(C.WORDS['lz'])
    w0, w1 = struct.unpack_from('<II', w, AT)
    w0 = w0 & ~(0x7f << 18) & ~(0xf << 8) & ~(7 << 3) | 0x60 << 18 | dmask << 8 | dim << 3
    struct.pack_into('<II', w, AT, w0, w1)
    if dmask == 3:  # export v4, v5
        struct.pack_into('<II', w, 0x40, 0xf800180f, 4 | 5 << 8 | 4 << 16 | 5 << 24)
    code = bytes(w)
    if branch:  # s_cbranch_scc0 to the next instruction first: control flow before the read
        code = code[:0x34] + struct.pack('<I', 0xbf840000) + code[0x34:]
    return code


def ps(code, switch=False):
    saved = os.environ.pop('X2W_PS_QUAD_DIVERGENT', None)
    if switch:
        os.environ['X2W_PS_QUAD_DIVERGENT'] = '1'
    try:
        pipe = G.Pipeline(VS_WORDS, code, root_blob(flags=0))
        return pipe, pipe.hlsl_ps()
    finally:
        os.environ.pop('X2W_PS_QUAD_DIVERGENT', None)
        if saved is not None:
            os.environ['X2W_PS_QUAD_DIVERGENT'] = saved


def refused(code, text, switch=False):
    try:
        ps(code, switch)
    except G.Unsupported as error:
        assert text in str(error), str(error)
    else:
        raise AssertionError('accepted, expected %r' % text)


def controls():
    listing = [a for _, _, _, a in G.decode(stage())]
    assert 'image_get_lod v4, v[0:1], s[16:23], s[24:27] dmask:0x1 dim:SQ_RSRC_IMG_2D' in listing, listing
    _, text = ps(stage())
    call = 't = float4(r0.CalculateLevelOfDetail(q0, float2(F(v0), F(v1))), ' \
           'r0.CalculateLevelOfDetailUnclamped(q0, float2(F(v0), F(v1))), 0.0, 0.0);'
    assert call in text and 'v4 = U(t.x);' in text, text
    _, text = ps(stage(dmask=3))
    assert 'v4 = U(t.x);' in text and 'v5 = U(t.y);' in text, text
    refused(stage(dmask=4), 'image_get_lod form')
    refused(stage(dim=5), 'image_get_lod form')  # SQ_RSRC_IMG_2D_ARRAY
    refused(stage(branch=True), 'image_get_lod outside straight-line code with every lane active')
    pipe, text = ps(stage(branch=True), switch=True)
    assert call in text and 'quaddivergent 1' in pipe.manifest(), pipe.manifest()
    saved = G.Stage.op_image_get_lod
    del G.Stage.op_image_get_lod
    try:
        refused(stage(), 'image_get_lod')
    finally:
        G.Stage.op_image_get_lod = saved
    print('image_get_lod: CalculateLevelOfDetail / Unclamped for dmask 0x1 and 0x3; dmask 0x4, 2D array and a '
          'branch without the switch refused, accepted with it (quaddivergent 1); refused without the handler')


def compile_check(dxc, fxc=None):
    _, text = ps(stage(dmask=3))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'ps.hlsl'
        path.write_text(text)
        runs = [('dxc', [dxc, '-T', 'ps_6_0', '-E', 'main', '-Fo', str(Path(tmp) / 'a.cso'), str(path)])]
        if fxc:
            runs.append(('fxc', [fxc, '/nologo', '/T', 'ps_5_1', '/E', 'main', '/Fo', str(Path(tmp) / 'b.cso'), str(path)]))
        for kind, args in runs:
            r = subprocess.run(args, capture_output=True, text=True, timeout=60)
            if r.returncode:
                raise AssertionError('%s: %s' % (kind, r.stderr[-1500:]))
            print('%s compiles the dmask 0x3 stage' % kind)


def main():
    controls()
    if len(sys.argv) > 1:
        if sys.argv[1] != 'draw':
            raise SystemExit(__doc__)
        compile_check(*sys.argv[2:])


if __name__ == '__main__':
    main()
