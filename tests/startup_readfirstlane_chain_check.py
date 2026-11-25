"""v_readfirstlane_b32 of a VGPR computed under a partial exec by a chain of writes from one scalar.

Synthetic gfx1013 words only. The shape is the unsigned-division reciprocal that compilers emit for a division by
an SGPR value inside a branch (the largest readfirstlane refusal in the translator census):

    s_and_saveexec_b64 s[20:21], vcc
    v_cvt_f32_u32 v1, s22 ; v_rcp_f32 v1, v1 ; v_mul_f32 v1, 0x4f7ffffe, v1 ; v_cvt_u32_f32 v1, v1
    v_readfirstlane_b32 s23, v1

Every write of v1 changes only the active lanes, but each one reads only s22 and the v1 the previous write left in
those same lanes, so the first active lane holds the value computed from s22, as long as exec is not written
between the first write and the read. This is the scalar recompute of upstream PR #2203 (lower the read by
recomputing the scalar expression that defined the source) in the translator's one-lane model: each write of the
chain also assigns a shadow xv1 in every lane from the previous shadow, and the SGPR takes the last shadow.

1. The chain above: four lines `xv1 = <shadow>; if (exec & 1) v1 = xv1;`, the first reading s22, each later one
   reading xv1, and `s23 = xv1;`. The shadow is declared `uint xv1 = 0;`.
2. A two-register chain (v_cvt_f32_u32 v1, s22; v_rcp_f32 v2, v1): both v1 and v2 get shadows, xv2 reads xv1.
Controls (refused): a link that reads a per-lane value (v_mul_f32 v1, v0, v1); exec written inside the chain
(epoch changes); exec written between the chain and the read; a branch target inside the chain.
Negative control of the change: with partial_shadow limited to lane-invariant expressions (the earlier rule, which
accepted one write only), case 1 is refused again.
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class TestPipeline:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def vop1(op, vdst, src0):
    return 0x7e000000 | vdst << 17 | op << 9 | src0


def vop2(op, vdst, src0, vsrc1):
    return op << 25 | vdst << 17 | vsrc1 << 9 | src0


V = lambda n: 256 + n  # noqa: E731  VGPR source operand
CVT_F32_U32, CVT_U32_F32, RCP_F32, READFIRSTLANE, MOV = 6, 7, 0x2a, 2, 1
MUL_F32 = 8
LITERAL = 255

V_CMP_LT_U32_VCC_V0 = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129     # v_cmp_lt_u32 vcc, 1, v0
AND_SAVEEXEC = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106           # s_and_saveexec_b64 s[20:21], vcc
AND_SAVEEXEC2 = 0xbe800000 | 24 << 16 | 0x24 << 8 | 106          # s_and_saveexec_b64 s[24:25], vcc
RESTORE_EXEC = 0xbe800000 | 126 << 16 | 0x04 << 8 | 20           # s_mov_b64 exec, s[20:21]
NOP_BRANCH = 0xbf820000                                          # s_branch to the next instruction
READ = vop1(READFIRSTLANE, 23, V(1))                             # v_readfirstlane_b32 s23, v1 (vdst is the SGPR)
READ_V2 = vop1(READFIRSTLANE, 23, V(2))                          # v_readfirstlane_b32 s23, v2
USE = vop1(MOV, 3, 23)                                           # v_mov_b32 v3, s23

C1 = vop1(CVT_F32_U32, 1, 22)                                    # v_cvt_f32_u32 v1, s22
C2 = vop1(RCP_F32, 1, V(1))                                      # v_rcp_f32 v1, v1
C3 = [vop2(MUL_F32, 1, LITERAL, 1), 0x4f7ffffe]                  # v_mul_f32 v1, 0x4f7ffffe, v1
C3_LANE = vop2(MUL_F32, 1, V(0), 1)                              # v_mul_f32 v1, v0, v1
C4 = vop1(CVT_U32_F32, 1, V(1))                                  # v_cvt_u32_f32 v1, v1
ENTER = [V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC]
CHAIN = [C1, C2] + C3 + [C4]


def stage(words):
    return G.Stage(TestPipeline(), 'vs', struct.pack('<%dI' % (len(words) + 1), *words, 0xbf810000))


def asm(words):
    return [x[3] for x in G.decode(struct.pack('<%dI' % len(words), *words))]


def body(s):
    return [line for block in s.blocks for line in block[1]]


def refused(words, label):
    try:
        stage(words)
    except G.Unsupported as error:
        assert 'requires a proven uniform source' in str(error), (label, str(error))
        return
    raise AssertionError('accepted: ' + label)


def main():
    got = asm(ENTER + CHAIN + [READ, USE, READ_V2, C3_LANE])
    assert got == ['v_cmp_lt_u32_e32 vcc_lo, 1, v0', 's_and_saveexec_b64 s[20:21], vcc', 'v_cvt_f32_u32_e32 v1, s22',
                   'v_rcp_f32_e32 v1, v1', 'v_mul_f32_e32 v1, 0x4f7ffffe, v1', 'v_cvt_u32_f32_e32 v1, v1',
                   'v_readfirstlane_b32 s23, v1', 'v_mov_b32_e32 v3, s23', 'v_readfirstlane_b32 s23, v2',
                   'v_mul_f32_e32 v1, v0, v1'], got

    # 1. the reciprocal chain
    s = stage(ENTER + CHAIN + [READ, USE])
    lines = body(s)
    shadowed = [line for line in lines if line.startswith('xv1 = ')]
    assert len(shadowed) == 4, lines
    for line in shadowed:
        assert line.endswith('; if (exec & 1) v1 = xv1;'), line
    assert 's22' in shadowed[0] and not re.search(r'\bv\d+\b', shadowed[0].split(';')[0]), shadowed
    for line in shadowed[1:]:
        rhs = line.split(';')[0]
        assert 'xv1' in rhs[len('xv1 = '):] and not re.search(r'\bv\d+\b', rhs), line
    assert 's23 = xv1;' in lines, lines
    assert lines.index(shadowed[-1]) < lines.index('s23 = xv1;'), lines
    assert 'uint xv1 = 0;' in '\n'.join(s.declarations()), s.declarations()

    # 2. a chain through two registers
    two = ENTER + [C1, vop1(RCP_F32, 2, V(1)), READ_V2, USE]
    lines = body(stage(two))
    first = [line for line in lines if line.startswith('xv1 = ')]
    second = [line for line in lines if line.startswith('xv2 = ')]
    assert len(first) == 1 and len(second) == 1, lines
    assert 'xv1' in second[0].split(';')[0][len('xv2 = '):], second
    assert lines.index(first[0]) < lines.index(second[0]) < lines.index('s23 = xv2;'), lines

    # Controls.
    refused(ENTER + [C1, C2, C3_LANE, C4, READ], 'a link reads a per-lane VGPR')
    refused(ENTER + [C1, C2, V_CMP_LT_U32_VCC_V0, AND_SAVEEXEC2] + C3 + [C4, READ], 'exec written inside the chain')
    refused(ENTER + CHAIN + [RESTORE_EXEC, READ], 'exec written between the chain and the read')
    refused(ENTER + [C1, C2, NOP_BRANCH] + C3 + [C4, READ], 'branch target inside the chain')

    # Negative control: the earlier rule (a shadow only for an expression with no VGPR at all).
    original = G.Stage.partial_shadow

    def single(self, expr):
        return (expr, []) if self.lane_invariant(expr) else None
    G.Stage.partial_shadow = single
    try:
        refused(ENTER + CHAIN + [READ, USE], 'chain shadow disabled')
    finally:
        G.Stage.partial_shadow = original
    print('readfirstlane_chain: 4-write reciprocal chain and a 2-register chain read via shadows xv1/xv2; '
          '4 controls refused; negative control (single-write rule) refused')


if __name__ == '__main__':
    main()
