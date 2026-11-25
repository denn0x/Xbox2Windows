"""VOPC with a DPP16 / DPP8 second dword (src0 250 / 233 / 234) in tools/disasm_rdna2.py and gfx2hlsl.

Compiled pixel programs carry v_cmp words with src0 250: `7c081cfa ff4c4e0e` is v_cmp_gt_f32 with DPP16 control
0x04e (quad_perm:[2,3,0,1]), fi:1, bound_ctrl:1, src1 neg, masks 0xf, src0 v14 and vsrc1 v14. The decoder used to
print it as `v_cmp_gt_f32_e32 vcc_lo, s250, v14` followed by `.long 0xff4c4e0e`. LLVM decodes VOPC DPP from GFX11 on
(`v_cmp_gt_f32_dpp vcc_lo, v14, -v14 quad_perm:[2,3,0,1] ...`); that spelling is used.

Checks: the measured words; an integer compare, v_cmpx (EXEC result, no vcc operand) and DPP8; refusals for a
64-bit compare, float modifiers on an integer compare and an invalid DPP16 control. Translation: a mesh pixel
stage with the measured compare reads src0 through QuadReadAcrossY and compares against -v14 (the lane's own value);
control: with quad_ops off (no SM 6.0 lane reads) it is refused, as the VOP1/VOP2 DPP forms are.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import disasm_rdna2 as D  # noqa: E402
import gfx2hlsl as G  # noqa: E402
import startup_quad_dpp_check as Q  # noqa: E402

MEASURED = bytes.fromhex('fa1c087c0e4e4cff')


def vopc(op, vsrc1, src0, w1):
    return struct.pack('<II', 0x7c000000 | op << 17 | vsrc1 << 9 | src0, w1)


def asm(words):
    return D.decode_instruction(words, 0)[:3]


def main():
    assert asm(MEASURED) == (8, 'v_cmp_gt_f32_dpp', 'v_cmp_gt_f32_dpp vcc_lo, v14, -v14 quad_perm:[2,3,0,1] '
                             'row_mask:0xf bank_mask:0xf bound_ctrl:1 fi:1'), asm(MEASURED)
    # v_cmp_eq_u32 (0xc2) row_shr:1; v_cmpx_gt_f32 (0x14) quad_perm:[1,0,3,2]; DPP8 lanes 1,0,3,2,5,4,7,6
    n, name, text = asm(vopc(0xc2, 3, 250, 4 | 0x111 << 8 | 0xff << 24))
    assert (name, text) == ('v_cmp_eq_u32_dpp', 'v_cmp_eq_u32_dpp vcc_lo, v4, v3 row_shr:1 row_mask:0xf bank_mask:0xf'), text
    n, name, text = asm(vopc(0x14, 3, 250, 4 | 0xb1 << 8 | 1 << 18 | 0xff << 24))
    assert text == 'v_cmpx_gt_f32_dpp v4, v3 quad_perm:[1,0,3,2] row_mask:0xf bank_mask:0xf fi:1', text
    lanes = sum(sel << 3 * k for k, sel in enumerate((1, 0, 3, 2, 5, 4, 7, 6)))
    n, name, text = asm(vopc(0x04, 3, 233, 4 | lanes << 8))
    assert (n, text) == (8, 'v_cmp_gt_f32_dpp vcc_lo, v4, v3 dpp8:[1,0,3,2,5,4,7,6]'), text
    # refused: v_cmp_lt_f64 (0x21), neg on v_cmp_eq_u32, DPP16 control 0x1ff
    for words, why in ((vopc(0x21, 4, 250, 2 | 0x4e << 8 | 0xff << 24), 'not supported for this opcode'),
                       (vopc(0xc2, 3, 250, 4 | 0x4e << 8 | 1 << 20 | 0xff << 24), 'modifier'),
                       (vopc(0x04, 3, 250, 4 | 0x1ff << 8 | 0xff << 24), 'Invalid GFX10 DPP16 control')):
        n, name, text = asm(words)
        assert n == 8 and '<unsupported' in text and why in D.decode_instruction(words, 0)[3], (text, why)
    # translation: v2 = interpolated attribute; v_cmp_gt_f32_dpp vcc, v2 (quad +y neighbour), -v2; v4 = vcc ? 1 : 0
    cmp = vopc(0x04, 2, 250, 2 | 0x4e << 8 | 1 << 18 | 1 << 19 | 1 << 22 | 0xff << 24)
    cnd = struct.pack('<I', 0x01 << 25 | 4 << 17 | 242 << 9 | 128)  # v_cndmask_b32_e32 v4, 0, 1.0, vcc_lo
    ps = Q.INTERP + cmp + cnd + struct.pack('<III', 0xf800180f, 4 | 4 << 8 | 4 << 16 | 4 << 24, 0xbf810000)
    listing = [a for _, _, _, a in G.decode(ps)]
    assert listing[2].startswith('v_cmp_gt_f32_dpp vcc_lo, v2, -v2 quad_perm:[2,3,0,1]'), listing
    text = Q.mesh_pipeline(ps).hlsl_ps()
    assert 'uint dpp0 = QuadReadAcrossY(v2);' in text, text
    assert 'F(dpp0) > -F(v2)' in text.replace('(-F(v2))', '-F(v2)'), text
    try:
        Q.mesh_pipeline(ps, quad_ops=False).hlsl_ps()
    except G.Unsupported as error:
        assert 'v_cmp_gt_f32_dpp' in str(error), error
    else:
        raise AssertionError('VOPC DPP accepted without quad_ops')
    print('vopc dpp: measured v_cmp_gt_f32 DPP16 word decoded (quad_perm [2,3,0,1], src1 neg); u32 row_shr, cmpx and '
          'DPP8 decoded; f64, integer modifiers and an invalid control refused; the pixel stage reads src0 through '
          'QuadReadAcrossY; refused with quad_ops off (control)')


if __name__ == '__main__':
    main()
