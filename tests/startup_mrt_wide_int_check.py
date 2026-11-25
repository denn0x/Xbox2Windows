"""Integer render targets with 32 bits per channel beyond R32: R32G32B32A32 / R32G32B32 / R32G32 _UINT and _SINT.

Pure Python. The pixel fixture of tests/startup_mrt_check.py (v0..v3 = 1, 2, 0x7fc00001, 0x80000001, then
exp mrt0 v0, v1, v2, v3) is translated with each DXGI format as the slot-0 RTV. Expected: the 32-bit-per-channel
integer formats (3, 7, 17, 42 uint; 4, 8, 18, 43 sint) declare uint4 / int4 SV_Target0 and export the four
register words unconverted, exactly as the verified R32 forms do; narrower integer formats (12 R16G16B16A16_UINT,
30 R8G8B8A8_UINT, 57 R16_UINT, 14 R16G16B16A16_SINT) stay refused as "unverified". ASSUMED (not drawn): the colour
buffer stores the export words of these wider formats as it does for R32.
Negative control: the float target (2, R32G32B32A32_FLOAT) declares float4.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import struct  # noqa: E402
from startup_mrt_check import FLOAT_WORDS  # noqa: E402
from startup_stream_check import gfx2hlsl as G  # noqa: E402
from startup_stream_draw_check import vop1  # noqa: E402
from startup_sampler_heap_check import VS_WORDS, root_blob  # noqa: E402


def pixel_words(values):
    """startup_mrt_check.pixel's program without its container: v_mov_b32 vN, literal; exp mrt0 v0..v3 done vm."""
    words = []
    for reg, value in enumerate(values):
        words += [vop1(1, reg, 255), value]
    words += [0xf800180f, 0x03020100, 0xbf810000]
    return struct.pack('<%dI' % len(words), *words)


def ps(fmt):
    p = G.Pipeline(VS_WORDS, pixel_words(FLOAT_WORDS), root_blob(flags=0), rtv_formats=(fmt,))
    return p.hlsl_ps()


def main():
    for fmt, typ in ((3, 'uint4'), (7, 'uint4'), (17, 'uint4'), (42, 'uint4'),
                     (4, 'int4'), (8, 'int4'), (18, 'int4'), (43, 'int4')):
        text = ps(fmt)
        assert '%s mrt0 : SV_Target0;' % typ in text, (fmt, text)
        line = [l.strip() for l in text.splitlines() if l.strip().startswith('o.mrt0 =')]
        want = 'uint4(v0, v1, v2, v3)'
        assert line == ['o.mrt0 = %s;' % (want if typ == 'uint4' else 'asint(%s)' % want)], (fmt, line)
    for fmt in (12, 30, 57, 14):
        try:
            ps(fmt)
        except G.Unsupported as error:
            assert re.search(r'unverified [SU]INT RTV format %d' % fmt, str(error)), str(error)
        else:
            raise AssertionError('narrow integer format %d accepted' % fmt)
    assert 'float4 mrt0 : SV_Target0;' in ps(2)
    print('mrt_wide_int: formats 3/7/17/42 -> uint4 and 4/8/18/43 -> int4 with the raw words; 12/30/57/14 refused; '
          'float control declares float4')


if __name__ == '__main__':
    main()
