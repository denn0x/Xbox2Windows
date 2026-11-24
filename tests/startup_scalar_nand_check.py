"""s_nand_b32 / s_nor_b32 / s_xnor_b32 (SOP2), the 32-bit forms of the existing b64 entries.

Synthetic words (SOP2: op [29:23], sdst [22:16], ssrc1 [15:8], ssrc0 [7:0]; gfx10 opcodes 0x18, 0x1a, 0x1c, names
asserted from the decoder). RDNA2 ISA: D = ~(S0 & S1), ~(S0 | S1), ~(S0 ^ S1); SCC = (D != 0). The emitted
expression is evaluated on a value grid; a lane-mask source (vcc_lo) is refused (SCC would need every lane's bit).
Control: the existing s_and_b32 output is unchanged.
"""
import itertools
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402

MASK = 0xffffffff
OPS = {0x18: ('s_nand_b32', lambda a, b: ~(a & b)), 0x1a: ('s_nor_b32', lambda a, b: ~(a | b)),
       0x1c: ('s_xnor_b32', lambda a, b: ~(a ^ b))}
GRID = [0, 1, 0x80000000, 0x0f0f0f0f, 0xfffffffe, MASK]


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def stage():
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    s.pc = 0
    return s


def run_one(s, word):
    _, n, name, text = G.decode(struct.pack('<I', word))[0]
    s.one(name, text)
    return text


def main():
    for op, (name, ref) in OPS.items():
        s = stage()
        text = run_one(s, 0x80000000 | op << 23 | 2 << 16 | 1 << 8 | 0)
        assert text == '%s s2, s0, s1' % name, text
        body = '\n'.join(s.lines)
        m = re.search(r's2 = (.*);', body)
        assert m and 'scc' in body, body
        py = m.group(1).replace('~', 'MASK ^ ').replace('u', '')
        for a, b in itertools.product(GRID, repeat=2):
            got = eval(py, {'__builtins__': {}, 'MASK': MASK}, {'s0': a, 's1': b}) & MASK
            assert got == ref(a, b) & MASK, (name, a, b, got)
        s = stage()
        try:  # S0 = vcc_lo, a lane mask: SCC needs every lane's bit (startup_scalar_complement_mask_check)
            run_one(s, 0x80000000 | op << 23 | 2 << 16 | 1 << 8 | 106)
        except G.Unsupported as error:
            assert 'on a lane mask' in str(error), str(error)
        else:
            raise AssertionError((name, 'lane-mask source accepted'))
    s = stage()
    assert run_one(s, 0x80000000 | 0x0e << 23 | 2 << 16 | 1 << 8) == 's_and_b32 s2, s0, s1'
    assert re.search(r's2 = s0 & s1;', '\n'.join(s.lines)), s.lines
    print('scalar nand/nor/xnor b32: %d forms match the reference on %d inputs; mask source refused'
          % (len(OPS), len(GRID) ** 2))


if __name__ == '__main__':
    main()
