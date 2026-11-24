"""s_nand_b32, s_nor_b32, s_xnor_b32 on a lane mask or on descriptor data: refused.

Synthetic gfx1013 SOP2 words only. AMD RDNA 2 ISA: D = ~(S0 op S1) on 32 bits, SCC = (D != 0). A vcc_lo/exec_lo
operand or destination, or an SGPR that holds a compare mask, is the whole wave's mask; SCC then depends on every
lane's bit of the complemented result, and the one-lane model keeps only this lane's bit, so these forms stay
refused. A source the translator holds as descriptor data (sym desc/descnum/ptr) takes the symbolic path, which
emits no SCC; the complement there is refused too. (Same rule as upstream 9b620015 and 34105de4, re-implemented.)

Accepted control: numeric SGPR operands give ~(s11 op s12) and SCC = D != 0, checked with the integer evaluator of
startup_gfx2hlsl_bit_alu_check on an edge grid. Negative control: the evaluator catches the expression without ~.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import disasm_rdna2 as D  # noqa: E402
import gfx2hlsl as G  # noqa: E402
from startup_gfx2hlsl_bit_alu_check import Expr  # noqa: E402
from startup_scalar_bits_check import stage, asm, sop1  # noqa: E402

MASK = 0xffffffff
OPCODE = {name: op for op, (name, _) in D.SOP2_OPS.items()}
REF = {'s_nand_b32': lambda a, b: ~(a & b) & MASK, 's_nor_b32': lambda a, b: ~(a | b) & MASK,
       's_xnor_b32': lambda a, b: ~(a ^ b) & MASK}
GRID = [0, 1, 0x7fffffff, 0x80000000, 0xfffffffe, MASK, 0xaaaaaaaa, 0x55555555, 0x12345678]
VCC_LO, EXEC_LO = 106, 126
CMP_VCC = 0x7c000000 | 0xc1 << 17 | 0 << 9 | 129                    # v_cmp_lt_u32 vcc, 1, v0
SAVE = 0xbe800000 | 20 << 16 | 0x24 << 8 | 106                      # s_and_saveexec_b64 s[20:21], vcc (s20 mask)


def sop2(name, sdst, s0, s1):
    return 0x80000000 | OPCODE[name] << 23 | sdst << 16 | s1 << 8 | s0


def one(words, sym=None):
    s = stage([])
    s.lines = []
    s.sym.update(sym or {})
    for _, _, name, text in G.decode(struct.pack('<%dI' % len(words), *words)):
        s.one(name, text)
    return s.lines


def refused(words, why, label, sym=None):
    try:
        one(words, sym) if sym else stage(words)
    except G.Unsupported as error:
        assert why in str(error), (label, str(error))
        return 1
    raise AssertionError('accepted: ' + label)


def main():
    accepted = refusals = 0
    for name, ref in REF.items():
        words = [sop2(name, 13, 11, 12)]
        assert asm(words) == ['%s s13, s11, s12' % name], asm(words)
        lines = one(words)
        assert len(lines) == 2 and lines[0].startswith('s13 = ') and lines[1] == 'scc = s13 != 0;', lines
        expr = Expr(lines[0][len('s13 = '):-1])
        for a in GRID:
            for b in GRID:
                got = expr.run({'s11': a, 's12': b})[0] & MASK
                assert got == ref(a, b), (name, a, b, got)
        wrong = Expr(lines[0][len('s13 = '):-1].replace('~', '', 1))
        assert any(wrong.run({'s11': a, 's12': b})[0] & MASK != ref(a, b) for a in GRID for b in GRID), name
        accepted += 1

        assert asm([sop2(name, 13, VCC_LO, 12)]) == ['%s s13, vcc_lo, s12' % name]
        refusals += refused([sop2(name, 13, VCC_LO, 12)], 'on a lane mask', name + ' of vcc_lo')
        refusals += refused([sop2(name, 13, 11, EXEC_LO)], 'on a lane mask', name + ' of exec_lo')
        refusals += refused([sop2(name, VCC_LO, 11, 12)], 'on a lane mask', name + ' into vcc_lo')
        refusals += refused([sop2(name, EXEC_LO, 11, 12)], 'on a lane mask', name + ' into exec_lo')
        refusals += refused([CMP_VCC, SAVE, sop2(name, 13, 20, 12)], 'on a lane mask', name + ' of a mask SGPR')
        refusals += refused([sop2(name, 13, 6, 12)], 'scc from descriptor data', name + ' of a descriptor word',
                            sym={6: ('descnum',)})
    # the sibling s_and_b32 of a descriptor word keeps its symbolic path (not refused)
    lines = one([sop2('s_and_b32', 13, 6, 12)], sym={6: ('descnum',)})
    assert lines == [], lines
    print('scalar_complement_mask: %d numeric forms match ~(a op b) and SCC on %d inputs (negative control caught); '
          '%d lane-mask / descriptor forms refused; s_and_b32 of a descriptor word unchanged'
          % (accepted, len(GRID) ** 2, refusals))


if __name__ == '__main__':
    main()
