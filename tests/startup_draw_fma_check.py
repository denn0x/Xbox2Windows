"""The multiply-add rule in draw stages (tools/startup/gfx2hlsl.py, X2W_DRAW_FMA; lead 000800 answer).

RDNA 2 ISA: V_FMA_F32 / V_FMAC_F32 / V_FMAMK_F32 / V_FMAAK_F32 compute a * b + c with one rounding; V_MAD_F32 /
V_MAC_F32 / V_MADMK_F32 / V_MADAK_F32 round the product first. Rule (the same as mesh2hlsl's):
- exact (default): fused ops -> x2w_fma_exact = (float)fma((double)a, (double)b, (double)c); unfused ops ->
  x2w_mad_precise = precise multiply then precise add; the manifest says `fma exact`;
- precise: both through x2w_mad_precise; `fma precise`;
- legacy: `a * b + c` as before, no manifest line.
Checked on synthetic vertex stages (VOP3 v_fma_f32 and v_mad_f32 into v4, exp pos0 v[4:7]): the emitted call,
the helper definitions, the manifest line; a stage with no multiply-add has no `fma` line and no helper. The
helpers' arithmetic in Python with exact rationals: on a case where a * b + c rounded twice differs from the fused
value, the binary64 fma rounded to binary32 equals the fused value and the precise form equals the two-rounding
value. With fxc available, a vs_5_1 shader with both helpers compiles. Not checked: a GPU readback.
"""
import os
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from startup_sampler_heap_check import root_blob  # noqa: E402
from startup_stream_check import gfx2hlsl as G  # noqa: E402

EXPORT = (0xf80008cf, 0x07060504, 0xbf810000)   # exp pos0 v4, v5, v6, v7 done; s_endpgm
PS = struct.pack('<3I', 0xf800180f, 0, 0xbf810000)


def vop3(op, dst, a, b, c):
    return (0xd4000000 | op << 16 | dst, (256 + a) | (256 + b) << 9 | (256 + c) << 18)


def pipeline(*ops):
    words = [w for op in ops for w in vop3(*op)] + list(EXPORT)
    vs = struct.pack('<%dI' % len(words), *words)
    p = G.Pipeline(vs, PS, root_blob(flags=0))
    return [x[3] for x in G.decode(vs)], p.hlsl_vs(), p.manifest().split('\n')


def f32(x):
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]


def round32(q):
    """Fraction -> nearest binary32 (ties to even), through the exact neighbours."""
    lo = f32(q)
    return lo if Fraction(lo) == q else min((lo, *(struct.unpack('<f', struct.pack('<I', struct.unpack('<I', struct.pack('<f', lo))[0] + d))[0]
                                                  for d in (-1, 1))), key=lambda v: (abs(Fraction(v) - q), struct.unpack('<I', struct.pack('<f', v))[0] & 1))


def main():
    saved = G.DRAW_FMA
    try:
        G.DRAW_FMA = 'exact'
        asm, hlsl, man = pipeline((0x14b, 4, 1, 2, 3), (0x141, 5, 1, 2, 3))
        assert asm[:2] == ['v_fma_f32 v4, v1, v2, v3', 'v_mad_f32 v5, v1, v2, v3'], asm
        assert 'v4 = U(x2w_fma_exact(F(v1), F(v2), F(v3)));' in hlsl, hlsl
        assert 'v5 = U(x2w_mad_precise(F(v1), F(v2), F(v3)));' in hlsl, hlsl
        assert G.HELPERS['x2w_fma_exact'] in hlsl and G.HELPERS['x2w_mad_precise'] in hlsl
        assert 'fma exact' in man, man
        G.DRAW_FMA = 'precise'
        _, hlsl, man = pipeline((0x14b, 4, 1, 2, 3))
        assert 'v4 = U(x2w_mad_precise(F(v1), F(v2), F(v3)));' in hlsl and 'x2w_fma_exact' not in hlsl, hlsl
        assert 'fma precise' in man, man
        G.DRAW_FMA = 'legacy'
        _, hlsl, man = pipeline((0x14b, 4, 1, 2, 3))
        assert 'v4 = U(F(v1) * F(v2) + F(v3));' in hlsl and not any(l.startswith('fma') for l in man), (hlsl, man)
        G.DRAW_FMA = 'exact'
        # negative control: no multiply-add, no rule line and no helper (v_add_f32 = VOP3 0x103)
        _, hlsl, man = pipeline((0x103, 4, 1, 2, 2))
        assert not any(l.startswith('fma') for l in man) and 'x2w_fma_exact' not in hlsl, man
    finally:
        G.DRAW_FMA = saved

    # arithmetic: a = b = 1 + 2^-12, c = -(1 + 2^-11): a * b = 1 + 2^-11 + 2^-24, a tie in binary32 (rounds to
    # 1 + 2^-11), so the two-rounding result is 0 and the fused one 2^-24
    a, b, c = f32(1 + 2 ** -12), f32(1 + 2 ** -12), f32(-(1 + 2 ** -11))
    exact = Fraction(a) * Fraction(b) + Fraction(c)
    fused = round32(exact)
    via_double = f32(float(exact))                     # fma in binary64 (exact here), then binary32
    two_roundings = f32(f32(a * b) + c)                # precise multiply then precise add
    assert fused == via_double == 2.0 ** -24, (fused, via_double)
    assert two_roundings == 0.0 and two_roundings != fused, two_roundings

    fxc = G.find_fxc()
    compiled = 'skipped (no fxc)'
    if fxc:
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 'fma.hlsl')
            open(src, 'w').write('%s\n%s\nfloat4 main(float4 p : POSITION) : SV_Position { return '
                                 'float4(x2w_fma_exact(p.x, p.y, p.z), x2w_mad_precise(p.x, p.y, p.w), 0, 1); }\n'
                                 % (G.HELPERS['x2w_fma_exact'], G.HELPERS['x2w_mad_precise']))
            r = subprocess.run([fxc, '/nologo', '/T', 'vs_5_1', '/E', 'main', '/Fo', os.path.join(d, 'fma.cso'), src],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            compiled = 'compiles with fxc vs_5_1'
    print('draw_fma: fused -> x2w_fma_exact, unfused -> x2w_mad_precise, manifest fma exact/precise, legacy unchanged; '
          'no multiply-add -> no line; fused value matched through binary64 where two roundings give 0; helpers %s'
          % compiled)


if __name__ == '__main__':
    main()
