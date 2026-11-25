"""x2w_fmt_load in its table form gives the per-format chain's values (tools/startup/gfx2hlsl.py).

The emitted helper (HELPERS['x2w_fmt_load'] = buf_format_helper_table()) looks the FORMAT code of V# word 3 up in a
constant table of channel widths and kind and decodes the channel with one routine; the older helper
(buf_format_helper(), kept as the reference) had one if-arm per format, which fxc compiled for minutes per call site.

Checked in Python, over every FORMAT code 0..127, every DST_SEL value 0..7 for each of the four result channels, and
32 element addresses (all four byte alignments) of a pseudo-random buffer that also holds the most negative SNORM
fields:
- the reference: each format's channel expressions exactly as the old chain emits them (buf_format_channel), run
  by a small evaluator, with the old helper's fills (unknown code: 0; selector 0/2/3: 0; 1: 1 or 1.0; a channel
  the format lacks: 0, w: one);
- the new helper: its table parsed from the emitted HLSL text, and a line-for-line Python transcription of its
  decoder, whose HLSL lines are asserted verbatim so the transcription cannot drift.
Float results are compared as binary32 bits; the division is rounded to binary32 once in both (on the GPU the
chain divided by a literal and the table form divides by a run-time value: equal results there are ASSUMED).
Negative controls: a table with one width changed, and a decoder with the SNORM clamp dropped, both mismatch.
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
import gfx2hlsl as G  # noqa: E402

M = 0xffffffff
DATA = bytearray(((i * 0x9e3779b1) >> 13) & 0xff for i in range(256))
DATA[2:5] = bytes((0x00, 0x80, 0x80))  # the most negative SNORM fields: -32768 at a = 2, -128 at a = 4
ADDRS = list(range(0, 64, 2))


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', f32(x)))[0]


def half(h):
    return struct.unpack('<e', struct.pack('<H', h & 0xffff))[0]


def load(a):
    return int.from_bytes(DATA[a & ~3:(a & ~3) + 4], 'little')


def ldu(a):
    return int.from_bytes(DATA[a:a + 4], 'little')


def s32(x):
    x &= M
    return x - (1 << 32) if x >> 31 else x


def chain_channel(code, k, a):
    """The old chain's expression for channel k of `code`, evaluated."""
    widths, kind = G.BUF_FORMATS[code]
    text = G.buf_format_channel(widths, kind, k)
    py = re.sub(r'(0x[0-9a-f]+|\d+)u\b', r'\1', text)
    py = py.replace('b.Load(', 'load(').replace('ldu(b, ', 'ldu(').replace('uint(int(', 'u32(s32(')
    py = py.replace('float(int(', 'float(s32(').replace('U(', 'ubits(')
    env = {'a': a, 'load': load, 'ldu': ldu, 's32': s32, 'u32': lambda x: x & M, 'max': max,
           'ubits': lambda x: bits(x) if isinstance(x, float) else x & M, 'f16tof32': half,
           'float': float, '__builtins__': {}}
    return eval(py, env) & M


def chain_load(code, a, w3, c):
    s = (w3 >> (3 * c)) & 7
    if code not in G.BUF_FORMATS or s in (0, 2, 3):
        return 0
    widths, kind = G.BUF_FORMATS[code]
    one = 1 if kind in ('uint', 'sint') else 0x3f800000
    if s == 1:
        return one
    k = s - 4
    if k >= len(widths):
        return one if k == 3 else 0
    return chain_channel(code, k, a)


