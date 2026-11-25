"""s_ff1_i32_b32 and s_ff0_i32_b32 on a value that is not a lane mask.

Synthetic gfx1013 SOP1 words. Reference (AMD RDNA 2 ISA SOP1): S_FF1_I32_B32 D = the index of the lowest one bit
of S0, -1 if S0 has none; S_FF0_I32_B32 the same for the lowest zero bit; SCC is not written (LLVM
SOPInstructions.td: no SCC def). The emitted HLSL is evaluated with the integer evaluator of
startup_gfx2hlsl_bit_alu_check over an edge grid (its firstbitlow returns 0xffffffff for 0, as HLSL documents).
Controls (refused): s_ff1_i32_b32 of exec_lo and of a mask SGPR (a lane index the one-lane model cannot give).
Negative control: an expression without the -1 case (0 for no bit) is caught.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from startup_gfx2hlsl_bit_alu_check import Expr  # noqa: E402
from startup_scalar_bits_check import GRID, MASK, asm, lines_of, refused, rhs, sop1  # noqa: E402


def lowest(x):
    return (x & -x).bit_length() - 1 if x else MASK


def main():
    grid = GRID + [1 << n for n in range(32)] + [MASK ^ (1 << n) for n in range(32)]
    for name, op, ref in (('s_ff0_i32_b32', 0x11, lambda s: lowest(~s & MASK)), ('s_ff1_i32_b32', 0x13, lowest)):
        words = [sop1(op, 5, 6)]
        assert asm(words) == ['%s s5, s6' % name], asm(words)
        _, lines = lines_of(words)
        assert len(lines) == 1, lines  # no SCC write
        expr = rhs(lines[0], 's5')
        bad = [s for s in grid if Expr(expr).run({'s6': s})[0] & MASK != ref(s)]
        assert not bad, (name, expr, bad[:4])
        wrong = Expr('(%s == 0u ? 0u : %s)' % ('s6' if op == 0x13 else '~s6', expr))
        assert any(wrong.run({'s6': s})[0] & MASK != ref(s) for s in grid), name
    cmp_vcc = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129                   # v_cmp_lt_u32 vcc, 1, v0
    save = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106                      # s_and_saveexec_b64 s[20:21], vcc (s20 mask)
    refused([sop1(0x13, 5, 126)], 'lane mask', 's_ff1_i32_b32 of exec_lo')
    refused([cmp_vcc, save, sop1(0x13, 5, 20)], 'lane mask', 's_ff1_i32_b32 of a mask SGPR')
    print('scalar_ff: s_ff0/s_ff1_i32_b32 match the ISA reference on %d inputs (-1 for none, SCC kept); exec_lo and '
          'mask-SGPR sources refused; negative control caught' % len(grid))


if __name__ == '__main__':
    main()
