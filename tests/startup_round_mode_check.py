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

Without s_round_mode (MODE 0) the same stage rounds to nearest even through x2w_f32tof16_rne, and its draw must give
the nearest-even words. MEASURED (fxc ps_5_1 and dxc ps_6_0, WARP and an RX 9070 XT): HLSL f32tof16 alone rounds
these values toward zero (70000 -> 0x7bff), which is why both helpers exist; the draw prints that as information.
Controls: without the s_round_mode handler the stage refuses as before; the toward-zero stage does not match the
nearest-even words (the comparison separates the roundings); both helpers' rules are checked on the CPU over f32tof16
modelled toward zero and nearest even (the draw cannot exercise the toward-zero helper's step-back there). s_round_mode 0x1 (f32 toward +inf), 0x4 (f16 toward
+inf) and one after a branch are refused; an f16 instruction that rounds and is not lowered for the mode (the SDWA
v_cvt_f16_f32) is refused under 0xc and accepted without it.

The launch FLOAT_MODE (SPI_SHADER_PGM_RSRC1_PS bits 19:12, the optional third .psin field) starts MODE: 0xcc rounds
f16 toward zero without any s_round_mode, 0xc0 and a request without the field round to nearest even; other modes are
refused. Control: with launch_mode disabled the 0xcc stage stays nearest even. The draw also checks the 0xcc stage.
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


def pixel_words(mode=0xc, branch=False, add_f16=False, pkrtz=False):
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
    if pkrtz:
        w += struct.pack('<I', 0x2f << 25 | 15 << 17 | 7 << 9 | 256 + 6)  # v_cvt_pkrtz_f16_f32_e32 v15, v6, v7
    if add_f16:
        # v_cvt_f16_f32_sdwa v14, v12 dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE src0_sel:DWORD (src0 0xf9 = SDWA;
        # second dword: src0 [7:0], dst_sel [10:8], dst_unused [12:11], src0_sel [18:16])
        w += struct.pack('<II', 0x7e000000 | 14 << 17 | 0x0a << 9 | 0xf9, 12 | 4 << 8 | 2 << 11 | 6 << 16)
    return w + bytes.fromhex('0f1800f802030405') + bytes.fromhex('000081bf')  # exp mrt0 v2..v5 done vm


NOT_PASSED = 'not passed'
# SPI inputs: PERSP_SAMPLE/CENTER/CENTROID, LINEAR_SAMPLE/CENTER, POS_X..W, FRONT_FACE (v0..v14; the SDWA preserve of
# add_f16 reads v14, front_face). The stage overwrites the others before reading them.
ENA = 0x1f37


def pipe(ps, launch=0xc0):
    """launch: the pixel stage's FLOAT_MODE passed as the third SPI input field, or NOT_PASSED (two fields)."""
    return G.Pipeline(VS_WORDS, ps, root_blob(),
                      ps_hw_inputs=(ENA, ENA) if launch == NOT_PASSED else (ENA, ENA, launch))


def refused(ps, text, launch=0xc0):
    try:
        pipe(ps, launch).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted, expected %r' % text)


def f16_value(h):
    return float('inf') * (-1 if h & 0x8000 else 1) if h & 0x7fff == 0x7c00 else struct.unpack('<e', struct.pack('<H', h))[0]


def rne_helper(bits, f32tof16):
    """x2w_f32tof16_rne statement by statement, over a model of the adapter's f32tof16."""
    x = struct.unpack('<f', struct.pack('<I', bits))[0]
    t, a = f32tof16(bits), abs(x)
    if abs(f16_value(t)) > a:
        t -= 1
    if t & 0x7c00 == 0x7c00:
        return t
    lo = abs(f16_value(t))
    if a == lo:
        return t
    u = t + 1
    hi = 65536.0 if u & 0x7fff == 0x7c00 else abs(f16_value(u))
    dl, dh = a - lo, hi - a
    return u if dh < dl or (dh == dl and t & 1) else t


