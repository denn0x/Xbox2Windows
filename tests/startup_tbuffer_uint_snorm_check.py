"""tbuffer_load_format_* with BUF_FMT_8_8_8_8_UINT and BUF_FMT_16_16_SNORM (gfx10 unified formats 60 and 24).

Synthetic gfx1013 words (the vertex stage of startup_vertex_fetch_formats_check: s_load_dwordx4 of vertex buffer 1,
tbuffer_load_format_xyzw v[4:7], v0, ... idxen offset:<o> format:<code>, exp pos0). References from the format
definitions, little-endian fields from the lowest byte:
- 8_8_8_8_UINT: four unsigned bytes, x first; the integer value itself (no conversion).
- 16_16_SNORM: two signed 16-bit fields; float = max(c / 32767, -1.0) (D3D11.3 SNORM -> FLOAT, both -32768 and
  -32767 give -1.0); missing z = 0, missing w = 1.0. That the console fetch clamps the same way is ASSUMED.
The emitted lines are run in Python (ldu = the unaligned little-endian dword at a) on 16 vertices of a byte
pattern at an odd stride, and on hand-picked edge fields (0, 1, 0x7fff, 0x8000, 0x8001, 0xffff). The float
division is rounded to binary32 once; the GPU's division is not modelled (documented tolerance: exact here).
Negative controls: a reference with x/y swapped, and a translation with the -1.0 clamp removed, both fail.
"""
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from startup_sampler_heap_check import root_blob  # noqa: E402
from startup_stream_check import gfx2hlsl  # noqa: E402

DATA = bytearray(b''.join(struct.pack('<I', (0x8b02a7f1 + i * 0x413b1) & 0xffffffff) for i in range(64)))
FORMATS = {'BUF_FMT_8_8_8_8_UINT': (60, 5, 3), 'BUF_FMT_16_16_SNORM': (24, 6, 2)}  # code, stride, offset


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def s32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >> 31 else x


def ldu(a):
    return int.from_bytes(DATA[a:a + 4], 'little')


def lines(name):
    code, _, offset = FORMATS[name]
    words = (0xf4080400, 0xfa000010, 0xe8032000 | offset | code << 19, 0x80040400, 0xf80008cf, 0x07060504, 0xbf810000)
    pipe = gfx2hlsl.Pipeline(struct.pack('<7I', *words), struct.pack('<3I', 0xf800180f, 0, 0xbf810000),
                             root_blob(flags=0))
    text = [x[3] for x in gfx2hlsl.decode(struct.pack('<7I', *words))][1]
    assert 'format:[%s]' % name in text, text
    assert 'srv 0 buf vb 1' in pipe.manifest().splitlines()
    out = [l.strip() for l in pipe.hlsl_vs().split('\n')]
    return [l for l in out if re.match(r'(a|w|v[4-7]) = ', l)]


def to_py(expr):
    expr = re.sub(r'(\d)u\b', r'\1', expr)
    expr = expr.replace('ldu(r0, a)', 'ldu(a)').replace('asint(', 's32(').replace('U(', 'ubits(')
    return expr.replace('float(', 'float(')


def ubits(x):
    return bits(f32(x)) if isinstance(x, float) else x & 0xffffffff


def run(body, vertex, stride):
    env = {'v0': vertex, 'ud4': stride}
    g = {'ldu': ldu, 's32': s32, 'ubits': ubits, 'max': max, 'float': float, '__builtins__': {}}
    for line in body:
        m = re.fullmatch(r'(\w+) = (.*);', line)
        expr = to_py(m.group(2))
        # binary32 division: the dividend is an exact integer, the quotient rounded once
        expr = re.sub(r'float\((.*?)\) / 32767\.0', r'f32(float(\1) / 32767.0)', expr)
        g['f32'] = f32
        env[m.group(1)] = eval(expr, g, env) & 0xffffffff if m.group(1) in ('a', 'w') else eval(expr, g, env)
    return [ubits(env['v%d' % i]) for i in range(4, 8)]


def reference(name, vertex, swap=False):
    _, stride, offset = FORMATS[name]
    a = vertex * stride + offset
    if name == 'BUF_FMT_8_8_8_8_UINT':
        f = list(DATA[a:a + 4])
        if swap:
            f[0], f[1] = f[1], f[0]
        return f
    f = [struct.unpack('<h', DATA[a + 2 * i:a + 2 * i + 2])[0] for i in range(2)]
    if swap:
        f.reverse()
    return [bits(f32(max(c / 32767.0, -1.0))) for c in f] + [0, bits(1.0)]


def main():
    checked = 0
    for name, (_, stride, _) in FORMATS.items():
        body = lines(name)
        assert body[0] == 'a = v0 * ud4 + %d;' % FORMATS[name][2] and body[1] == 'w = ldu(r0, a);', body
        for v in range(16):
            got = run(body, v, stride)
            assert got == reference(name, v), (name, v, got, reference(name, v))
            checked += 1
        assert any(run(body, v, stride) != reference(name, v, swap=True) for v in range(16)), name
    # SNORM edge fields at vertex 0
    body = lines('BUF_FMT_16_16_SNORM')
    for lo, hi in ((0, 1), (0x7fff, 0x8000), (0x8001, 0xffff), (0x8000, 0x7fff)):
        DATA[2:6] = struct.pack('<HH', lo, hi)
        got = run(body, 0, 6)
        assert got == reference('BUF_FMT_16_16_SNORM', 0), (hex(lo), hex(hi), got)
    DATA[2:6] = struct.pack('<HH', 0x8000, 0x8000)
    assert run(body, 0, 6)[:2] == [bits(-1.0)] * 2
    unclamped = [l.replace('max(', '(').replace(', -1.0))', '))') for l in body]
    assert run(unclamped, 0, 6)[0] != bits(-1.0) and math.isfinite(struct.unpack('<f', struct.pack('<I', run(unclamped, 0, 6)[0]))[0])
    print('tbuffer_uint_snorm: 8_8_8_8_UINT and 16_16_SNORM match the format definitions on %d vertices at odd '
          'strides and 5 SNORM edge pairs; swapped-reference and unclamped-translation controls caught' % checked)


if __name__ == '__main__':
    main()
