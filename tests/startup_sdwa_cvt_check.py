"""SDWA forms of the float-result conversions (v_cvt_f32_u32, _i32, _ubyte*, _f16) after #1707 moved them out of VEC_U.

Synthetic VOP1 SDWA words: v_cvt_f32_u32 v2, v1 with src0_sel BYTE_1 (op 6, src0 = 0xf9, SDWA dword: v1, dst_sel DWORD,
UNUSED_PAD, src0_sel BYTE_1). The lowering must be the expression VEC_U used to give, U(float(<selected field>)), as
before the integration merge; v_cvt_f32_f16 likewise with WORD_1. An SDWA output modifier stays refused.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
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
    s.used_v.update((1, 2))
    s.one(name, text)
    return text, '\n'.join(s.lines)


def sdwa(op, src_sel, omod=0):
    return [0x7e000000 | 2 << 17 | op << 9 | 0xf9, 1 | 6 << 8 | omod << 14 | src_sel << 16]


def main():
    text, body = one(sdwa(6, 1))
    assert text.startswith('v_cvt_f32_u32_sdwa v2, v1') and 'src0_sel:BYTE_1' in text, text
    assert body == 'v2 = U(float(((v1 >> 8u) & 0xffu)));', body
    text, body = one(sdwa(0x0b, 5))  # v_cvt_f32_f16, WORD_1
    assert text.startswith('v_cvt_f32_f16_sdwa v2, v1') and 'src0_sel:WORD_1' in text, text
    assert body == 'v2 = U(f16tof32(((v1 >> 16u) & 0xffffu)));', body
    try:
        one(sdwa(6, 1, omod=1))
    except G.Unsupported as error:
        assert 'SDWA modifier mul:2' in str(error), str(error)
    else:
        raise AssertionError('SDWA output modifier accepted')
    print('sdwa cvt: v_cvt_f32_u32 BYTE_1 and v_cvt_f32_f16 WORD_1 lowered as before; mul:2 refused')


if __name__ == '__main__':
    main()
