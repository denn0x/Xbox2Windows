"""s_round_mode (MODE[3:0], the f32 and f16/f64 rounding) in tools/startup/gfx2hlsl.py.

  (no arguments)                       translation controls, pure Python
  draw <dxc> <offscreen_check> <warp|hardware> [fxc]
                                       translate, compile (FXC ps_5_1 when given, DXC ps_6_0), draw one pixel (the
                                       startup_flat_param renderer) and compare the words

MODE[1:0] rounds f32 results and MODE[3:2] f16 and f64 results; 0 is nearest even, 3 toward zero (LLVM SIDefines.h
FP_ROUND_MODE_SP / FP_ROUND_MODE_DP and FP_ROUND_ROUND_TO_*; Mesa aco_ir.h struct float_mode round32 / round16_64).
The form the pixel stages use is 0xc: f32 to nearest even, f16/f64 toward zero.

The pixel stage (hand-assembled, the decoder reads it back) starts with s_round_mode 0xc, rounds eight f32 values to
binary16 with v_fma_mixlo_f16 / v_fma_mixhi_f16 (x * 1.0 + 0, f32 operands) and v_cvt_f16_f32, and exports the four
packed words to mrt0. The expected words are computed here by truncating the f32 mantissa (an independent rule, not
the helper's step-back); three of the values round differently to nearest even. v_interp_p2_f16 (the stage of
startup_interp_f16_check with s_round_mode 0xc first) is checked as text.

Controls: without the s_round_mode handler the stage refuses as before; the same stage without s_round_mode keeps
f32tof16; the drawn words do not match the nearest-even words (the comparison separates the roundings); the helper's
rule matches truncation on the CPU whether f32tof16 rounds to nearest even or toward zero. MEASURED on WARP (fxc
ps_5_1 and dxc ps_6_0): f32tof16 alone rounds these values toward zero (70000 -> 0x7bff), so on WARP the draw
passes with or without the step-back; the CPU check is what exercises it. s_round_mode 0x1 (f32 toward +inf), 0x4 (f16 toward
+inf) and one after a branch are refused; an f16 instruction that rounds and is not lowered for the mode (the SDWA
v_cvt_f16_f32) is refused under 0xc and accepted without it.
"""
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_flat_param_check import VS_WORDS, root_blob, compile_hlsl, draw  # noqa: E402

# f32 inputs: 1 + 0.75 ulp(f16), its negative, 70000 (above the binary16 range), 1/3, 0.1, -1000.7, 1.5 * 2^-14, pi
VALUES = (0x3f801800, 0xbf801800, 0x4788b800, 0x3eaaaaab, 0x3dcccccd, 0xc47a2ccd, 0x38c00000, 0x40490fdb)


def f16_toward_zero(bits):
    """binary16 of an f32 rounded toward zero, by truncation; only finite values whose result is normal or the
    largest finite binary16 (overflow) are used here."""
    sign, exp, mant = bits >> 31, (bits >> 23) & 0xff, bits & 0x7fffff
    e = exp - 127 + 15
    assert exp != 0xff and e >= 1, hex(bits)
    h = 0x7bff if e >= 31 else e << 10 | mant >> 13
    return sign << 15 | h


def f16_nearest(bits):
    try:
        return struct.unpack('<H', struct.pack('<e', struct.unpack('<f', struct.pack('<I', bits))[0]))[0]
    except OverflowError:
        return (bits >> 31) << 15 | 0x7c00


def words(rule):
    h = [rule(v) for v in VALUES]
    return [h[i] | h[i + 1] << 16 for i in range(0, 8, 2)]


EXPECTED = ','.join('%08x' % w for w in words(f16_toward_zero))


def v_mov_literal(reg, value):
    return struct.pack('<II', 0x7e000000 | reg << 17 | 1 << 9 | 0xff, value)


def mix(op, vdst, src0):
    # VOP3P (gfx10 0b110011000 << 23): opcode [22:16], vdst [7:0]; src0 [8:0], src1 [17:9] = 1.0 (242), src2 [26:18]
    # = 0 (128), op_sel_hi 0 (f32 operands). v_fma_mixlo_f16 0x21, v_fma_mixhi_f16 0x22 (LLVM VOP3PInstructions.td).
    return struct.pack('<II', 0xcc000000 | op << 16 | vdst, (256 + src0) | 242 << 9 | 128 << 18)


ROUND_MODE = 0xbfa40000  # SOPP opcode 0x24 (rdna2_tools_check: 0900a4bf is s_round_mode 0x9)


