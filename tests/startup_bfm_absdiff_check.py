"""v_bfm_b32 (VOP3) and s_absdiff_i32 (SOP2).

Synthetic gfx1013 words. References (AMD RDNA 2 ISA): V_BFM_B32 D = ((1 << S0[4:0]) - 1) << S1[4:0];
S_ABSDIFF_I32 D = S0 - S1 (32-bit wrap), negated if negative, SCC = (D != 0) (LLVM SOPInstructions.td Defs = [SCC]).
The emitted HLSL is evaluated with the integer evaluator of startup_gfx2hlsl_bit_alu_check (abs added here with
HLSL int semantics: abs(INT_MIN) = INT_MIN) over an edge grid. Negative controls: a wrong expression of each form
(the shift count masked to 4 bits; abs dropped) is caught.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from startup_gfx2hlsl_bit_alu_check import Expr  # noqa: E402
from startup_scalar_bits_check import GRID, MASK, asm, lines_of, rhs  # noqa: E402


class AbsExpr(Expr):
    @staticmethod
    def call(name, args):
        if name == 'abs':
            v, s = args[0]
            assert s
            x = v - (1 << 32) if v >> 31 else v
            return (-x if x < 0 else x) & MASK, True
        return Expr.call(name, args)


def s32(x):
    return x - (1 << 32) if x >> 31 else x


def main():
    grid = GRID + [33 << 16 | 5, 0x1f001f, 0x3f]
    words = [0xd4000000 | 0x363 << 16 | 2, 256 | 257 << 9]
    assert asm(words) == ['v_bfm_b32 v2, v0, v1'], asm(words)
    _, lines = lines_of(words)
    expr = rhs(lines[0], 'v2')
    ref = lambda a, b: (((1 << (a & 31)) - 1) << (b & 31)) & MASK  # noqa: E731
    bad = [(a, b) for a in grid for b in grid if Expr(expr).run({'v0': a, 'v1': b})[0] & MASK != ref(a, b)]
    assert not bad, (expr, bad[:4])
    wrong = Expr(expr.replace('{0} & 31'.format('v0'), 'v0 & 15'))
    assert any(wrong.run({'v0': a, 'v1': b})[0] & MASK != ref(a, b) for a in grid for b in grid)
    words = [0x80000000 | 0x2c << 23 | 5 << 16 | 7 << 8 | 6]
    assert asm(words) == ['s_absdiff_i32 s5, s6, s7'], asm(words)
    _, lines = lines_of(words)
    assert len(lines) == 2 and lines[1] == 'scc = s5 != 0;', lines
    expr = rhs(lines[0], 's5')
    ref = lambda a, b: abs(s32((a - b) & MASK)) & MASK  # noqa: E731
    bad = [(a, b) for a in grid for b in grid if AbsExpr(expr).run({'s6': a, 's7': b})[0] & MASK != ref(a, b)]
    assert not bad, (expr, bad[:4])
    assert ref(0x80000000, 0) == 0x80000000
    wrong = AbsExpr(expr.replace('abs(', '('))
    assert any(wrong.run({'s6': a, 's7': b})[0] & MASK != ref(a, b) for a in grid for b in grid)
    print('bfm_absdiff: v_bfm_b32 and s_absdiff_i32 (SCC = D != 0) match the ISA references on %d inputs; '
          'negative controls caught' % len(grid) ** 2)


if __name__ == '__main__':
    main()
