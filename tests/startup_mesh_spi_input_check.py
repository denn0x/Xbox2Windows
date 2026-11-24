"""SPI pixel inputs in mesh pipelines (gfx2hlsl Pipeline.spi_inputs / spi_reads, shared with draws).

The mapping of entry VGPRs to SPI inputs (ps_input_layout, Mesa ac_shader_util.c order) used to live in
G.Pipeline.__init__, which MeshPipeline does not call, so a mesh pair's pixel stage refused every SPI input read,
the POS_FIXED_PT the draws already lower included. Both pipelines now call the shared methods.

Synthetic pixel stage `exp mrt0 vA, vB, vC, vD done vm` (entry reads only); the mesh stage is a stub (this checks
the pixel stage). ENA 0x8002: persp_center v0-v1, pos_fixed_pt v2.

Controls: without ENA the read is refused as before (the old behaviour, which a disabled change reproduces); the
barycentric persp_center read as a value is refused. translate_request passes req_<key>.psin through
(translate_to replaced by a recorder).
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
    ps = pixel_words(2, 2, 2, 2)
    assert G.decode(ps)[0][3] == 'exp mrt0 v2, v2, v2, v2 done vm', G.decode(ps)[0][3]
    hlsl = mesh(ps, (0x8002, 0x8002)).hlsl_ps()
    assert '    v2 = (uint(i.pos.x) & 0xffffu) | ((uint(i.pos.y) & 0xffffu) << 16);' in hlsl.split('\n'), hlsl
    refused(ps, None, 'layout not in the request')
    refused(pixel_words(0, 0, 0, 0), (0x8002, 0x8002), 'persp_center (v0) at 0x0: not lowered')

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
                f.write('spi_ps_input 1 00008002 00008002\n')
            assert M.translate_request(tmp, 1) == 'recorded' and seen[-1] == (0x8002, 0x8002), seen
            with open(base + '.psin', 'w', newline='\n') as f:
                f.write('spi_ps_input 1 8002 8002\n')
            assert M.translate_request(tmp, 1) == 'unsupported: invalid SPI input file'
    finally:
        M.translate_to, M.is_mesh = saved_to, saved_is
    print('mesh SPI inputs: pos_fixed_pt lowered in a mesh pipeline; 2 refusals; .psin passed through translate_request')


if __name__ == '__main__':
    main()