def pixel_words(mode=0xc, branch=False, add_f16=False):
    w = b''
    if branch:
        w += struct.pack('<I', 0xbf840000)  # s_cbranch_scc0 to the next instruction: control flow before the mode
    if mode is not None:
        w += struct.pack('<I', ROUND_MODE | mode)
    w += b''.join(v_mov_literal(6 + i, v) for i, v in enumerate(VALUES))
    w += mix(0x21, 2, 6) + mix(0x22, 2, 7) + mix(0x21, 3, 8) + mix(0x22, 3, 9)
    w += mix(0x21, 4, 10) + mix(0x22, 4, 11)
    w += struct.pack('<I', 0x7e000000 | 5 << 17 | 0x0a << 9 | 256 + 12)  # v_cvt_f16_f32_e32 v5, v12
    w += mix(0x22, 5, 13)
    if add_f16:
        # v_cvt_f16_f32_sdwa v14, v12 dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE src0_sel:DWORD (src0 0xf9 = SDWA;
        # second dword: src0 [7:0], dst_sel [10:8], dst_unused [12:11], src0_sel [18:16])
        w += struct.pack('<II', 0x7e000000 | 14 << 17 | 0x0a << 9 | 0xf9, 12 | 4 << 8 | 2 << 11 | 6 << 16)
    return w + bytes.fromhex('0f1800f802030405') + bytes.fromhex('000081bf')  # exp mrt0 v2..v5 done vm


def pipe(ps):
    return G.Pipeline(VS_WORDS, ps, root_blob())


def refused(ps, text):
    try:
        pipe(ps).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted, expected %r' % text)


def controls():
    asm = [a for _, _, _, a in G.decode(pixel_words(add_f16=True))]
    assert asm[0] == 's_round_mode 0xc', asm
    assert asm[9:11] == ['v_fma_mixlo_f16 v2, v6, 1.0, 0', 'v_fma_mixhi_f16 v2, v7, 1.0, 0'], asm
    assert asm[15:18] == ['v_cvt_f16_f32_e32 v5, v12', 'v_fma_mixhi_f16 v5, v13, 1.0, 0', 'v_cvt_f16_f32_sdwa v14, v12 dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE src0_sel:DWORD'], asm
    assert [f16_nearest(v) == f16_toward_zero(v) for v in VALUES].count(False) == 3
    ps = pipe(pixel_words()).hlsl_ps()
    assert '    v2 = (v2 & 0xffff0000u) | x2w_f32tof16_rtz(F(v6) * 1.0 + F(0x00000000u));' in ps, ps
    assert '    v2 = (v2 & 0xffffu) | (x2w_f32tof16_rtz(F(v7) * 1.0 + F(0x00000000u)) << 16);' in ps, ps
    assert '    v5 = (v5 & 0xffff0000u) | x2w_f32tof16_rtz(F(v12));' in ps, ps
    assert 'uint x2w_f32tof16_rtz(float x)' in ps and 'f32tof16(F(' not in ps, ps
    plain = pipe(pixel_words(mode=None)).hlsl_ps()
    assert '    v2 = (v2 & 0xffff0000u) | f32tof16(F(v6) * 1.0 + F(0x00000000u));' in plain and 'x2w_f32tof16_rtz' not in plain, plain
    assert 'x2w_f32tof16_rtz' not in pipe(pixel_words(mode=0)).hlsl_ps()  # 0: nearest even, as without it
    # v_interp_p2_f16 rounds its f32 interpolant to binary16 too (startup_interp_f16_check's stage, mode first)
    import startup_interp_f16_check as I
    interp = I.pipe(struct.pack('<I', ROUND_MODE | 0xc) + I.pixel_words()).hlsl_ps()
    assert '    v2 = (v2 & 0xffff0000u) | x2w_f32tof16_rtz(i.loparam0.x);' in interp, interp
    assert '    v2 = (v2 & 0xffffu) | (x2w_f32tof16_rtz(i.hiparam0.x) << 16);' in interp, interp
    # The helper's rule on the CPU, over both ways f32tof16 can round (WARP measured toward zero, see check_draw):
    # nearest even (the step-back then fires) and toward zero (it never does), against the truncation above.
    import random
    rng = random.Random(1)
    cases = list(VALUES) + [rng.getrandbits(1) << 31 | rng.randrange(113, 143) << 23 | rng.getrandbits(23)
                            for _ in range(20000)]
    stepped = 0
    for bits in cases:
        x = struct.unpack('<f', struct.pack('<I', bits))[0]
        for base in (f16_nearest, f16_toward_zero):
            h = base(bits)
            back = struct.unpack('<e', struct.pack('<H', h))[0] if h & 0x7c00 != 0x7c00 else float('inf')
            got = h - 1 if abs(back) > abs(x) else h
            stepped += got != h
            assert got == f16_toward_zero(bits), (hex(bits), base.__name__, hex(got))
    assert stepped > 5000, stepped
    print('x2w_f32tof16_rtz rule: %d values, nearest-even and toward-zero f32tof16, %d step-backs, all match '
          'truncation' % (len(cases), stepped))
    print('s_round_mode 0xc: f16 results of fma_mix and v_cvt_f16_f32 round toward zero (x2w_f32tof16_rtz); '
          'without it, or with 0, f32tof16')
    saved = G.Stage.op_s_round_mode
    del G.Stage.op_s_round_mode
    try:
        refused(pixel_words(), 's_round_mode')
    finally:
        G.Stage.op_s_round_mode = saved
    refused(pixel_words(mode=0x1), 's_round_mode 0x1 (only f32 nearest even')
    refused(pixel_words(mode=0x4), 's_round_mode 0x4 (only f32 nearest even')
    assert G.decode(pixel_words(branch=True))[0][3].startswith('s_cbranch_scc0')
    refused(pixel_words(branch=True), 's_round_mode 0xc after the straight-line entry')
    # s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 12 (SOPK 0x15) after the mode: a MODE write the tracking misses
    setreg = struct.pack('<II', 0xb0000000 | 0x15 << 23 | (1 | 3 << 11), 0xc)
    assert G.decode(setreg)[0][3] == 's_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 4), 12'
    w = pixel_words()
    refused(w[:4] + setreg + w[4:], 's_setreg_imm32_b32 writes MODE in a program that uses s_round_mode')
    refused(pixel_words(add_f16=True),'v_cvt_f16_f32_sdwa under s_round_mode f16/f64 toward zero (not lowered)')
    assert 'v14 = ' in pipe(pixel_words(mode=None, add_f16=True)).hlsl_ps()
    print('controls: refused without the handler; 0x1, 0x4 and after a branch refused; the SDWA v_cvt_f16_f32 (not lowered for the mode) refused under 0xc, '
          'accepted without it')


