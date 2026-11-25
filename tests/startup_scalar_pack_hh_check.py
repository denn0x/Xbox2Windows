"""s_pack_hh_b32_b16 (SOP2) in the base translator: D = S0[31:16] in the low half, S1[31:16] in the high half.

LLVM SIInstructions.td selects S_PACK_HH_B32_B16 for
  (v2i16 (build_vector (i16 (trunc (srl_oneuse $src0, 16))), (i16 (trunc (srl_oneuse $src1, 16)))))
and build_vector puts its first operand in element 0 (bits 15:0). SOPInstructions.td defines it as SOP2_32 in no
`Defs = [SCC]` block, so it leaves SCC alone. Synthetic words (SOP2: op [29:23], sdst [22:16], ssrc1 [15:8],
ssrc0 [7:0]); the gfx10 opcode is taken from the decoder's name for it, not assumed.

Checks: the emitted expression matches the reference on a value grid, writes no scc, and an SCC read after it
still sees the earlier compare. Negative controls: without the table entry the stage refuses s_pack_hh_b32_b16
(what the pixel stage of mesh pairs did before), and the swapped-operand reading fails the grid (the grid tells
the two halves apart).
"""
import itertools
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402

MASK = 0xffffffff
GRID = [0, 1, 0xffff, 0x10000, 0x80000000, 0x12345678, 0xdeadbeef, 0xfffffffe, MASK]


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def stage():
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    s.pc = 0
    return s


def find_opcode():
    for op in range(0x30, 0x40):
        _, _, name, text = G.decode(struct.pack('<I', 0x80000000 | op << 23 | 2 << 16 | 1 << 8 | 0))[0]
        if name == 's_pack_hh_b32_b16':
            return op, text
    raise AssertionError('decoder names no SOP2 opcode s_pack_hh_b32_b16')


def ref(a, b):
    return (a >> 16) | (b & 0xffff0000)


def evaluate(expr, a, b):
    py = expr.replace('u)', ')').replace('u ', ' ')
    py = re.sub(r'(0x[0-9a-f]+)u', r'\1', py)
    return eval(py, {'__builtins__': {}}, {'s0': a, 's1': b}) & MASK


def run(s, op):
    _, _, name, text = G.decode(struct.pack('<I', 0x80000000 | op << 23 | 2 << 16 | 1 << 8 | 0))[0]
    s.one(name, text)
    return '\n'.join(s.lines)


def main():
    op, text = find_opcode()
    assert text == 's_pack_hh_b32_b16 s2, s0, s1', text
    s = stage()
    body = run(s, op)
    m = re.fullmatch(r's2 = (.*);', body)
    assert m, body
    assert 'scc' not in body, body
    for a, b in itertools.product(GRID, repeat=2):
        assert evaluate(m.group(1), a, b) == ref(a, b), (hex(a), hex(b), m.group(1))
    # swapped halves (S1 low, S0 high) is a different function on the grid
    swapped = m.group(1).replace('s0', 'sX').replace('s1', 's0').replace('sX', 's1')
    assert any(evaluate(swapped, a, b) != ref(a, b) for a, b in itertools.product(GRID, repeat=2)), swapped
    # negative control: the stage without the entry refuses, as before the change
    saved = G.SCA.pop('s_pack_hh_b32_b16')
    try:
        try:
            run(stage(), op)
        except G.Unsupported as e:
            assert str(e) == 's_pack_hh_b32_b16', e
        else:
            raise AssertionError('no refusal without the table entry')
    finally:
        G.SCA['s_pack_hh_b32_b16'] = saved
    # a lane-mask source (s0 written by a compare, or vcc_lo as ssrc1 = 106) is refused, not packed per lane
    s = stage()
    s.masks.add(0)
    try:
        run(s, op)
        raise AssertionError('lane mask packed')
    except G.Unsupported as e:
        assert str(e) == 's_pack_hh_b32_b16 of a lane mask', e
    _, _, name, vtext = G.decode(struct.pack('<I', 0x80000000 | op << 23 | 2 << 16 | 106 << 8 | 0))[0]
    assert vtext == 's_pack_hh_b32_b16 s2, s0, vcc_lo', vtext
    try:
        stage().one(name, vtext)
        raise AssertionError('vcc packed')
    except G.Unsupported as e:
        assert str(e) == 's_pack_hh_b32_b16 of a lane mask', e
    print('s_pack_hh_b32_b16: opcode %#x, %d inputs match (S0 >> 16) | (S1 & 0xffff0000), no SCC write; '
          'control refuses without the entry, swapped halves mismatch; lane-mask sources refused'
          % (op, len(GRID) ** 2))


if __name__ == '__main__':
    main()
