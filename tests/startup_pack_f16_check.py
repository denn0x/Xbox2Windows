"""v_pack_b32_f16 (VOP3): D[15:0] = S0.f16, D[31:16] = S1.f16 (AMD RDNA 2 ISA).

Synthetic VOP3 words (gfx10 opcode 0x311, op_sel in word 0 [14:11], abs [10:8], neg in word 1 [31:29]), names and
the printed op_sel / modifier forms asserted from the decoder. Measured forms: `v_pack_b32_f16 vD, vA, vB
op_sel:[1,0,0]` (14 title keys) and without op_sel (1). The emitted expression is evaluated in Python for every
op_sel[0..1] / neg / abs combination over sample register pairs and compared with a reference that selects the
half (op_sel[i] = high half of source i) and then applies abs (clear bit 15) and neg (flip bit 15).
Negative control: the halves swapped are caught. Refused: a destination op_sel bit, clamp.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def vop3(vdst, s0, s1, opsel=0, absm=0, neg=0, clamp=0):
    return [0xd4000000 | 0x311 << 16 | clamp << 15 | opsel << 11 | absm << 8 | vdst,
            (256 + s0) | (256 + s1) << 9 | neg << 29]


def one(words):
    s = G.Stage(Pipe(), 'vs', b'')
    s.lines, s.pc = [], 0
    _, n, name, text = G.decode(struct.pack('<2I', *words))[0]
    s.one(name, text)
    return text, s.lines


def ev(expr, env):
    py = re.sub(r'(0x[0-9a-f]+|\d+)u\b', r'\1', expr)
    return eval(py, {}, env) & 0xffffffff


def ref(a, b, sel, absm, neg):
    out = []
    for i, x in enumerate((a, b)):
        h = (x >> 16) & 0xffff if sel >> i & 1 else x & 0xffff
        if absm >> i & 1:
            h &= 0x7fff
        if neg >> i & 1:
            h ^= 0x8000
        out.append(h)
    return out[0] | out[1] << 16


def main():
    values = [0x00000000, 0x3c00bc00, 0x80017fff, 0xfc007e00, 0x12345678, 0xffff0001]
    cases = 0
    for sel in range(4):
        for absm in range(4):
            for neg in range(4):
                text, lines = one(vop3(3, 1, 2, sel, absm, neg))
                assert text.startswith('v_pack_b32_f16 v3, '), text
                assert ('op_sel:[%d,%d,0]' % (sel & 1, sel >> 1) in text) == bool(sel), text
                m = re.fullmatch(r'v3 = (.*);', lines[0])
                assert m and len(lines) == 1, lines
                for a in values:
                    for b in values:
                        assert ev(m.group(1), {'v1': a, 'v2': b}) == ref(a, b, sel, absm, neg), (text, hex(a), hex(b))
                        cases += 1
    _, lines = one(vop3(3, 1, 2, 1))
    swapped = re.fullmatch(r'v3 = (.*) \| \((.*) << 16\);', lines[0])
    wrong = '%s | (%s << 16)' % (swapped.group(2), swapped.group(1))
    assert any(ev(wrong, {'v1': a, 'v2': b}) != ref(a, b, 1, 0, 0) for a in values for b in values)
    for words, why in ((vop3(3, 1, 2, 4), 'form'), (vop3(3, 1, 2, clamp=1), 'modifier clamp')):
        try:
            one(words)
        except G.Unsupported as error:
            assert why in str(error), str(error)
        else:
            raise AssertionError('accepted: %s' % why)
    print('pack_f16: %d (op_sel, abs, neg, value) cases match the reference; swapped halves caught; destination '
          'op_sel and clamp refused' % cases)


if __name__ == '__main__':
    main()
