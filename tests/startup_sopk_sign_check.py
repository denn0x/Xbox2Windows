"""SOPK immediates: s_movk_i32 and s_cmpk_*_i32 sign-extend SIMM16, s_cmpk_*_u32 zero-extends (picked 56eee135).

Synthetic SOPK words (0xb0000000 | op << 23 | sdst << 16 | simm16). Reference: RDNA 2 ISA SOPK; LLVM
SOPInstructions.td s16imm for s_movk_i32 / s_cmpk_*_i32, u16imm for s_cmpk_*_u32. The disassembler prints the field
unsigned, so 0xf000 / 0x8000 are the cases that differ. Negative control: a lowering that keeps the printed value
(the old behaviour) gives 0x0000f000 for s_movk_i32 0xf000.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from startup_scalar_bits_check import asm, lines_of  # noqa: E402


def sopk(op, sdst, simm):
    return [0xb0000000 | op << 23 | sdst << 16 | simm]


def main():
    cases = [
        (0, 0xf000, 's_movk_i32 s5, 0xf000', ['s5 = 0xfffff000u;']),
        (0, 0x7fff, 's_movk_i32 s5, 0x7fff', ['s5 = 0x00007fffu;']),
        (3, 0x8000, 's_cmpk_eq_i32 s5, 0x8000', ['scc = int(s5) == int(0xffff8000u);']),
        (9, 0x8000, 's_cmpk_eq_u32 s5, 0x8000', ['scc = s5 == 0x00008000u;']),
    ]
    for op, simm, text, want in cases:
        words = sopk(op, 5, simm)
        assert asm(words) == [text], asm(words)
        s, lines = lines_of(words)
        assert lines == want, (text, lines)
    s, _ = lines_of(sopk(0, 5, 0xf000))
    assert s.sym.get(5) == ('const', 0xfffff000), s.sym.get(5)
    assert s.sym.get(5) != ('const', 0xf000)  # the old, unsigned value
    print('sopk_sign: s_movk_i32 / s_cmpk_eq_i32 sign-extend, s_cmpk_eq_u32 zero-extends; constant 0xfffff000 kept')


if __name__ == '__main__':
    main()
