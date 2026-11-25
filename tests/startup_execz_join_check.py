"""A join where one register is symbolic only on the s_cbranch_execz edge, then ANDed with exec in place.

Measured form (a title pixel stage): s13 holds a copy of an entry SGPR (no run-time value) on the path that
s_cbranch_execz takes, a compare mask on the fall-through, and after the join `s_and_b32 s13, s13, exec_lo`.
RDNA 2 ISA: S_CBRANCH_EXECZ branches when EXEC is 0; S_AND_B32 D = S0 & S1. On the taken path this lane's exec
bit is 0 in the one-lane model, so the AND gives 0 whatever s13 held; on the other path s13's run-time variable
is its value. The translation reads the run-time variable there instead of refusing the conflict.
Synthetic words: s_mov_b32 s13, s0 (s0 a root pointer); v_cmp_lt_u32 vcc, 1, v0; s_and_saveexec_b32 s20, vcc_lo;
s_cbranch_execz L; s_and_b32 s13, vcc_lo, exec_lo; s_nop 0; L: s_and_b32 s13, s13, exec_lo.
The emitted line must be the in-place AND of the run-time variable; its value on both paths (v0 = 0: branch
taken, s13 never assigned at run time, result 0; v0 = 2: s13 the mask, result 1) is checked.
Controls (refused): the same join reached by s_cbranch_scc0 (exec not known to be 0 there); exec written between
the join and the AND; a use other than the in-place AND (s_lshl_b32).
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import sop1, stage  # noqa: E402

CMP_VCC = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129      # v_cmp_lt_u32 vcc, 1, v0
SAVE32 = sop1(0x3c, 20, 106)                           # s_and_saveexec_b32 s20, vcc_lo
NOP = 0xbf800000


def sop2(op, dst, a, b):
    return 0x80000000 | op << 23 | dst << 16 | b << 8 | a


def program(branch_op=8, before_use=(), use=None):
    use = use if use is not None else sop2(0x0e, 13, 13, 126)        # s_and_b32 s13, s13, exec_lo
    return [sop1(3, 13, 0), CMP_VCC, SAVE32, 0xbf800000 | branch_op << 16 | 2,
            sop2(0x0e, 13, 106, 126), NOP] + list(before_use) + [use]


def refused(words, label):
    try:
        stage(words)
    except G.Unsupported as error:
        assert 'conflict' in str(error) or 'execzero' in str(error), (label, str(error))
        return 1
    raise AssertionError('accepted: ' + label)


def main():
    s = stage(program())
    body = [line for _, lines, _ in s.blocks for line in lines]
    assert 's13 = (s13 & exec) & 1;' in body, body
    for v0, want in ((0, 0), (2, 1)):
        exec_bit = int(1 < v0)
        # the two paths: taken (exec 0, s13 never assigned at run time) and fall-through (s13 the mask)
        s13 = 0x5678 if not exec_bit else 1
        assert (s13 & exec_bit) & 1 == want
    controls = refused(program(branch_op=4), 's_cbranch_scc0 edge')
    controls += refused(program(before_use=[sop1(3, 126, 20)]), 'exec written before the AND')
    controls += refused(program(use=sop2(0x1e, 5, 13, 0x81)), 's_lshl_b32 use')   # s_lshl_b32 s5, s13, 1
    print('execz_join: in-place AND with exec after an execz-only conflict reads the run-time variable (0 on the '
          'taken path, the mask on the other); %d controls refused' % controls)


if __name__ == '__main__':
    main()
