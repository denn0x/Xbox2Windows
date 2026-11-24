"""Map pixel-stage entry VGPRs to SPI inputs from SPI_PS_INPUT_ENA/ADDR; lower only POS_FIXED_PT.

Synthetic code only: a vertex stage that exports a position and a pixel stage `exp mrt0 vN, vN, vN, vN done vm`
that reads entry VGPR vN. Without the masks the read is refused (the request does not carry the layout); with
them, vN must be the enabled POS_FIXED_PT input, placed by the ADDR bits (Mesa ac_shader_util.c).
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from startup_image_3d_check import VS_WORDS, root_blob  # noqa: E402


def pixel_words(reg):
    # exp mrt0 vN, vN, vN, vN done vm; s_endpgm
    return struct.pack('<III', 0xf800180f, reg * 0x01010101, 0xbf810000)


def refused(reg, masks, text):
    try:
        G.Pipeline(VS_WORDS, pixel_words(reg), root_blob(), ps_hw_inputs=masks).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (reg, masks, str(error))
        return
    raise AssertionError('accepted v%d with %s' % (reg, masks))


def packed(reg):
    return 'v%d = (uint(i.pos.x) & 0xffffu) | ((uint(i.pos.y) & 0xffffu) << 16);' % reg


def main():
    # The lowering's arithmetic: SV_Position.xy is the pixel centre, so its integer part is the pixel.
    for x, y in ((0, 0), (1, 0), (0, 1), (1919, 1079), (3839, 2159), (65535, 65535)):
        assert (int(x + 0.5) & 0xffff) | ((int(y + 0.5) & 0xffff) << 16) == x | y << 16

    assert G.decode(pixel_words(2))[0][3] == 'exp mrt0 v2, v2, v2, v2 done vm'
    # Accepted: one earlier single-VGPR input, one earlier IJ pair, and an ADDR-only (allocated but not
    # enabled) pair that still moves the position: persp_sample + persp_center allocated -> v4.
    for reg, masks in ((1, (0x8100, 0x8100)), (2, (0x8002, 0x8002)), (4, (0x8002, 0x8003)),
                       (0, (0x8000, 0x8000)), (4, (0x8008, 0x8108))):  # pull model (3) + pos_x_float (1)
        ps = G.Pipeline(VS_WORDS, pixel_words(reg), root_blob(), ps_hw_inputs=masks).hlsl_ps()
        assert packed(reg) in ps, (reg, masks, ps)
        assert 'o.mrt0 = float4(F(v%d), F(v%d), F(v%d), F(v%d));' % ((reg,) * 4) in ps, ps
    # Refused, each with its own reason.
    refused(1, (0x8100, 0x8000), 'enable/address')          # ENA outside ADDR (upstream #1634's control)
    refused(1, (0x18000, 0x18000), 'enable/address')        # not a 16-bit mask
    refused(1, (0x8002, 0x8002), 'persp_center (v1) at 0x0: not lowered')
    refused(0, (0x1000, 0x1000), 'front_face (v0) at 0x0: not lowered')  # packed position disabled
    # (pos_x_float, refused here before, is lowered as SV_Position.x: tests/startup_mesh_spi_input_check.py)
    refused(0, (0x8002, 0x8003), 'persp_sample (v0) at 0x0, allocated but not enabled')
    refused(3, (0x0002, 0x0002), 'past the SPI inputs (ADDR allocates 2)')
    try:
        G.Pipeline(VS_WORDS, pixel_words(0), root_blob(), ps_hw_inputs=(0x0000, 0x8000)).hlsl_ps()
    except G.Unsupported as error:
        assert 'pos_fixed_pt (v0) at 0x0, allocated but not enabled' in str(error), str(error)
    else:
        raise AssertionError('read an allocated but disabled POS_FIXED_PT')
    # Without masks nothing changes: no packed line, and translate_to still refuses the read.
    assert 'i.pos.x' not in G.Pipeline(VS_WORDS, pixel_words(1), root_blob()).hlsl_ps()

    # Requests: the optional req_<key>.psin file (not part of the key) supplies the masks.
    G.FXC = os.path.join(tempfile.gettempdir(), 'no-such-fxc.exe')  # translation only, no compiler
    with tempfile.TemporaryDirectory() as tmp:
        ps_code = pixel_words(2)
        key = G.pipeline_key(VS_WORDS, ps_code, root_blob())
        base = os.path.join(tmp, 'req_%016x' % key)
        for ext, blob in (('.a', VS_WORDS), ('.b', ps_code), ('.root', root_blob())):
            open(base + ext, 'wb').write(blob)
        result = G.translate_request(tmp, key)
        assert result == 'unsupported: pixel stage reads entry VGPR v2 (an SPI input; layout not in the request) at 0x0', result
        for text, expect in (('spi_ps_input 1 00008002 00008002\n', 'ok'),
                             ('spi_ps_input 1 00008002 00008003\n', 'unsupported: pixel stage reads SPI input persp_center'),
                             ('spi_ps_input 1 00008002 00008002\nvs_input 1 1 00000000\n', 'ok'),
                             ('spi_ps_input 1 8002 8002\n', 'unsupported: invalid SPI input file'),
                             ('spi_ps_input 1 00008002 00008002', 'unsupported: invalid SPI input file'),
                             ('spi_ps_input 2 00008002 00008002\n', 'unsupported: invalid SPI input file')):
            for ext in ('.txt', '.fail'):
                if os.path.exists(os.path.join(tmp, '%016x%s' % (key, ext))):
                    os.remove(os.path.join(tmp, '%016x%s' % (key, ext)))
            with open(base + '.psin', 'w', newline='\n') as file:
                file.write(text)
            result = G.translate_request(tmp, key)
            assert result.startswith(expect), (text, result)
            if expect == 'ok':
                assert packed(2) in open(os.path.join(tmp, '%016x.ps.hlsl' % key)).read()
    print('SPI input layout: 5 accepted layouts, 7 refusals, request .psin read (2 accepted, 4 refused)')


if __name__ == '__main__':
    main()