DECODER = [
    'uint x2w_fmt_chan(ByteAddressBuffer b, uint a, uint d, uint k, uint one) {',
    '    uint n = (d >> (6u * k)) & 63u;',
    '    if (n == 0u) return k == 3u ? one : 0u;',
    '    uint pos = (k > 0u ? (d & 63u) : 0u) + (k > 1u ? ((d >> 6) & 63u) : 0u) + (k > 2u ? ((d >> 12) & 63u) : 0u);',
    '    uint kind = (d >> 24) & 7u, sh = 32u - n;',
    '    uint raw = n == 32u ? b.Load(a + pos / 8u) : ((ldu(b, a + 4u * (pos / 32u)) >> (pos % 32u)) & ((1u << n) - 1u));',
    '    int sv = int(raw << sh) >> sh;',
    '    if (kind == 0u || (kind == 4u && n == 32u)) return raw;',
    '    if (kind == 1u) return uint(sv);',
    '    if (kind == 2u) return U(float(raw) / float((1u << n) - 1u));',
    '    if (kind == 3u) return U(max(float(sv) / float((1u << (n - 1u)) - 1u), -1.0));',
    '    return U(f16tof32(n == 16u ? raw : raw << (15u - n))); }',
    'uint x2w_fmt_load(ByteAddressBuffer b, uint a, uint w3, uint c) {',
    '    uint f = (w3 >> 12) & 127u, s = (w3 >> (3u * c)) & 7u;',
    '    uint d = f < 78u ? x2w_fmt_tab[f] : 0u;',
    '    if (d == 0u || s == 0u || s == 2u || s == 3u) return 0u;',
    '    uint one = ((d >> 24) & 7u) <= 1u ? 1u : 0x3f800000u;',
    '    return s == 1u ? one : x2w_fmt_chan(b, a, d, s - 4u, one); }']


def table_chan(a, d, k, one, clamp=True):
    """Line-for-line transcription of DECODER's x2w_fmt_chan (HLSL shifts take the low 5 bits of the count)."""
    n = (d >> (6 * k)) & 63
    if n == 0:
        return one if k == 3 else 0
    pos = ((d & 63) if k > 0 else 0) + (((d >> 6) & 63) if k > 1 else 0) + (((d >> 12) & 63) if k > 2 else 0)
    kind, sh = (d >> 24) & 7, (32 - n) & 31
    raw = load(a + pos // 8) if n == 32 else (ldu(a + 4 * (pos // 32)) >> (pos % 32)) & ((1 << n) - 1)
    sv = s32(raw << sh) >> sh
    if kind == 0 or (kind == 4 and n == 32):
        return raw
    if kind == 1:
        return sv & M
    if kind == 2:
        return bits(float(raw) / float((1 << n) - 1))
    if kind == 3:
        q = float(sv) / float((1 << (n - 1)) - 1)
        return bits(max(q, -1.0) if clamp else q)
    return bits(half(raw if n == 16 else raw << (15 - n)))


def table_load(tab, a, w3, c, clamp=True):
    f, s = (w3 >> 12) & 127, (w3 >> (3 * c)) & 7
    d = tab[f] if f < len(tab) else 0
    if d == 0 or s in (0, 2, 3):
        return 0
    one = 1 if (d >> 24) & 7 <= 1 else 0x3f800000
    return one if s == 1 else table_chan(a, d, s - 4, one, clamp)


def mismatches(tab, clamp=True):
    bad = []
    for code in range(128):
        for c in range(4):
            for s in range(8):
                w3 = code << 12 | s << (3 * c)
                for a in ADDRS:
                    if table_load(tab, a, w3, c, clamp) != chain_load(code, a, w3, c):
                        bad.append((code, c, s, a))
    return bad


def main():
    text = G.HELPERS['x2w_fmt_load']
    assert text == G.buf_format_helper_table(), 'the emitted helper is the table form'
    lines = text.split('\n')
    m = re.fullmatch(r'static const uint x2w_fmt_tab\[(\d+)\] = \{ (.*) \};', lines[0])
    assert m and lines[1:] == DECODER, lines[1:]
    tab = [int(x[:-1], 16) for x in m.group(2).split(', ')]
    assert len(tab) == int(m.group(1)) == 78
    bad = mismatches(tab)
    assert not bad, bad[:5]
    checked = 128 * 4 * 8 * len(ADDRS)
    wrong = list(tab)
    wrong[56] = (wrong[56] & ~63) | 7  # 8_8_8_8_UNORM x read as 7 bits
    assert mismatches(wrong), 'a wrong width escaped'
    assert mismatches(tab, clamp=False), 'a dropped SNORM clamp escaped'
    print('fmt_load_table: table helper equals the per-format chain on %d (code, channel, selector, address) cases '
          '(%d formats); wrong-width and no-clamp controls caught' % (checked, len(G.BUF_FORMATS)))


if __name__ == '__main__':
    main()
