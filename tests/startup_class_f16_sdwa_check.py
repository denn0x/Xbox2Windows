"""v_cmp_class_f16_sdwa: a binary16 value selected from a register, classified against a class-mask register.

Synthetic VOPC SDWA words (word 0: src0 = 0xf9, vsrc1, op 0x8f = v_cmp_class_f16, 0x3e; word 1: src0, sdst | SD,
src0_sel, src0_neg, src0_abs, S0, src1_sel). SDWA source selection (AMD RDNA ISA, SDWA): the selected field is
zero-extended and the f16 op reads bits [15:0], so WORD_1 classifies the high half; neg/abs act on bit 15 of the
selected value. The emitted HLSL is checked as text, and its value is evaluated with a Python model of the ten class
bits (LLVM SIDefines.h ClassFlags order) for every field selection over sample halves.
Controls: a constant value is refused (its f16 inline form is not modelled); v_cmp_class_f32_sdwa is unchanged.
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


def vopc_sdwa(op, src0, vsrc1, sdst, src0_sel=6, src1_sel=6, neg0=False, abs0=False, s0=False):
    w0 = 0xf9 | vsrc1 << 9 | op << 17 | 0x3e << 25
    w1 = src0 | (sdst | 0x80) << 8 | src0_sel << 16 | int(neg0) << 20 | int(abs0) << 21 | int(s0) << 23 \
        | src1_sel << 24
    return [w0, w1]


def one(words):
    s = G.Stage(Pipe(), 'vs', b'')
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<2I', *words))[0]
    s.pc = 0
    s.one(name, text)
    return text, '\n'.join(s.lines)


def cls16(h):
    e, m, neg = h >> 10 & 31, h & 0x3ff, h >> 15 & 1
    if e == 31:
        return (1 if m & 0x200 else 0) if m else (2 if neg else 9)
    if e == 0:
        return (4 if neg else 7) if m else (5 if neg else 6)
    return 3 if neg else 8


def evaluate(expr, v1, v2):
    """Evaluate the emitted uint expression in Python (x2w_class_f16 by the model, HLSL uint ops as Python ints)."""
    py = expr.replace('u)', ')').replace('u ', ' ')
    py = re.sub(r'(0x[0-9a-f]+|\d+)u\b', r'\1', py)
    return eval(py, {'v1': v1, 'v2': v2, 'x2w_class_f16': lambda x, mask: mask >> cls16(x & 0xffff) & 1})


def main():
    halves = [0x0000, 0x8000, 0x0001, 0x8001, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0x7d00, 0x03ff, 0xfbff]
    sels = {0: 'BYTE_0', 1: 'BYTE_1', 4: 'WORD_0', 5: 'WORD_1', 6: 'DWORD'}
    checked = 0
    for sel, name in sels.items():
        for neg, ab in ((False, False), (True, False), (False, True), (True, True)):
            text, body = one(vopc_sdwa(0x8f, 1, 2, 38, src0_sel=sel, neg0=neg, abs0=ab))
            shown = '-|v1|' if neg and ab else '-v1' if neg else '|v1|' if ab else 'v1'
            assert text == 'v_cmp_class_f16_sdwa s38, %s, v2 src0_sel:%s src1_sel:DWORD' % (shown, name), text
            m = re.fullmatch(r's38 = (.*);', body)
            assert m and 'x2w_class_f16(' in body, body
            for lo in halves:
                for hi in halves:
                    v1 = hi << 16 | lo
                    off, width = G.SDWA_SEL[name]
                    field = (v1 >> off) & ((1 << width) - 1)
                    h = field & 0xffff
                    h = h & 0x7fff if ab else h  # abs first, then neg (-|x| is negative)
                    h = h ^ 0x8000 if neg else h
                    for bit in range(10):
                        assert evaluate(m.group(1), v1, 1 << bit) == int(cls16(h) == bit), (name, neg, ab, hex(v1))
                        checked += 1
    _, body = one(vopc_sdwa(0x8f, 1, 2, 38, src0_sel=5))
    assert body == 's38 = x2w_class_f16(((v1 >> 16u) & 0xffffu), v2);', body
    try:
        one(vopc_sdwa(0x8f, 240, 2, 38, s0=True))  # constant 0.5
    except G.Unsupported as error:
        assert 'f16 value operand' in str(error), str(error)
    else:
        raise AssertionError('constant f16 class value accepted')
    text, body = one(vopc_sdwa(0x88, 128, 6, 38, src1_sel=1, neg0=True, s0=True))
    assert body == 's38 = x2w_class_f32((0x00000000u ^ 0x80000000u), ((v6 >> 8u) & 0xffu));', body
    print('class_f16 sdwa: %d (selection, modifier, value, class) cases match the class model; WORD_1 reads the '
          'high half; constant value refused; f32 control unchanged' % checked)


if __name__ == '__main__':
    main()
