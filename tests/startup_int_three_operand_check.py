"""v_min3/v_max3/v_med3 _i32/_u32: the lowered HLSL expression against a sorted reference.

Synthetic VOP3 words (gfx10 opcodes 0x152..0x159 except the f32 ones; the decoder's names are asserted). The
expression the translator emits is evaluated with 32-bit int()/uint() semantics on a grid of signed and unsigned
edge values. Reference: RDNA2 ISA (min/max/median of the three sources as signed or unsigned 32-bit integers).
Control: a mutated reference (median replaced by the maximum) must disagree.
"""
import itertools
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402

MASK = 0xffffffff
OPS = {0x152: 'v_min3_i32', 0x153: 'v_min3_u32', 0x155: 'v_max3_i32', 0x156: 'v_max3_u32',
       0x158: 'v_med3_i32', 0x159: 'v_med3_u32'}
GRID = [0, 1, 2, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, MASK, 0x12345678]


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def s32(x):
    x &= MASK
    return x - (1 << 32) if x >> 31 else x


def run(expr, env):
    py = re.sub(r'\buint\(', 'U32(', re.sub(r'(?<![a-z])int\(', 'S32(', expr))
    return eval(py, {'__builtins__': {}, 'U32': lambda x: x & MASK, 'S32': s32, 'min': min, 'max': max}, env) & MASK


def ref(name, a, b, c):
    conv = s32 if name.endswith('_i32') else (lambda x: x)
    vals = sorted((conv(a), conv(b), conv(c)))
    pick = {'min3': vals[0], 'max3': vals[2], 'med3': vals[1]}[name.split('_')[1]]
    return pick & MASK


def main():
    for op, name in OPS.items():
        words = [0xd4000000 | op << 16 | 0, (256 + 1) | (256 + 2) << 9 | (256 + 3) << 18]
        _, n, dname, text = G.decode(struct.pack('<2I', *words))[0]
        assert text == '%s v0, v1, v2, v3' % name, text
        s = G.Stage(Pipe(), 'ps', b'')
        s.lines = []
        s.pc = 0
        s.one(dname, text)
        assert len(s.lines) == 1 and s.lines[0].startswith('v0 = ') and s.lines[0].endswith(';'), s.lines
        expr = s.lines[0][5:-1]
        bad = [(a, b, c) for a, b, c in itertools.product(GRID, repeat=3)
               if run(expr, {'v1': a, 'v2': b, 'v3': c}) != ref(name, a, b, c)]
        assert not bad, (name, expr, bad[:3])
    # Control: the median is not the maximum on this grid, so a wrong lowering would be caught.
    assert any(ref('v_med3_i32', a, b, c) != ref('v_max3_i32', a, b, c) for a, b, c in itertools.product(GRID, repeat=3))
    print('int three-operand: %d forms match the sorted reference on %d inputs each; median/max control differs'
          % (len(OPS), len(GRID) ** 3))


if __name__ == '__main__':
    main()
