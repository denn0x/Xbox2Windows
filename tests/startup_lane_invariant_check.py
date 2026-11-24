"""v_readfirstlane_b32 of a lane-invariant value, and the carry forms v_{add,sub,subrev}_co[_ci]_u32.

Synthetic gfx1013 words only. The readfirstlane case is the shape LLVM AMDGPU emits for a 32-bit unsigned
division by a uniform divisor (AMDGPUCodeGenPrepare expandDivRem32: v_cvt_f32_u32 of the SGPR divisor, v_rcp,
v_mul by 0x4f7ffffe = 2^32 - 512, v_cvt_u32, then v_readfirstlane_b32 back to an SGPR), here behind a scalar
branch that is allowed now. Controls refuse: a source written from an SGPR assigned on only one side of a join, a
source behind a loop head, a source built from vcc, and a source read at a branch target.

The carry forms are evaluated by the integer HLSL evaluator of startup_gfx2hlsl_bit_alu_check over an edge grid and
compared with 33-bit Python references written from the AMD RDNA 2 ISA description (V_ADD_CO_CI_U32: D = S0 + S1 +
CI, carry out; V_SUB_CO_CI_U32: D = S0 - S1 - CI, borrow when S1 + CI > S0; SUBREV swaps S0 and S1).
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_gfx2hlsl_bit_alu_check import Expr  # noqa: E402

MASK = 0xffffffff
LITERAL = 255


class TestPipeline:
    root = {'params': [{'type': 2}], 'flags': 0}

    def resource(self, key, kind):
        assert key == (('root', 0), None) and kind == 'buf'
        return 'r0'

    def ud(self, source):
        return 0


def vop1(op, dst, src):
    return 0x7e000000 | dst << 17 | op << 9 | src


def vop2(op, dst, src0, vsrc1):
    return op << 25 | dst << 17 | vsrc1 << 9 | src0


SBL = [0xf4000000 | 8 << 18 | 7 << 6 | 4, 125 << 25 | 12]       # s_buffer_load_dword s7, s[8:11], 12


def udiv(divisor_sgpr):
    # v_cvt_f32_u32 v1, sD; v_rcp_f32 v1, v1; v_mul_f32 v1, 0x4f7ffffe, v1; v_cvt_u32_f32 v1, v1; v_readfirstlane s10, v1
    return [vop1(6, 1, divisor_sgpr), vop1(0x2a, 1, 257), vop2(8, 1, LITERAL, 1), 0x4f7ffffe, vop1(7, 1, 257),
            vop1(2, 10, 257)]


S_CMP_EQ_U32_S7_0 = 0xbf000000 | 6 << 16 | 128 << 8 | 7
S_MOV_B32_S12_1 = 0xbe800000 | 12 << 16 | 3 << 8 | 129


def stage(words):
    return G.Stage(TestPipeline(), 'vs', struct.pack('<%dI' % (len(words) + 1), *words, 0xbf810000))


def asm(words):
    return [x[3] for x in G.decode(struct.pack('<%dI' % len(words), *words))]


def refused(words, label):
    try:
        stage(words)
    except G.Unsupported as error:
        assert 'requires a proven uniform source' in str(error), (label, str(error))
        return
    raise AssertionError('accepted: ' + label)


def body(s):
    return '\n'.join(line for block in s.blocks for line in block[1])


def readfirstlane():
    words = SBL + udiv(7)
    assert asm(udiv(7))[:3] == ['v_cvt_f32_u32_e32 v1, s7', 'v_rcp_f32_e32 v1, v1', 'v_mul_f32_e32 v1, 0x4f7ffffe, v1'], asm(udiv(7))
    assert asm(udiv(7))[-1] == 'v_readfirstlane_b32 s10, v1'
    assert 's10 = v1;' in body(stage(words))
    # A scalar branch around an SGPR write before the division: the divisor s7 was loaded before the branch.
    branchy = SBL + [S_CMP_EQ_U32_S7_0, 0xbf840001, S_MOV_B32_S12_1] + udiv(7)  # s_cbranch_scc0 over the s_mov
    assert asm([0xbf840001])[0].startswith('s_cbranch_scc0'), asm([0xbf840001])
    assert 's10 = v1;' in body(stage(branchy))
    # Controls.
    refused(SBL + [S_CMP_EQ_U32_S7_0, 0xbf840001, S_MOV_B32_S12_1] + udiv(12),
            'divisor assigned on one side of a join')
    refused(SBL + udiv(7) + [0xbf85fff9], 'divisor behind a loop head')               # s_cbranch_scc1 back to udiv
    assert G.decode(struct.pack('<%dI' % len(SBL + udiv(7) + [0xbf85fff9]), *SBL, *udiv(7), 0xbf85fff9))[-1][3] \
        == 's_cbranch_scc1 label_0x8', 'loop branch target'
    refused(SBL + [vop2(1, 1, 128, 0x101 - 256)] + [vop1(2, 10, 257)], 'v_cndmask_b32 v1, 0, v1, vcc')
    refused(SBL + [vop1(6, 1, 7), 0xbf820000, vop1(2, 10, 257)], 'read at a branch target')
    refused(SBL + [vop1(6, 1, 256)] + udiv(7)[1:], 'per-lane source v0')
    print('readfirstlane: lane-invariant division accepted with and without a scalar branch; 5 controls refused')


def ref(kind, a, b, c):
    if kind == 'subrev':
        a, b = b, a
    if kind == 'add':
        total = a + b + c
        return total & MASK, total >> 32
    return (a - b - c) & MASK, int(b + c > a)


def carry_text(words):
    s = stage([])
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<%dI' % len(words), *words))[0]
    s.pc = 0
    s.one(name, text)
    return s, '\n'.join(s.lines)


def carries():
    grid = [0, 1, 2, 0x7fffffff, 0x80000000, 0xfffffffe, MASK, 0x12345678]
    bad = []
    forms = []
    for kind, op32, op64 in (('add', 0x28, 0x128), ('sub', 0x29, 0x129), ('subrev', 0x2a, 0x12a)):
        e32 = [vop2(op32, 2, 256 + 4, 3)]  # v_X_co_ci_u32_e32 v2, vcc_lo, v4, v3, vcc_lo
        assert asm(e32)[0] == 'v_%s_co_ci_u32_e32 v2, vcc_lo, v4, v3, vcc_lo' % kind, asm(e32)
        e64 = [0xd4000000 | op64 << 16 | 18 << 8 | 2, (256 + 4) | (256 + 3) << 9 | 106 << 18]
        assert asm(e64)[0] == 'v_%s_co_ci_u32_e64 v2, s18, v4, v3, vcc_lo' % kind, asm(e64)
        co_op = {'add': 0x30f, 'sub': 0x310, 'subrev': 0x319}[kind]
        co = [0xd4000000 | co_op << 16 | 18 << 8 | 2, (256 + 4) | (256 + 3) << 9]
        assert asm(co)[0] == 'v_%s_co_u32 v2, s18, v4, v3' % kind, asm(co)
        for words, carry_in, sdst in ((e32, True, 'vcc'), (e64, True, 's18'), (co, False, 's18')):
            s, text = carry_text(words)
            lines = text.split('\n')
            assert lines[0].startswith('q = uint2(') and lines[1] == 'v2 = q.x;', text
            assert lines[2] == ('vcc = q.y;' if sdst == 'vcc' else 's18 = q.y;'), text
            if sdst == 's18':
                assert 18 in s.masks, 'carry-out SGPR is a mask'
            value, carry = G.split_top(lines[0][len('q = uint2('):-2])
            value, carry = Expr(value), Expr(carry)
            for a in grid:
                for b in grid:
                    for c in ((0, 1) if carry_in else (0,)):
                        env = {'v4': a, 'v3': b, 'vcc': c}
                        got = value.run(env)[0], carry.run(env)[0]
                        if got != ref(kind, a, b, c):
                            bad.append((kind, words, a, b, c, got, ref(kind, a, b, c)))
            forms.append(asm(words)[0].split(' ')[0])
    assert not bad, bad[:5]
    # Refusals: clamp, and a carry-in that is not a lane mask.
    clamp = [0xd4000000 | 0x128 << 16 | 1 << 15 | 18 << 8 | 2, (256 + 4) | (256 + 3) << 9 | 106 << 18]
    numeric_ci = [0xd4000000 | 0x128 << 16 | 18 << 8 | 2, (256 + 4) | (256 + 3) << 9 | 20 << 18]  # CI = s20
    for words, why in ((clamp, 'clamp'), (numeric_ci, 'lane mask')):
        try:
            carry_text(words)
        except G.Unsupported as error:
            assert why in str(error), (why, str(error), asm(words))
        else:
            raise AssertionError('accepted %s' % asm(words))
    print('carry forms: %d forms match the 33-bit references on %d inputs each; 2 controls refused'
          % (len(forms), len(grid) ** 2 * 2))


if __name__ == '__main__':
    readfirstlane()
    carries()
