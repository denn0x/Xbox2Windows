"""Binary16 attribute interpolation (v_interp_p1ll_f16 + v_interp_p2_f16) in tools/startup/gfx2hlsl.py.

  (no arguments)                       translation controls, pure Python
  draw <dxc> <offscreen_check> <warp|hardware> [fxc]
                                       translate, compile (FXC vs_5_1/ps_5_1 when given, DXC vs_6_0/ps_6_0),
                                       draw one pixel (the startup_flat_param renderer) and compare the words

The vertex stage is startup_flat_param_check's full-screen triangle with four constant param0 words, each
holding two binary16 values. The pixel stage interpolates every half of attr0 (p1ll + p2_f16, `high` for bits
31:16) and packs the two results of a channel back into one VGPR: the low half with a plain p2_f16, the high half
with op_sel bit 3 set (Mesa aco_assembler.cpp emit_vintrp_instruction: v_interp_p2_hi_f16 is v_interp_p2_f16 with
opsel 0x8; LLVM 15 and the decoder do not print the bit). A constant attribute interpolates to itself, so mrt0 must
read the exported words.

Encodings (hand-assembled, no llvm-mc here; the decoder reads them back): VOP3 word 0 = 0b110101 << 26 |
opcode << 16 | op_sel << 11 | vdst, word 1 = attribute | channel << 6 | high << 8 | src1 << 9 | src2 << 18 (Mesa
aco_assembler.cpp, gfx10 opcodes p1ll 0x342, p2 0x35a from aco_opcodes.py).

Controls: without the p2 handler the stage refuses as before; with op_sel bit 3 cleared the high halves are lost
(text, and the draw mismatches); another op_sel bit, and clamp, are refused; a pixel stage that reads attr0 only
as f32 gets no half copies; the mesh pipeline drops the half copies of attributes the pixel stage does not read.
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_flat_param_check import LINEAR_PS_WORDS, VS_WORDS as FLAT_VS, root_blob, compile_hlsl, draw  # noqa: E402

# binary16 pairs (high, low): (-2.5, 1.0), (65504, 0.5), (2^-24 denormal, 2^-14), (0.333, -1.0)
WORDS = (0xc1003c00, 0x7bff3800, 0x00010400, 0x3555bc00)
EXPECTED = ','.join('%08x' % w for w in WORDS)


def v_mov_literal(reg, value):
    return struct.pack('<II', 0x7e000000 | reg << 17 | 1 << 9 | 0xff, value)


def vertex_words():
    # the flat-param vertex stage up to v_mov_b32 v5, 0 (8 instructions, 0x28 bytes), then v6..v9 = WORDS
    head = FLAT_VS[:0x28]
    tail = FLAT_VS[FLAT_VS.find(bytes.fromhex('cf0800f8')):]
    return head + b''.join(v_mov_literal(6 + i, w) for i, w in enumerate(WORDS)) + tail


def interp(op, vdst, attr, chan, high, src1, src2=0, opsel=0, clamp=0):
    w0 = 0b110101 << 26 | op << 16 | opsel << 11 | clamp << 15 | vdst
    w1 = attr | chan << 6 | high << 8 | (256 + src1) << 9 | ((256 + src2) << 18 if op == 0x35a else 0)
    return struct.pack('<II', w0, w1)


def pixel_words(hi_opsel=8, clamp=0):
    w = bytes.fromhex('0203fcbe')  # s_mov_b32 m0, s2
    for chan in range(4):
        d = 2 + chan
        w += interp(0x342, 6, 0, chan, 0, 0)                     # v_interp_p1ll_f16 v6, v0, attr0.c
        w += interp(0x35a, d, 0, chan, 0, 1, 6, clamp=clamp)     # v_interp_p2_f16 vd, v1, attr0.c, v6
        w += interp(0x342, 7, 0, chan, 1, 0)                     # ... high
        w += interp(0x35a, d, 0, chan, 1, 1, 7, opsel=hi_opsel)  # ... high, to the high half of vd
    return w + bytes.fromhex('0f1800f802030405') + bytes.fromhex('000081bf')  # exp mrt0 v2..v5 done vm


def pipe(ps):
    return G.Pipeline(vertex_words(), ps, root_blob())


def refused(ps, text):
    try:
        pipe(ps).hlsl_ps()
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted, expected %r' % text)


def controls():
    asm = [a for _, _, _, a in G.decode(pixel_words())]
    assert asm[1:5] == ['v_interp_p1ll_f16 v6, v0, attr0.x', 'v_interp_p2_f16 v2, v1, attr0.x, v6',
                        'v_interp_p1ll_f16 v7, v0, attr0.x high', 'v_interp_p2_f16 v2, v1, attr0.x, v7 high'], asm
    vs_asm = [a for _, _, _, a in G.decode(vertex_words())]
    assert vs_asm[7:12] == ['v_mov_b32_e32 v5, 0'] + [
        'v_mov_b32_e32 v%d, 0x%x' % (6 + i, w) for i, w in enumerate(WORDS)], vs_asm
    p = pipe(pixel_words())
    vs, ps = p.hlsl_vs(), p.hlsl_ps()
    for text in (vs, ps):
        assert 'float4 loparam0 : LOPARAM0;' in text and 'float4 hiparam0 : HIPARAM0;' in text, text
    assert '    o.loparam0 = float4(f16tof32(v6), f16tof32(v7), f16tof32(v8), f16tof32(v9));' in vs, vs
    assert '    o.hiparam0 = float4(f16tof32((v6) >> 16), f16tof32((v7) >> 16), f16tof32((v8) >> 16), ' \
           'f16tof32((v9) >> 16));' in vs, vs
    assert '    o.loparam0 = p.loparam0 + r.loparam0 - q.loparam0;' in vs, vs  # the rectangle-list corner
    for chan, d in zip('xyzw', range(2, 6)):
        assert '    v%d = (v%d & 0xffff0000u) | x2w_f32tof16_rne(i.loparam0.%s);' % (d, d, chan) in ps, ps
        assert '    v%d = (v%d & 0xffffu) | (x2w_f32tof16_rne(i.hiparam0.%s) << 16);' % (d, d, chan) in ps, ps
    print('interp f16: VS exports loparam0/hiparam0 (f16tof32 of each half); PS packs both halves rounded to nearest even (x2w_f32tof16_rne)')

    # controls
    saved = G.Stage.op_v_interp_p2_f16
    del G.Stage.op_v_interp_p2_f16
    try:
        refused(pixel_words(), 'v_interp_p2_f16')
    finally:
        G.Stage.op_v_interp_p2_f16 = saved
    lost = pipe(pixel_words(hi_opsel=0)).hlsl_ps()
    assert '(x2w_f32tof16_rne(i.hiparam0.x) << 16)' not in lost and '    v2 = (v2 & 0xffff0000u) | x2w_f32tof16_rne(i.hiparam0.x);' \
        in lost, lost
    refused(pixel_words(hi_opsel=1), 'v_interp_p2_f16 with op_sel 0x1')
    refused(pixel_words(clamp=1), 'v_interp_p2_f16 with clamp')
    # A compressed param export (upstream PR #2052's form, `exp param0 v6, v6, v7, v7 compr`) puts each half in its
    # own channel, so the words are not halves: a binary16 read of it is refused, not read as halves of f32 bits.
    plain_exp = bytes.fromhex('0f0200f806070809')
    vs = vertex_words()
    assert vs.count(plain_exp) == 1
    compr_vs = vs.replace(plain_exp, struct.pack('<I', 0xf800020f | 1 << 10) + bytes.fromhex('06070000'))
    assert [a for _, _, _, a in G.decode(compr_vs)][-2] == 'exp param0 v6, v6, v7, v7 compr', G.decode(compr_vs)
    try:
        G.Pipeline(compr_vs, pixel_words(), root_blob())
        raise AssertionError('compr export read as halves accepted')
    except G.Unsupported as error:
        assert 'binary16 interpolation of attribute [0] exported with compr' in str(error), error
    assert 'i.param0' in G.Pipeline(compr_vs, LINEAR_PS_WORDS, root_blob()).hlsl_ps()  # f32 reads unaffected
    linear = pipe(LINEAR_PS_WORDS)
    assert 'loparam' not in linear.hlsl_vs() + linear.hlsl_ps() and 'hiparam' not in linear.hlsl_vs()
    # mesh pipelines: the stage's half copies are kept only for attributes the pixel stage reads as halves
    line = 'if (exec & 1) { o.param0 = float4(F(v6), 0.0, 0.0, 0.0); o.fparam0 = uint4(v6, 0u, 0u, 0u); ' \
           'o.loparam0 = float4(f16tof32(v6), 0.0, 0.0, 0.0); o.hiparam0 = float4(f16tof32((v6) >> 16), 0.0, 0.0, 0.0); }'

    class StubMesh:
        def __init__(self, pipe, code):
            self.exports, self.exported_prim = {'pos': ['0'] * 4, 'param0': ['0'] * 4}, True
            self.broadcasts, self.lds_used, self.wave_any, self.used_v = 0, False, [], set()

        def __getattr__(self, name):  # other per-wave tables hlsl_ms reads (scc_any, ...): empty
            return []

        def body(self):
            return ['    ' + line]

        def declarations(self):
            return []
    saved = M.MeshStage
    M.MeshStage = StubMesh
    try:
        kept = M.MeshPipeline(b'', pixel_words(), root_blob()).hlsl_ms()
        dropped = M.MeshPipeline(b'', LINEAR_PS_WORDS, root_blob()).hlsl_ms()
    finally:
        M.MeshStage = saved
    assert 'o.loparam0 = ' in kept and 'o.hiparam0 = ' in kept and 'float4 loparam0 : LOPARAM0;' in kept, kept
    assert 'loparam0' not in dropped and 'hiparam0' not in dropped and 'o.param0 = ' in dropped, dropped
    print('controls: refused without the p2 handler; op_sel cleared loses the high halves; op_sel 0x1, clamp and a compr export '
          'refused; no half copies for an f32-only reader; mesh stage keeps them only when read')


def check_draw(dxc, renderer, adapter, fxc=None):
    compilers = ([('fxc', fxc, 'vs_5_1', 'ps_5_1')] if fxc else []) + [('dxc', dxc, 'vs_6_0', 'ps_6_0')]
    good, lost = pipe(pixel_words()), pipe(pixel_words(hi_opsel=0))
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        for kind, compiler, vs_profile, ps_profile in compilers:
            def build(name, source, profile):
                return compile_hlsl(compiler, kind, source, '%s_%s' % (name, kind), profile, directory)
            vs = build('vs', good.hlsl_vs(), vs_profile)
            run = draw(renderer, adapter, vs, build('ps', good.hlsl_ps(), ps_profile), EXPECTED)
            if run.returncode:
                raise AssertionError('%s translated pair\n%s\n%s' % (kind, run.stdout, run.stderr))
            print('%s translated pair: %s' % (kind, EXPECTED), flush=True)
            run = draw(renderer, adapter, build('vs_lost', lost.hlsl_vs(), vs_profile),
                       build('ps_lost', lost.hlsl_ps(), ps_profile), EXPECTED)
            if run.returncode == 0:
                raise AssertionError('%s control with op_sel cleared matched' % kind)
            print('%s control, op_sel cleared: mismatches as expected (%s)' % (kind, run.stderr.strip()[-80:]),
                  flush=True)


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