def check_draw(dxc, renderer, adapter, fxc=None):
    compilers = ([('fxc', fxc, 'vs_5_1', 'ps_5_1')] if fxc else []) + [('dxc', dxc, 'vs_6_0', 'ps_6_0')]
    nearest = ','.join('%08x' % w for w in words(f16_nearest))
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        for kind, compiler, vs_profile, ps_profile in compilers:
            def build(name, source, profile):
                return compile_hlsl(compiler, kind, source, '%s_%s' % (name, kind), profile, directory)
            good = pipe(pixel_words())
            vs = build('vs', good.hlsl_vs(), vs_profile)
            run = draw(renderer, adapter, vs, build('ps', good.hlsl_ps(), ps_profile), EXPECTED)
            if run.returncode:
                raise AssertionError('%s translated stage\n%s\n%s' % (kind, run.stdout, run.stderr))
            print('%s s_round_mode 0xc: %s' % (kind, EXPECTED), flush=True)
            # control: the comparison tells the two roundings apart (the same stage against nearest-even words)
            run = draw(renderer, adapter, vs, build('ps_c', good.hlsl_ps(), ps_profile), nearest)
            if run.returncode == 0:
                raise AssertionError('%s control: the toward-zero stage matched the nearest-even words' % kind)
            print('%s control against nearest-even words %s: mismatches as expected' % (kind, nearest), flush=True)
            # informational, not asserted: how this adapter's f32tof16 rounds (the stage without s_round_mode).
            # MEASURED on WARP (fxc ps_5_1): toward zero, so there the helper's step-back never fires.
            plain = build('ps_plain', pipe(pixel_words(mode=None)).hlsl_ps(), ps_profile)
            run = draw(renderer, adapter, vs, plain, EXPECTED)
            print('%s f32tof16 alone (no s_round_mode): %s' % (kind, 'toward zero (matches)' if run.returncode == 0
                                                             else 'not toward zero'), flush=True)
            run = draw(renderer, adapter, vs, plain, nearest)
            print('%s f32tof16 alone against nearest-even words: %s' % (kind, 'match' if run.returncode == 0
                                                                       else 'mismatch'), flush=True)


def main():
    if len(sys.argv) == 1:
        controls()
        return
    mode, *args = sys.argv[1:]
    if mode == 'draw':
        controls()
        check_draw(*args)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
