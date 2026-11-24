"""Packed D16 results of image_sample / image_sample_lz in tools/startup/gfx2hlsl.py.

Synthetic gfx1013 words: the 2D form of the startup_image_3d_check fixture with its image_sample word changed to
dmask 0x3 (0x7 for the three-channel case) and the D16 bit (MIMG gfx10 bit 63, LLVM MIMGInstructions.td
`Inst{63} = d16`) set, writing v0 (v[0:1] for three channels).

Semantics (gfx9+ packed D16, LLVM GCNSubtarget::hasPackedD16VMem, SIISelLowering::adjustLoadValueType): every
enabled channel of the result is converted to binary16 and two channels share one VGPR, the first in bits 15:0.
After an odd channel count the high half of the last VGPR is undefined; the translation writes 0 there.
Checked on the emitted HLSL text (the binary16 conversion is HLSL f32tof16; its rounding against the hardware's is
NOT verified here).
Controls (refused): d16 on image_load (an integer texel format would be converted as a float); d16 on
image_sample_l (its own modifier list).
Negative control: the same words without the D16 bit give four unpacked VGPR writes, not the packed one.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import startup_image_3d_check as T  # noqa: E402

BASE = bytes.fromhex('080f80f00000c400')        # image_sample v[0:3], v[0:1], s[16:23], s[24:27] dmask:0xf 2D


def words(word0, word1):
    return T.WORDS_2D.replace(BASE, struct.pack('<2I', word0, word1))


def asm(ws):
    return [x[3] for x in G.decode(ws) if x[3].startswith('image')][0]


def ps_of(ws):
    return T.translate(ws).hlsl_ps()


def refused(ws, label):
    try:
        ps_of(ws)
    except G.Unsupported as error:
        return str(error)
    raise AssertionError('accepted: %s (%s)' % (label, asm(ws)))


def main():
    two = words(0xf0800308, 0x80c40000)
    assert asm(two) == 'image_sample v0, v[0:1], s[16:23], s[24:27] dmask:0x3 dim:SQ_RSRC_IMG_2D d16', asm(two)
    ps = ps_of(two)
    line = 'v0 = f32tof16(t.x) | (f32tof16(t.y) << 16);'
    assert line in ps and 'v1 = U(t.y)' not in ps, ps
    three = words(0xf0800708, 0x80c40000)
    assert asm(three) == 'image_sample v[0:1], v[0:1], s[16:23], s[24:27] dmask:0x7 dim:SQ_RSRC_IMG_2D d16', asm(three)
    ps3 = ps_of(three)
    assert 'v0 = f32tof16(t.x) | (f32tof16(t.y) << 16);' in ps3 and 'v1 = f32tof16(t.z);' in ps3, ps3
    # lz form (image_sample_lz opcode 0x27): same packing
    lz = words(0xf09c0308, 0x80c40000)
    assert asm(lz) == 'image_sample_lz v0, v[0:1], s[16:23], s[24:27] dmask:0x3 dim:SQ_RSRC_IMG_2D d16', asm(lz)
    assert 't = r0.SampleLevel(q0, float2(F(v0), F(v1)), 0);' in ps_of(lz) and line in ps_of(lz), ps_of(lz)

    # Controls.
    load = words(0xf0001308, 0x80040000)          # image_load v0, v[0:1], s[16:23] dmask:0x3 unorm d16
    assert asm(load).startswith('image_load v0, v[0:1], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D') and 'd16' in asm(load), asm(load)
    assert 'modifier d16' in refused(load, 'image_load d16')
    wide = words(0xf0800308, 0x80c40000).replace(struct.pack('<2I', 0xf0800308, 0x80c40000),
                                                 struct.pack('<2I', 0xf0800f08, 0x80c40000))  # dmask 0xf into v[0:1] ok
    assert asm(wide) == 'image_sample v[0:1], v[0:1], s[16:23], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D d16', asm(wide)
    ps4 = ps_of(wide)
    assert 'v1 = f32tof16(t.z) | (f32tof16(t.w) << 16);' in ps4, ps4
    samplel = words(0xf0900308, 0x80c40000)        # image_sample_l ... d16
    assert asm(samplel).startswith('image_sample_l ') and 'd16' in asm(samplel), asm(samplel)
    assert 'image_sample_l modifier d16' in refused(samplel, 'image_sample_l d16')

    # Negative control: without D16 the result is four unpacked writes.
    plain = words(0xf0800f08, 0x00c40000)
    assert 'd16' not in asm(plain)
    ps0 = ps_of(plain)
    assert 'v0 = U(t.x);' in ps0 and 'f32tof16' not in ps0, ps0
    print('image d16: sample and sample_lz pack 2 and 3 and 4 channels into binary16 halves; '
          'image_load d16 and image_sample_l d16 refused; plain words unpacked')


if __name__ == '__main__':
    main()
