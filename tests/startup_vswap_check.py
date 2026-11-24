"""v_swap_b32 (VOP1 0x65): tmp = D; D = S0; S0 = tmp, in the active lanes.

Synthetic gfx1013 words only. Reference: AMD RDNA 2 ISA V_SWAP_B32 ("swap operands; input and output modifiers
not supported; this is an untyped operation"); LLVM VOP1Instructions.td defines V_SWAP_B32 for gfx9+ with outs
$vdst, $vdst1 and src0 tied to $vdst1, VOP1 only (no e64, DPP or SDWA form).

The emitted statements are run by a small interpreter (assignments and `if (exec & 1)` guards only) on a grid of
register values, with exec known (every lane writes) and unknown (the guard is honoured: exec 1 swaps, exec 0
leaves both registers). Checked: v_swap_b32 v1, v2 swaps; v_swap_b32 v3, v3 emits nothing; a swap whose source
is a lane-uniform constant leaves the destination uniform for v_readfirstlane_b32 while the temporary is not.
Refused (decoder): the VOP3A spelling, which llvm-mc rejects on gfx1013.
Negative control: the statements with their order changed (D written before tmp is taken) fail the reference.
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import stage, asm  # noqa: E402

GRID = [0, 1, 0x80000000, 0xffffffff, 0x12345678, 0x3f800000]


def vop1(op, d, s):
    return 0x7e000000 | d << 17 | op << 9 | s


def lines_of(words, exec_known=True):
    s = stage([])
    s.lines, s.pc, s.exec_known = [], 0, exec_known
    for _, _, name, text in G.decode(struct.pack('<%dI' % len(words), *words)):
        s.one(name, text)
    return s, s.lines


def run(lines, regs):
    regs = dict(regs)
    for line in lines:
        m = re.fullmatch(r'(if \(exec & 1\) )?(\w+) = (\w+);', line)
        assert m, line
        if m.group(1) and not regs['exec'] & 1:
            continue
        regs[m.group(2)] = regs[m.group(3)]
    return regs


def check(lines, exec_value):
    bad = 0
    for a in GRID:
        for b in GRID:
            got = run(lines, {'v1': a, 'v2': b, 'exec': exec_value, 'xswap': 0xdeadbeef})
            want = (b, a) if exec_value & 1 else (a, b)
            bad += (got['v1'], got['v2']) != want
    return bad


def main():
    swap = [vop1(0x65, 1, 256 + 2)]
    assert asm(swap) == ['v_swap_b32 v1, v2'], asm(swap)
    s, lines = lines_of(swap)
    assert lines == ['xswap = v1;', 'v1 = v2;', 'v2 = xswap;'], lines
    assert '    uint xswap = 0;' in s.declarations()
    assert check(lines, 1) == 0
    s, lines = lines_of(swap, exec_known=False)
    assert lines == ['xswap = v1;', 'if (exec & 1) v1 = v2;', 'if (exec & 1) v2 = xswap;'], lines
    assert check(lines, 1) == 0 and check(lines, 0) == 0
    # negative control: D written before the temporary is taken
    wrong = [lines[1], lines[0], lines[2]]
    assert check(wrong, 1) > 0, wrong

    s, lines = lines_of([vop1(0x65, 3, 256 + 3)])
    assert lines == [] and not s.uses_swap, lines

    # uniformity: v2 = 7 is lane-uniform; after the swap v1 holds it, v2 holds the old (non-uniform) v1
    s = stage([])
    s.lines, s.pc = [], 0
    s.lane_s = set()
    for _, _, name, text in G.decode(struct.pack('<2I', vop1(1, 2, 128 + 7), swap[0])):
        s.one(name, text)
    assert 1 in s.uniform_v and 2 not in s.uniform_v, s.uniform_v

    vop3 = [0xd5000000 | (0x180 + 0x65) << 16 | 1, 256 + 2]
    assert asm(vop3) == ['v_swap_b32 <unsupported VOP3A encoding>'], asm(vop3)
    try:
        lines_of(vop3)
    except G.Unsupported:
        pass
    else:
        raise AssertionError('VOP3A v_swap_b32 accepted')
    print('vswap: v_swap_b32 matches tmp/D/S0 on %d pairs with exec known, 1 and 0; self-swap emits nothing; '
          'uniformity follows the moved value; VOP3A refused; reordered statements caught' % len(GRID) ** 2)


if __name__ == '__main__':
    main()