def check_rne_rule():
    """x2w_f32tof16_rne against Python's binary16 (struct 'e', round to nearest even, OverflowError past 65519.99)
    for f32tof16 modelled both ways: toward zero (MEASURED on WARP and an RX 9070 XT) and nearest even. The cases
    cover normal, subnormal (2^-24 .. 2^-14) and overflow (65504 .. 2^17) results and the exact ties."""
    import random
    rng = random.Random(2)
    def truncate(bits):  # toward zero, for any finite value (subnormals and overflow included)
        h = f16_nearest(bits)
        x = struct.unpack('<f', struct.pack('<I', bits))[0]
        return h - 1 if abs(f16_value(h)) > abs(x) else h
    ties = [struct.unpack('<I', struct.pack('<f', (f16_value(h) + f16_value(h + 1)) / 2))[0] for h in range(1, 0x7bff, 97)]
    cases = list(VALUES) + ties + [0x477fe000, 0x477ff000, 0xc77ff000] + [
        rng.getrandbits(1) << 31 | rng.randrange(103, 144) << 23 | rng.getrandbits(23) for _ in range(30000)]
    for bits in cases:
        want = f16_nearest(bits)
        for name, base in (('toward zero', truncate), ('nearest even', f16_nearest)):
            got = rne_helper(bits, base)
            assert got == want, (hex(bits), name, hex(got), hex(want))
    print('x2w_f32tof16_rne rule: %d values (%d exact ties), f32tof16 modelled toward zero and nearest even, all '
          'match binary16 nearest even' % (len(cases), len(ties)))


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
    assert '    v2 = (v2 & 0xffff0000u) | x2w_f32tof16_rne(F(v6) * 1.0 + F(0x00000000u));' in plain, plain
    assert '    v5 = (v5 & 0xffff0000u) | x2w_f32tof16_rne(F(v12));' in plain and 'x2w_f32tof16_rtz' not in plain, plain
    assert 'x2w_f32tof16_rtz' not in pipe(pixel_words(mode=0)).hlsl_ps()  # 0: nearest even, as without it
    check_rne_rule()
    # v_cvt_pkrtz_f16_f32 rounds toward zero in every mode (its definition), through the helper
    for m in (None, 0xc):
        g = G.decode(pixel_words(mode=m, pkrtz=True))
        assert g[-3][3] == 'v_cvt_pkrtz_f16_f32_e32 v15, v6, v7', g[-3]
        text = pipe(pixel_words(mode=m, pkrtz=True)).hlsl_ps()
        assert '    v15 = x2w_f32tof16_rtz(F(v6)) | (x2w_f32tof16_rtz(F(v7)) << 16);' in text, text
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
          'without it, or with 0, nearest even (x2w_f32tof16_rne)')
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
    launch_controls()


def check_dumped_float_mode():
    """gfx2hlsl.dumped_float_mode: u32 at state block A + 0x04c, bits 19:12, from <name>.vsregs or the per-state
    dumps <name>.<8 hex>.vsregs when they agree; and translate_request's use of it (.psin without the field)."""
    def block(mode, size=0x100):
        b = bytearray(size)
        if size >= 0x50:
            struct.pack_into('<I', b, 0x4c, mode << 12 | 0x2c0041)  # other RSRC1 fields set, must not leak
        return bytes(b)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 'a.vsregs').write_bytes(block(0xcc))
        (d / 'b.vsregs').write_bytes(block(0xc0))
        (d / 'c.vsregs').write_bytes(block(0xcc, 0x40))  # too short
        (d / 'e.00000001.vsregs').write_bytes(block(0xcc))
        (d / 'e.00000002.vsregs').write_bytes(block(0xcc))
        (d / 'f.00000001.vsregs').write_bytes(block(0xcc))
        (d / 'f.00000002.vsregs').write_bytes(block(0xc0))  # disagreeing per-state dumps
        got = {n: G.dumped_float_mode(str(d / n)) for n in 'abcefg'}
        assert got == {'a': 0xcc, 'b': 0xc0, 'c': None, 'e': 0xcc, 'f': None, 'g': None}, got
    print('dumped_float_mode: 0xcc, 0xc0, short block None, agreeing per-state dumps 0xcc, disagreeing None, none None')


