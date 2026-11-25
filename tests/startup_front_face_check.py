"""The SPI input FRONT_FACE (SPI_PS_INPUT_ENA bit 12) in gfx2hlsl pixel stages, draw and mesh pipelines.

  (no arguments)                       translation controls, pure Python (dxc ps_6_0 compile when M.DXC exists)
  draw <dxc> <offscreen_check> <warp|hardware> [fxc]
                                       translate, compile, draw one pixel (the startup_flat_param renderer)

The VGPR holds +1.0 for a front face and -1.0 for a back face: Mesa nir_intrinsics.py front_face_fsign
"front_face ? 1.0 : -1.0", loaded as the front_face argument itself by ac_nir_lower_intrinsics_to_args.c (and
load_front_face as fgt(arg, 0)). The lowering is U(i.ff ? 1.0 : -1.0) over `bool ff : SV_IsFrontFace` (a pixel
input only, never in the vertex/mesh output), and the manifest says `frontface 1`: which face D3D calls front is the
PSO's FrontCounterClockwise, i.e. the runtime's cull convention, not the translator's.

Synthetic pixel stage `exp mrt0 vA, vB, vC, vD done vm` (entry reads only). Layouts: ENA 0x1000 (front_face v0),
0x9002 (persp_center v0-v1, front_face v2, pos_fixed_pt v3), 0x1001 (persp_sample v0-v1, front_face v2: per-sample, but front face is not a
position input, so it is taken).
Controls: before the change the read was refused ("front_face ... not lowered"; reproduced by removing bit 12 from
PS_INPUT_HLSL); a pixel stage without the read has no SV_IsFrontFace and no `frontface` line; the mesh stage's
output struct never carries it; ancillary (bit 13) stays refused. The draw checks that the drawn word is +1.0 or
-1.0 and that the inverted expression draws the other one (the input is live, whichever face the renderer draws).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_mesh_spi_input_check import pixel_words, mesh  # noqa: E402
from startup_image_3d_check import VS_WORDS, root_blob  # noqa: E402

LINE = '    v%d = U(i.ff ? 1.0 : -1.0);  // SPI input front_face'
FIELD = '    bool ff : SV_IsFrontFace;'


def draw_pipe(ps, masks):
    return G.Pipeline(VS_WORDS, ps, root_blob(), ps_hw_inputs=masks)


def refused(build, ps, masks, text):
    try:
        build(ps, masks).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted %s' % (masks,))


def controls():
    assert G.ps_input_layout(0x1000, 0x1000) == {0: 12}
    assert G.ps_input_layout(0x9002, 0x9002) == {0: 1, 1: 1, 2: 12, 3: 15}
    texts = {}
    for name, build in (('draw', draw_pipe), ('mesh', mesh)):
        pipe = build(pixel_words(0, 0, 0, 0), (0x1000, 0x1000))
        ps = pipe.hlsl_ps()
        assert LINE % 0 in ps.split('\n') and FIELD in ps.split('\n'), ps
        assert 'frontface 1' in pipe.manifest().split('\n'), pipe.manifest()
        texts[name] = ps
        both = build(pixel_words(2, 3, 2, 3), (0x9002, 0x9002)).hlsl_ps()
        assert LINE % 2 in both.split('\n') and 'v3 = (uint(i.pos.x) & 0xffffu)' in both, both
        assert LINE % 2 in build(pixel_words(2, 2, 2, 2), (0x1001, 0x1001)).hlsl_ps()
        plain = build(pixel_words(3, 3, 3, 3), (0x9002, 0x9002))
        assert 'SV_IsFrontFace' not in plain.hlsl_ps() and 'frontface' not in plain.manifest(), plain.hlsl_ps()
        refused(build, pixel_words(0, 0, 0, 0), (0x2000, 0x2000), 'ancillary (v0) at 0x0: not lowered')
    assert 'SV_IsFrontFace' not in draw_pipe(pixel_words(0, 0, 0, 0), (0x1000, 0x1000)).hlsl_vs()
    saved = G.PS_INPUT_HLSL.pop(G.FRONT_FACE)
    try:
        refused(draw_pipe, pixel_words(0, 0, 0, 0), (0x1000, 0x1000), 'front_face (v0) at 0x0: not lowered')
    finally:
        G.PS_INPUT_HLSL[G.FRONT_FACE] = saved
    compiled = 'dxc not found'
    if os.path.isfile(M.DXC):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'ps.hlsl')
            open(path, 'w').write(texts['mesh'])
            r = subprocess.run([M.DXC, '-nologo', '-T', 'ps_6_0', '-E', 'main', '-Fo', path + '.cso', path],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            compiled = 'dxc ps_6_0 ok'
    print('front_face: U(i.ff ? 1.0 : -1.0) over SV_IsFrontFace in draw and mesh pipelines, manifest frontface 1; '
          '%s; controls: refused before, absent when unread, not in the VS, ancillary refused' % compiled)


def check_draw(dxc, renderer, adapter, fxc=None):
    from startup_flat_param_check import VS_WORDS as FLAT_VS, root_blob as flat_root, compile_hlsl, draw
    compilers = ([('fxc', fxc, 'vs_5_1', 'ps_5_1')] if fxc else []) + [('dxc', dxc, 'vs_6_0', 'ps_6_0')]
    words = {'+1.0': ','.join(['3f800000'] * 4), '-1.0': ','.join(['bf800000'] * 4)}
    pipe = G.Pipeline(FLAT_VS, pixel_words(0, 0, 0, 0), flat_root(), ps_hw_inputs=(0x1000, 0x1000))
    good = pipe.hlsl_ps()
    inverted = good.replace('i.ff ? 1.0 : -1.0', 'i.ff ? -1.0 : 1.0')
    assert inverted != good
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        for kind, compiler, vs_profile, ps_profile in compilers:
            def build(name, source, profile):
                return compile_hlsl(compiler, kind, source, '%s_%s' % (name, kind), profile, directory)
            vs = build('vs', pipe.hlsl_vs(), vs_profile)
            seen = []
            for label, source in (('translated', good), ('inverted', inverted)):
                ps = build('ps_' + label, source, ps_profile)
                match = [face for face, expected in words.items() if draw(renderer, adapter, vs, ps, expected).returncode == 0]
                if len(match) != 1:
                    raise AssertionError('%s %s stage drew neither +1.0 nor -1.0' % (kind, label))
                seen.append(match[0])
                print('%s %s stage: every word %s' % (kind, label, match[0]), flush=True)
            if seen[0] == seen[1]:
                raise AssertionError('%s: the inverted expression drew the same word' % kind)


def main():
    if len(sys.argv) == 1:
        controls()
        return
    mode, *args = sys.argv[1:]
    if mode == 'draw':
        controls()
        check_draw(*args)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
