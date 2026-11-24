"""f16 halves of literals in VOP3P sources: v_fma_mix_* with a 32-bit literal, v_pk_* with the inline integer 0.

Synthetic words. A VOP3P literal is read through op_sel like a register: LLVM SIFoldOperands::tryFoldImmWithOpSel
takes a packed operand's halves as ImmVal >> (OP_SEL_0 ? 16 : 0) and ImmVal >> (OP_SEL_1 ? 16 : 0), which is how a
compiler encodes two different f16 constants in the one literal (here 0x4600 = 6.0 low, 0xc200 = -3.0 high:
6 * |x| - 3). The inline integer 0 has no set bit, so every half selection of it reads +0.0. Controls: a literal
read as an f32 mix operand keeps its full 32 bits, and a non-zero inline constant in a packed f16 operand with a
high-half selection stays refused.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    root = {'params': [], 'flags': 0}

    def ud(self, source):
        return 0


def one(words):
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines = []
    _, n, name, text = G.decode(struct.pack('<%dI' % len(words), *words))[0]
    s.pc = 0
    s.one(name, text)
    return text, '\n'.join(s.lines)


def half(bits):
    return struct.unpack('<e', struct.pack('<H', bits))[0]


def main():
    mix = [0xcc000000 | 0x20 << 16 | 1 << 14 | 1 << 13 | 1 << 9 | 20, 255 | (256 + 14) << 9 | 255 << 18 | 1 << 27,
           0xc2004600]
    text, body = one(mix)
    assert text == 'v_fma_mix_f32 v20, 0xc2004600, |v14|, 0xc2004600 op_sel:[0,0,1] op_sel_hi:[1,0,1]', text
    assert body == 'v20 = U(f16tof32(0x4600u) * abs(F(v14)) + f16tof32(0xc200u));', body
    assert (half(0x4600), half(0xc200)) == (6.0, -3.0)
    # op_sel_hi[0] = 0: the literal is an f32 operand and keeps all 32 bits.
    f32 = [mix[0], mix[1] & ~(1 << 27), mix[2]]
    text, body = one(f32)
    assert body.startswith('v20 = U(F(0xc2004600u) * '), (text, body)

    pk = [0xcc000000 | 0x12 << 16 | 14, (256 + 14) | 128 << 9 | 1 << 27]
    text, body = one(pk)
    assert text == 'v_pk_max_f16 v14, v14, 0 op_sel_hi:[1,0]', text
    assert body == ('v14 = f32tof16(max(f16tof32(v14), f16tof32(0x00000000u))) | '
                    '(f32tof16(max(f16tof32(v14 >> 16), f16tof32(0x00000000u))) << 16);'), body
    # inline 1.0 (242) read through its high half: its packed layout is not pinned down here, so refused
    one_hi = [0xcc000000 | 0x12 << 16 | 14, (256 + 14) | 242 << 9 | 1 << 27 | 1 << 28]
    try:
        one(one_hi)
    except G.Unsupported as error:
        assert 'packed f16 operand 1.0' in str(error), str(error)
    else:
        raise AssertionError('accepted %s' % one(one_hi)[0])
    print('literal halves: fma_mix literal halves 6.0 / -3.0, f32 literal control, pk inline 0, inline 1.0 refused')


if __name__ == '__main__':
    main()
