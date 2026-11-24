"""s_bitset0/1_b32, s_bcnt0/1_i32_b32 and v_xor3_b32.

Synthetic gfx1013 words only. References written from the AMD RDNA 2 ISA descriptions:
- S_BITSET0_B32 / S_BITSET1_B32: D.u[S0.u[4:0]] = 0 / 1, the other bits of D kept; SCC not written (LLVM
  SOPInstructions.td: D tied as an input, no SCC def).
- S_BCNT0_I32_B32 / S_BCNT1_I32_B32: D = number of zero / one bits in S0; SCC = (D != 0).
- V_XOR3_B32 (gfx10 VOP3 0x178): D = S0 ^ S1 ^ S2.
The emitted HLSL is evaluated with the integer evaluator of startup_gfx2hlsl_bit_alu_check over an edge grid.
Controls (refused): s_bitset1_b32 into a lane-mask SGPR; s_bcnt1_i32_b32 of exec_lo and of a mask SGPR (a lane
count the one-lane model cannot give). Constant folding: s_bitset1_b32 on a constant SGPR keeps a constant.
Negative control: the evaluator finds a mismatch for a deliberately wrong expression of each form.
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
GRID = [0, 1, 2, 31, 32, 33, 0x7fffffff, 0x80000000, 0xfffffffe, MASK, 0x12345678, 0x0000ffff]


class TestPipeline:
    root = {'params': [{'type': 2}], 'flags': 0}

    def resource(self, key, kind):
        return 'r0'

    def ud(self, source):
        return 0


def sop1(op, dst, src):
    return 0xbe800000 | dst << 16 | op << 8 | src


def stage(words):
    return G.Stage(TestPipeline(), 'vs', struct.pack('<%dI' % (len(words) + 1), *words, 0xbf810000))


def asm(words):
    return [x[3] for x in G.decode(struct.pack('<%dI' % len(words), *words))]


def lines_of(words):
    """The lines one instruction emits, on a bare stage with nothing known about its registers."""
    s = stage([])
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<%dI' % len(words), *words))[0]
    s.pc = 0
    s.one(name, text)
    return s, s.lines


def rhs(line, reg):
    assert line.startswith('%s = ' % reg) and line.endswith(';'), line
    return line[len(reg) + 3:-1]


def refs():
    return {
        's_bitset0_b32': lambda d, s: d & ~(1 << (s & 31)) & MASK,
        's_bitset1_b32': lambda d, s: (d | 1 << (s & 31)) & MASK,
        's_bcnt0_i32_b32': lambda d, s: 32 - bin(s).count('1'),
        's_bcnt1_i32_b32': lambda d, s: bin(s).count('1'),
    }


def mismatches(expr, ref, scc_expr=None):
    bad = []
    for d in GRID:
        for s in GRID:
            got = expr.run({'s5': d, 's6': s})[0] & MASK
            if got != ref(d, s):
                bad.append((d, s, got, ref(d, s)))
            if scc_expr is not None:
                scc = bool(scc_expr.run({'s5': got})[0])
                if scc != (ref(d, s) != 0):
                    bad.append(('scc', d, s))
    return bad


def check_scalar():
    forms = 0
    for name, op in (('s_bcnt0_i32_b32', 0x0d), ('s_bcnt1_i32_b32', 0x0f), ('s_bitset0_b32', 0x1b),
                     ('s_bitset1_b32', 0x1d)):
        words = [sop1(op, 5, 6)]
        assert asm(words) == ['%s s5, s6' % name], asm(words)
        _, lines = lines_of(words)
        expr = rhs(lines[0], 's5')
        scc = None
        if name.startswith('s_bcnt'):
            assert len(lines) == 2 and lines[1] == 'scc = s5 != 0;', lines
            scc = Expr('s5 != 0')
        else:
            assert len(lines) == 1, lines  # no SCC write
        bad = mismatches(Expr(expr), refs()[name], scc)
        assert not bad, (name, expr, bad[:4])
        # negative control: a wrong expression is caught
        wrong = expr.replace('countbits', '31u - countbits') if 'countbits' in expr else expr.replace('& 31', '& 15')
        assert wrong != expr and mismatches(Expr(wrong), refs()[name]), (name, wrong)
        forms += 1
    # constant folding: s_mov_b32 s5, 0x10; s_bitset1_b32 s5, 3 -> constant 0x18
    words = [sop1(0x03, 5, 0x90), sop1(0x1d, 5, 0x83)]
    assert asm(words) == ['s_mov_b32 s5, 16', 's_bitset1_b32 s5, 3'], asm(words)
    s = stage(words)
    assert s.sym.get(5) == ('const', 0x18), s.sym.get(5)
    return forms


def check_xor3():
    words = [0xd4000000 | 0x178 << 16 | 2, 256 | 257 << 9 | 258 << 18]
    assert asm(words) == ['v_xor3_b32 v2, v0, v1, v2'], asm(words)
    _, lines = lines_of(words)
    expr = Expr(rhs(lines[0], 'v2'))
    for a in GRID:
        for b in GRID:
            for c in (0, 1, MASK, 0x5a5a5a5a):
                assert expr.run({'v0': a, 'v1': b, 'v2': c})[0] & MASK == a ^ b ^ c, (a, b, c)
    wrong = Expr(rhs(lines[0], 'v2').replace('^', '|', 1))
    assert any(wrong.run({'v0': a, 'v1': b, 'v2': 0})[0] != a ^ b for a in GRID for b in GRID)


def refused(words, why, label):
    try:
        stage(words)
    except G.Unsupported as error:
        assert why in str(error), (label, str(error))
        return
    raise AssertionError('accepted: ' + label)


def controls():
    cmp_vcc = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129                   # v_cmp_lt_u32 vcc, 1, v0
    save = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106                      # s_and_saveexec_b64 s[20:21], vcc (s20 mask)
    refused([cmp_vcc, save, sop1(0x1d, 20, 0x83)], 'destination', 's_bitset1_b32 into a mask SGPR')
    refused([sop1(0x0f, 5, 126)], 'lane mask', 's_bcnt1_i32_b32 of exec_lo')
    refused([cmp_vcc, save, sop1(0x0f, 5, 20)], 'lane mask', 's_bcnt1_i32_b32 of a mask SGPR')
    assert asm([sop1(0x0f, 5, 126)]) == ['s_bcnt1_i32_b32 s5, exec_lo'], asm([sop1(0x0f, 5, 126)])


def main():
    forms = check_scalar()
    check_xor3()
    controls()
    print('scalar_bits: %d scalar forms and v_xor3_b32 match the ISA references on %d inputs; constant fold kept; '
          '3 controls refused; negative controls caught' % (forms, len(GRID) ** 2))


if __name__ == '__main__':
    main()
