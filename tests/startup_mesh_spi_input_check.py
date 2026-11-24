"""SPI pixel inputs in mesh pipelines, and the position inputs POS_X/Y/Z/W_FLOAT (gfx2hlsl spi_inputs/spi_reads).

The mapping of entry VGPRs to SPI inputs (ps_input_layout, Mesa ac_shader_util.c order) used to live in
G.Pipeline.__init__, which MeshPipeline does not call, so a mesh pair's pixel stage refused every SPI input read.
Both pipelines now call the shared Pipeline.spi_inputs / spi_reads.

Synthetic pixel stage `exp mrt0 vA, vB, vC, vD done vm` (entry reads only); the mesh stage is a stub (this checks
the pixel stage). ENA 0x8c02 is a layout that enables all three position forms: persp_center v0-v1,
pos_z_float v2, pos_w_float v3, pos_fixed_pt v4.
POS_W_FLOAT is SV_Position.w: Mesa ac_nir_lower_intrinsics_to_args.c loads frag_coord_w_rcp from it,
nir_builder.c sets gl_FragCoord.w = frcp(frag_coord_w_rcp), and vkd3d-shader spirv.c (frag_coord_fixup) gives
D3D SV_Position.w = 1 / gl_FragCoord.w.

Controls: without ENA the mesh translate_to refuses the read (the old behaviour); the barycentric persp_center read
as a value is refused; with a per-sample barycentric enabled the position read is refused; the draw pipeline lowers
the same inputs. translate_request passes req_<key>.psin through (translate_to replaced by a recorder).
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_image_3d_check import VS_WORDS, root_blob  # noqa: E402


def pixel_words(a, b, c, d):
    return struct.pack('<III', 0xf800180f, a | b << 8 | c << 16 | d << 24, 0xbf810000)


class StubMesh:
    def __init__(self, pipe, code):
        self.exports = {'pos': ['0'] * 4}
        self.exported_prim = True

    def body(self):
        return []


def mesh(ps, masks):
    saved = M.MeshStage
    M.MeshStage = StubMesh
    try:
        return M.MeshPipeline(b'', ps, root_blob(), ps_hw_inputs=masks)
    finally:
        M.MeshStage = saved


def refused(ps, masks, text):
    try:
        pipe = mesh(ps, masks)
        if pipe.ps_entry_reads:  # what mesh2hlsl.translate_to refuses
            raise G.Unsupported('pixel stage reads entry VGPR (an SPI input; layout not in the request)')
        pipe.hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted %s' % (masks,))


def main():
    ps = pixel_words(2, 3, 4, 4)
    assert G.decode(ps)[0][3] == 'exp mrt0 v2, v3, v4, v4 done vm', G.decode(ps)[0][3]
    assert G.ps_input_layout(0x8c02, 0x8c02) == {0: 1, 1: 1, 2: 10, 3: 11, 4: 15}
    hlsl = mesh(ps, (0x8c02, 0x8c02)).hlsl_ps()
    for line in ('    v2 = U(i.pos.z);  // SPI input pos_z_float', '    v3 = U(i.pos.w);  // SPI input pos_w_float',
                 '    v4 = (uint(i.pos.x) & 0xffffu) | ((uint(i.pos.y) & 0xffffu) << 16);',
                 '    o.mrt0 = float4(F(v2), F(v3), F(v4), F(v4));'):
        assert line in hlsl.split('\n'), (line, hlsl)
    # x and y (ENA 0x0300: pos_x_float v0, pos_y_float v1), in the draw pipeline too
    for build in (lambda p, m: mesh(p, m), lambda p, m: G.Pipeline(VS_WORDS, p, root_blob(), ps_hw_inputs=m)):
        text = build(pixel_words(0, 1, 1, 0), (0x0300, 0x0300)).hlsl_ps()
        assert '    v0 = U(i.pos.x);  // SPI input pos_x_float' in text and '    v1 = U(i.pos.y);  // SPI input pos_y_float' in text, text
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
    refused(ps, None, 'layout not in the request')
    refused(pixel_words(0, 0, 0, 0), (0x8c02, 0x8c02), 'persp_center (v0) at 0x0: not lowered')
    refused(pixel_words(4, 4, 4, 4), (0x8c03, 0x8c03), 'per-sample barycentrics enabled (0x8c03)')

    # translate_request: req_<key>.psin reaches translate_to
    seen = []
    saved_to, saved_is = M.translate_to, M.is_mesh
    M.translate_to = lambda out, key, ms, p, root, **k: seen.append(k.get('ps_hw_inputs')) or 'recorded'
    M.is_mesh = lambda code: code == b'MESH'
    try:
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, 'req_%016x' % 1)
            for ext, blob in (('.a', b'MESH'), ('.b', ps), ('.root', root_blob())):
                open(base + ext, 'wb').write(blob)
            assert M.translate_request(tmp, 1) == 'recorded' and seen[-1] is None, seen
            with open(base + '.psin', 'w', newline='\n') as f:
                f.write('spi_ps_input 1 00008c02 00008c02\n')
            assert M.translate_request(tmp, 1) == 'recorded' and seen[-1] == (0x8c02, 0x8c02), seen
            with open(base + '.psin', 'w', newline='\n') as f:
                f.write('spi_ps_input 1 8c02 8c02\n')
            assert M.translate_request(tmp, 1) == 'unsupported: invalid SPI input file'
    finally:
        M.translate_to, M.is_mesh = saved_to, saved_is
    print('mesh SPI inputs: pos_z/pos_w/pos_fixed_pt (ENA 0x8c02) and pos_x/pos_y lowered in mesh and draw '
          'pipelines; %s; 3 refusals; .psin passed through translate_request' % compiled)


if __name__ == '__main__':
    main()
