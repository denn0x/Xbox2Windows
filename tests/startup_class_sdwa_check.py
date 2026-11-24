"""v_cmp_class_f32_sdwa: a constant f32 value classified against a byte of a register (src1_sel:BYTE_1).

Synthetic VOPC SDWA words (word 0: src0 = 0xf9, vsrc1, op 0x88, 0x3e; word 1: src0, sdst | SD, src0_sel, src0_neg,
S0, src1_sel). A compiler tests single bits of a byte with it: class(-0.0) is mask bit 5, class(-denormal 0x80000001)
bit 4, class(0xffffffff, a quiet NaN) bit 1 (class bits: LLVM SIDefines.h ClassFlags, the order x2w_class_f32
uses). The emitted HLSL is checked as text and its value computed with a Python model of the class bits.
Controls: src0_sel other than DWORD is refused, and an integer SDWA compare keeps its old lowering.
"""
import math
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


def vopc_sdwa(op, src0, vsrc1, sdst, src0_sel=6, src1_sel=1, neg0=False, s0=True):
    w0 = 0xf9 | vsrc1 << 9 | op << 17 | 0x3e << 25
    w1 = src0 | (sdst | 0x80) << 8 | src0_sel << 16 | int(neg0) << 20 | int(s0) << 23 | src1_sel << 24
    return [w0, w1]


def one(words):
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<2I', *words))[0]
    s.pc = 0
    s.one(name, text)
    return text, '\n'.join(s.lines)


def cls(bits):
    """Class bit index of an f32 bit pattern: 0 sNaN, 1 qNaN, 2 -inf, 3 -normal, 4 -denormal, 5 -0, 6 +0, 7 +denormal,
    8 +normal, 9 +inf."""
    e, m, neg = bits >> 23 & 255, bits & 0x7fffff, bits >> 31
    if e == 255:
        return (1 if m & 0x400000 else 0) if m else (2 if neg else 9)
    if e == 0:
        return (4 if neg else 7) if m else (5 if neg else 6)
    return 3 if neg else 8


def main():
    cases = [(128, True, 'neg(0)', '(0x00000000u ^ 0x80000000u)', 5),
             (129, True, 'neg(1)', '(0x00000001u ^ 0x80000000u)', 4),
             (193, False, '-1', '0xffffffffu', 1)]
    for src0, neg, shown, value, bit in cases:
        text, body = one(vopc_sdwa(0x88, src0, 6, 38, neg0=neg))
        assert text == 'v_cmp_class_f32_sdwa s38, %s, v6 src0_sel:DWORD src1_sel:BYTE_1' % shown, text
        want = 's38 = x2w_class_f32(%s, ((v6 >> 8u) & 0xffu));' % value
        assert body == want, (body, want)
        bits = int(re.sub(r'u', '', value.strip('()').split(' ^ ')[0]), 16) ^ (0x80000000 if neg else 0)
        assert cls(bits) == bit, (shown, cls(bits))
        # the result is mask bit `bit` of byte 1 of v6
        for v6 in (0, 1 << (8 + bit), 0xff00 ^ 1 << (8 + bit), 0xffffffff):
            assert (((v6 >> 8) & 0xff) >> cls(bits) & 1) == (v6 >> (8 + bit) & 1)
    assert math.isnan(struct.unpack('<f', struct.pack('<I', 0xffffffff))[0])
    try:
        one(vopc_sdwa(0x88, 6, 7, 38, src0_sel=1, s0=False))  # src0 v6 BYTE_1
    except G.Unsupported as error:
        assert 'sub-dword f32 value' in str(error), str(error)
    else:
        raise AssertionError('sub-dword f32 class value accepted')
    text, body = one(vopc_sdwa(0x84, 6, 7, 38, src0_sel=0, src1_sel=0, s0=False))  # v_cmp_gt_i32 BYTE_0, BYTE_0
    assert text.startswith('v_cmp_gt_i32_sdwa s38, v6, v7'), text
    assert body == 's38 = ((int(((v6 >> 0u) & 0xffu)) > int(((v7 >> 0u) & 0xffu))) ? 1u : 0u);', body
    print('class sdwa: -0.0 / -denormal / NaN against byte 1 of v6 (bits 5, 4, 1); sub-dword value refused; '
          'integer SDWA control unchanged')


if __name__ == '__main__':
    main()
