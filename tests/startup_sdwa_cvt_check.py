"""SDWA forms of the float-result conversions (v_cvt_f32_u32, _i32, _ubyte*, _f16) after #1707 moved them out of VEC_U.

Synthetic VOP1 SDWA words: v_cvt_f32_u32 v2, v1 with src0_sel BYTE_1 (op 6, src0 = 0xf9, SDWA dword: v1, dst_sel DWORD,
UNUSED_PAD, src0_sel BYTE_1). The lowering must be the expression VEC_U used to give, U(float(<selected field>)), as
before the integration merge; v_cvt_f32_f16 likewise with WORD_1. SDWA output modifiers (omod, clamp) on these
conversions are lowered with dst_sel DWORD (they used to be refused; that control is now an acceptance check) and
stay refused with a partial destination field.
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
    # Output modifiers (SDWA9 omod [15:14], clamp [13]) on a float-result conversion with dst_sel DWORD: the
    # binary32 result is scaled, then clamped, as in VOP3 (set_vf).
    for omod, factor in ((1, '2.0'), (2, '4.0'), (3, '0.5')):
        text, body = one(sdwa(6, 4, omod=omod))
        assert text.startswith('v_cvt_f32_u32_sdwa v2, v1') and 'src0_sel:WORD_0' in text, text
        assert body == 'v2 = U((float(((v1 >> 0u) & 0xffffu))) * %s);' % factor, body
    text, body = one([sdwa(6, 4, omod=1)[0], sdwa(6, 4, omod=1)[1] | 1 << 13])
    assert 'clamp' in text, text
    assert body == 'v2 = U(saturate((float(((v1 >> 0u) & 0xffffu))) * 2.0));', body
    # Control: an output modifier with a partial destination field. (On an integer operation such as v_not_b32
    # the disassembler itself rejects the omod bits as an invalid SDWA encoding.)
    for words, why in (([0x7e000000 | 2 << 17 | 6 << 9 | 0xf9, 1 | 4 << 8 | 1 << 14 | 4 << 16], 'dst_sel:WORD_0'),):
        try:
            one(words)
        except G.Unsupported as error:
            assert why in str(error), (why, str(error))
        else:
            raise AssertionError('accepted %s' % one(words)[0])
    print('sdwa cvt: v_cvt_f32_u32 BYTE_1 and v_cvt_f32_f16 WORD_1 lowered as before; mul:2/mul:4/div:2/clamp '
          'lowered with dst_sel DWORD; 1 control refused')


if __name__ == '__main__':
    main()