def launch_controls():
    """The launch FLOAT_MODE (SPI_SHADER_PGM_RSRC1_PS bits 19:12, the optional third .psin field)."""
    # 0xcc (the 262 measured mesh pairs): the stage WITHOUT s_round_mode rounds toward zero from launch
    text = pipe(pixel_words(mode=None), launch=0xcc).hlsl_ps()
    assert '    v5 = (v5 & 0xffff0000u) | x2w_f32tof16_rtz(F(v12));' in text and 'x2w_f32tof16_rne' not in text, text
    # 0xc0 (the rest): nearest even
    plain = pipe(pixel_words(mode=None), launch=0xc0).hlsl_ps()
    assert 'x2w_f32tof16_rne(F(v12))' in plain and 'x2w_f32tof16_rtz(F(v12))' not in plain
    # s_round_mode 0 after a 0xcc launch switches back to nearest even
    assert 'x2w_f32tof16_rtz(F(v12))' not in pipe(pixel_words(mode=0), launch=0xcc).hlsl_ps()
    # an f16 op not lowered for toward zero is refused under a 0xcc launch alone (no s_round_mode), accepted at 0xc0
    refused(pixel_words(mode=None, add_f16=True), 'v_cvt_f16_f32_sdwa under s_round_mode f16/f64 toward zero',
            launch=0xcc)
    assert 'v14 = ' in pipe(pixel_words(mode=None, add_f16=True), launch=0xc0).hlsl_ps()
    # not passed (a two-field .psin): an f16 rounding before any s_round_mode is refused; after s_round_mode 0xc or 0
    # the mode is known; v_cvt_pkrtz_f16_f32 (toward zero by definition) needs no mode
    refused(pixel_words(mode=None), 'v_fma_mixlo_f16: f16/f64 rounding with the launch FLOAT_MODE not passed',
            launch=NOT_PASSED)
    assert pipe(pixel_words(mode=0xc), launch=NOT_PASSED).hlsl_ps() == pipe(pixel_words(mode=0xc)).hlsl_ps()
    assert pipe(pixel_words(mode=0), launch=NOT_PASSED).hlsl_ps() == pipe(pixel_words(mode=0)).hlsl_ps()
    only_pkrtz = pixel_words(mode=None, pkrtz=True)
    only_pkrtz = only_pkrtz[:only_pkrtz.index(mix(0x21, 2, 6))] + only_pkrtz[-16:]  # v_movs, pkrtz, exp, s_endpgm
    # (ENA PERSP_CENTER + POS_X..W: the export of v2..v5, not written here, reads the position)
    assert 'x2w_f32tof16_rtz(F(v6)) | (x2w_f32tof16_rtz(F(v7)) << 16)' in G.Pipeline(
        VS_WORDS, only_pkrtz, root_blob(), ps_hw_inputs=(0xf02, 0xf02)).hlsl_ps()
    # negative control: without check_launch_known the not-passed stage is accepted (and lowered as nearest even)
    saved = G.Stage.check_launch_known
    G.Stage.check_launch_known = lambda self, name: None
    try:
        assert 'x2w_f32tof16_rne(F(v12))' in pipe(pixel_words(mode=None), launch=NOT_PASSED).hlsl_ps()
    finally:
        G.Stage.check_launch_known = saved
    check_dumped_float_mode()
    # a MODE write after a non-zero launch mode is refused (the tracking would miss it)
    setreg = struct.pack('<II', 0xb0000000 | 0x15 << 23 | (1 | 3 << 11), 0xc)
    refused(setreg + pixel_words(mode=None), 's_setreg_imm32_b32 writes MODE', launch=0xcc)
    # refused launch modes: f32 not nearest even, f16 toward +inf, other denorm settings
    for bad in (0xcd, 0xc4, 0xf0, 0x0c, 0x00):
        refused(pixel_words(mode=None), 'launch FLOAT_MODE %#04x' % bad, launch=bad)
    # negative control: without launch_mode the 0xcc stage is nearest even (the check above would fail)
    saved = G.Stage.launch_mode
    G.Stage.launch_mode = lambda self, mode: None
    try:
        assert 'x2w_f32tof16_rne(F(v12))' in pipe(pixel_words(mode=None), launch=0xcc).hlsl_ps()
    finally:
        G.Stage.launch_mode = saved
    # the .psin line: two fields as before, a third FLOAT_MODE field, malformed ones refused
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'r.psin'
        for text_in, want in (('spi_ps_input 1 00000002 00000002\n', (2, 2)),
                              ('spi_ps_input 1 00000002 00000002 000000cc\n', (2, 2, 0xcc)),
                              ('spi_ps_input 1 00008802 00008802 000000c0\nvs_input 1 0 00000000\n', (0x8802, 0x8802, 0xc0))):
            path.write_text(text_in, newline='')
            assert G.read_ps_inputs(str(path)) == want, (text_in, G.read_ps_inputs(str(path)))
        for bad_in in ('spi_ps_input 1 00000002 00000002 cc\n', 'spi_ps_input 1 00000002 00000002 00000100\n',
                       'spi_ps_input 1 00000002 00000002  000000cc\n'):
            path.write_text(bad_in, newline='')
            try:
                G.read_ps_inputs(str(path))
            except G.Unsupported as error:
                assert 'invalid SPI input file' in str(error)
            else:
                raise AssertionError('accepted %r' % bad_in)
    print('launch FLOAT_MODE: 0xcc rounds f16 toward zero without s_round_mode, 0xc0 nearest even, not passed refused before s_round_mode; '
          '0xcd 0xc4 0xf0 0x0c 0x00 refused; without launch_mode 0xcc stays nearest even (control); .psin third field parsed')


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
            # the stage without s_round_mode (MODE 0): x2w_f32tof16_rne must draw the nearest-even words
            plain_text = pipe(pixel_words(mode=None)).hlsl_ps()
            run = draw(renderer, adapter, vs, build('ps_plain', plain_text, ps_profile), nearest)
            if run.returncode:
                raise AssertionError('%s stage without s_round_mode\n%s\n%s' % (kind, run.stdout, run.stderr))
            print('%s without s_round_mode (x2w_f32tof16_rne): %s' % (kind, nearest), flush=True)
            # the stage without s_round_mode under launch FLOAT_MODE 0xcc: toward zero again
            run = draw(renderer, adapter, vs, build('ps_launch', pipe(pixel_words(mode=None), launch=0xcc).hlsl_ps(),
                                                    ps_profile), EXPECTED)
            if run.returncode:
                raise AssertionError('%s launch FLOAT_MODE 0xcc\n%s\n%s' % (kind, run.stdout, run.stderr))
            print('%s launch FLOAT_MODE 0xcc, no s_round_mode: %s' % (kind, EXPECTED), flush=True)
            # informational, not asserted: how this adapter's own f32tof16 rounds (the helpers replaced by it).
            # MEASURED on WARP and an RX 9070 XT (fxc ps_5_1, dxc ps_6_0): toward zero.
            raw = plain_text.replace('x2w_f32tof16_rne(F(', 'f32tof16(F(')
            assert raw != plain_text
            run = draw(renderer, adapter, vs, build('ps_raw', raw, ps_profile), EXPECTED)
            print('%s HLSL f32tof16 alone: %s' % (kind, 'toward zero (matches the truncated words)' if run.returncode == 0
                                                  else 'NOT toward zero'), flush=True)


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
