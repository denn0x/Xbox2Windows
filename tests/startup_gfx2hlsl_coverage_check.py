"""Lowerings added to tools/startup/gfx2hlsl.py for the startup pipeline requests, pure Python.

Each case hand-encodes gfx1013 words (no title data), checks the shared disassembler prints the form the lowering
parses, and checks the translated statement. The integer helpers (64-bit shifts, the high word of a product and the
IEEE class of a binary32 or binary16 value) are evaluated by the integer HLSL evaluator of
startup_gfx2hlsl_bit_alu_check over edge grids and compared with references written from the ISA description with
Python integers and struct, not with the lowering's own expressions. Each lowering also has refusal controls for the
forms it does not prove.

Semantics sources: LLVM AMDGPU (SIDefines.h ClassFlags, llvm.amdgcn.class(value, mask); GCNSubtarget
zeroesHigh16BitsOfDest for the gfx10 16-bit results; SIFoldOperands "Clamp is applied after omod";
ConstantFoldAMDGCNCubeIntrinsic for v_cube*; SIInstrInfo s_and/s_cmp -> s_bitcmp folds; SOPInstructions.td for
the s_lshlN_add / s_abs / s_pack_ll patterns and which scalar ops define SCC; SIShrinkInstructions for the signed
SOPK immediate; AMDGPUISelDAGToDAG SelectVOP3PMadMixModsImpl and VOP3PInstructions.td VOP3P_Mix_Profile for the
mix operands; SIISelLowering packImage16bitOpsToDwords for a16) and Mesa main 13ebbc00 (ACO selection of
pack_[us]norm_2x16 to v_cvt_pknorm_*, nir_constant_expressions pack_[us]norm_1x16, util/macros.h CLAMP).
"""
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_gfx2hlsl_bit_alu_check import Expr  # noqa: E402

MASK = 0xffffffff
failures = []


def check(label, ok):
    print('%s: %s' % (label, 'ok' if ok else 'MISMATCH'))
    if not ok:
        failures.append(label)


class FakePipe:
    """The pipeline services one stage needs: resources, samplers, entry constants and the render-target kinds."""
    root = {'params': [{'type': 0}], 'flags': 0}
    sampler_heap = False
    uses_sampler_heap = False

    def __init__(self):
        self.resources, self.samplers, self.modified, self.notes = {}, {}, set(), set()

    def resource(self, key, kind):
        r = self.resources.setdefault(key, dict(index=len(self.resources), kind=kind))
        assert r['kind'] == kind
        return 'r%d' % r['index']

    def sampler(self, key, comparison=False):
        return 'q%d' % self.samplers.setdefault(key, len(self.samplers))

    def ud(self, source):
        return 0

    def mrt_kind(self, slot):
        return 'float'


def words_bytes(words):
    return struct.pack('<%dI' % len(words), *words)


def stage(words, kind='ps'):
    """Translate a whole program (s_endpgm appended)."""
    return G.Stage(FakePipe(), kind, words_bytes(list(words) + [0xbf810000]))


def asm_of(words):
    return G.decode(words_bytes(words))[0][3]


def lines(words, kind='ps', setup=None):
    """Translate one instruction on a fresh stage after an optional setup(stage); returns its statements."""
    s = stage([], kind)
    if setup:
        setup(s)
    s.lines = []
    _, n, name, asm = G.decode(words_bytes(words))[0]
    s.pc = 0
    s.one(name, asm)
    return s, '\n'.join(s.lines)


def refused(asm, kind='ps', setup=None, words=None):
    s = stage([], kind)
    if setup:
        setup(s)
    try:
        if words is not None:
            _, n, name, asm = G.decode(words_bytes(words))[0]
        else:
            name = asm.split(' ', 1)[0]
        s.one(name, asm)
    except G.Unsupported:
        return True
    return False


def helper_expr(name):
    text = G.HELPERS[name]
    return Expr(text[text.index('return ') + 7:text.rindex(';')])


# ---- exports -------------------------------------------------------------------------------------
def exports():
    w = [0xf8000000 | 0x3 | 1 << 10 | 1 << 11 | 1 << 12, 0x00000100]
    check('exp compr with the zw pair off prints as v0, v0, off, off', asm_of(w) == 'exp mrt0 v0, v0, off, off done compr vm')
    s, text = lines(w)
    check('  x, y from the halves of v0; z, w read 0', 'o.mrt0 = float4(f16tof32(v0), f16tof32(v0 >> 16), 0.0, 0.0);' in text)
    w = [0xf8000000 | 0xc | 1 << 10 | 1 << 11, 0x00000100]
    check('exp compr with the xy pair off prints as off, off, v1, v1', asm_of(w) == 'exp mrt0 off, off, v1, v1 done compr')
    check('  z, w from the halves of v1', 'float4(0.0, 0.0, f16tof32(v1), f16tof32(v1 >> 16))' in lines(w)[1])
    check('exp compr with an unpaired enable mask refused',
          refused(None, words=[0xf8000000 | 0x1 | 1 << 10 | 1 << 11, 0x00000100]))
    w = [0xf8000000 | 0x1 | 8 << 4 | 1 << 11 | 1 << 12, 5]
    check('exp mrtz depth alone', asm_of(w) == 'exp mrtz v5, off, off, off done vm' and 'o.depth = F(v5);' in lines(w)[1])
    check('exp mrtz with stencil refused', refused(None, words=[0xf8000000 | 0x3 | 8 << 4 | 1 << 11, 0x0605]))
    check('exp mrtz in a vertex stage refused', refused(None, 'vs', words=w))
    w = [0xf8000000 | 9 << 4 | 1 << 11 | 1 << 12, 0]
    check('exp null with no data is a no-op', asm_of(w) == 'exp null off, off, off, off done vm' and lines(w)[1] == '')
    check('exp null with data refused', refused(None, words=[0xf8000000 | 0x1 | 9 << 4 | 1 << 11, 0]))
    # A whole pixel stage that exports depth and colour declares SV_Depth next to SV_Target0.
    pipe = G.Pipeline.__new__(G.Pipeline)
    pipe.rtv_formats = ()
    pipe.packed_position_vgpr = None  # set by Pipeline.__init__ since upstream #1634
    pipe.resources, pipe.samplers, pipe.uds, pipe.uses_sampler_heap, pipe.helper_text = {}, {}, [], False, None
    pipe.vs = pipe.ps = None
    pipe.root = FakePipe.root
    pipe.ps = G.Stage(pipe, 'ps', words_bytes([0xf8000000 | 0x1 | 8 << 4, 1, 0xf8000000 | 0xf | 1 << 11 | 1 << 12, 0x03020100,
                                               0xbf810000]))
    pipe.vs = pipe.ps
    pipe.params, pipe.flat = [], []
    text = pipe.hlsl_ps()
    check('depth and colour: SV_Depth and SV_Target0 declared',
          'float depth : SV_Depth;' in text and 'float4 mrt0 : SV_Target0;' in text and 'o.depth = F(v1);' in text)


# ---- output modifiers ------------------------------------------------------------------------------
def output_modifiers():
    w = [0xd4000000 | 0x185 << 16, 256 | 1 << 27]
    check('v_cvt_f32_i32 omod prints mul:2', asm_of(w) == 'v_cvt_f32_i32_e64 v0, v0 mul:2')
    check('  the converted value is doubled', 'v0 = U((F(U(float(int(v0))))) * 2.0);' in lines(w)[1])
    check('omod on an integer result refused', refused('v_add_nc_u32_e64 v0, v0, v1 mul:2'))
    check('clamp on an integer result refused', refused('v_add_nc_u32_e64 v0, v0, v1 clamp'))
    w = [0xd4000000 | 0x14b << 16 | 1 << 15 | 1, 258 | 259 << 9 | 0xf2 << 18 | 3 << 27 | 1 << 29]
    check('v_fma_f32 clamp div:2 printed with clamp first', asm_of(w) == 'v_fma_f32 v1, -v2, v3, 1.0 clamp div:2')
    check('  the clamp applies after the output modifier',
          'v1 = U(saturate(((-F(v2)) * F(v3) + 1.0) * 0.5));' in lines(w)[1])


# ---- bit-exact selects and 16-bit results ---------------------------------------------------------------
def selects_and_halves():
    w = [0xd4000000 | 0x101 << 16 | 31, 286 | 286 << 9 | 106 << 18 | 2 << 29]
    check('v_cndmask_b32 with -v30 prints the modifier', asm_of(w) == 'v_cndmask_b32_e64 v31, v30, -v30, vcc_lo')
    check('  negation flips the sign bit of the selected word', 'v31 = (vcc & 1) ? (v30 ^ 0x80000000u) : v30;' in lines(w)[1])
    w = [0xd4000000 | 0x101 << 16 | 2 << 8 | 3, 259 | 260 << 9 | 106 << 18]
    check('  abs clears it', 'v3 = (vcc & 1) ? (v4 & 0x7fffffffu) : v3;' in lines(w)[1])
    w = [0x3a << 25 | 21 << 17 | 21 << 9 | 0xff, 0x7bff]
    check('v_min_f16 keeps D[31:16] (gfx10)', asm_of(w) == 'v_min_f16_e32 v21, 0x7bff, v21' and
          'v21 = (v21 & 0xffff0000u) | f32tof16(min(f16tof32(0x00007bffu), f16tof32(v21)));' in lines(w)[1])
    w = [0x39 << 25 | 21 << 17 | 21 << 9 | 0xf2]
    check('v_max_f16 takes inline 1.0 in half form', asm_of(w) == 'v_max_f16_e32 v21, 1.0, v21' and
          'f16tof32(0x3c00u)' in lines(w)[1])
    check('v_min_f16 with clamp refused', refused('v_min_f16_e64 v1, v2, v3 clamp'))
    check('v_min_f16 with a negated source refused', refused('v_min_f16_e64 v1, -v2, v3'))
    w = [0x7e000000 | 5 << 17 | 0xa << 9 | 262]
    check('v_cvt_f16_f32 keeps D[31:16] (gfx10)', asm_of(w) == 'v_cvt_f16_f32_e32 v5, v6' and
          'v5 = (v5 & 0xffff0000u) | f32tof16(F(v6));' in lines(w)[1])
    w = [0xcc000000 | 0x11 << 16 | 1 << 14 | 3, 259 | 0xff << 9 | 1 << 27, 0x7bef]
    check('v_pk_min_f16 op_sel_hi:[1,0] prints', asm_of(w) == 'v_pk_min_f16 v3, v3, 0x7bef op_sel_hi:[1,0]')
    check('  low from both low halves, high from v3.hi and the literal low half',
          lines(w)[1] == 'v3 = f32tof16(min(f16tof32(v3), f16tof32(0x00007befu))) | '
                         '(f32tof16(min(f16tof32(v3 >> 16), f16tof32(0x00007befu))) << 16);')
    check('v_pk_min_f16 reading a literal high half refused',
          refused(None, words=[0xcc000000 | 0x11 << 16 | 1 << 14 | 3, 259 | 0xff << 9 | 3 << 27, 0x7bef]))
    check('v_pk_min_f16 with neg_lo refused', refused('v_pk_min_f16 v3, v3, v4 neg_lo:[1,0]'))


# ---- integer helpers ----------------------------------------------------------------------------------
EDGES = [0, 1, 2, 3, 0x7fff, 0x8000, 0xffff, 0x10000, 0x12345678, 0x7fffffff, 0x80000000, 0x80000001, 0xaaaaaaab,
         0xdeadbeef, 0xfffeffff, 0xffff0000, 0xfffffffe, MASK]


def integer_helpers():
    e = helper_expr('x2w_mul_hi_u32')
    bad = [(a, b) for a in EDGES for b in EDGES if e.run({'a': a, 'b': b})[0] != (a * b) >> 32]
    check('x2w_mul_hi_u32 matches (a * b) >> 32 on %d pairs' % len(EDGES) ** 2, not bad)
    w = [0xd4000000 | 0x16a << 16 | 1, 256 | 0xff << 9, 0xaaaaaaab]
    check('v_mul_hi_u32 lowers to the helper', asm_of(w) == 'v_mul_hi_u32 v1, v0, 0xaaaaaaab' and
          'v1 = x2w_mul_hi_u32(v0, 0xaaaaaaabu);' in lines(w)[1])
    pairs = [(l, h) for l in (0, 1, 0x80000000, 0x12345678, MASK) for h in (0, 1, 0x80000000, 0xdeadbeef, MASK)]
    for helper, ref in (('x2w_lshr64', lambda v, n: v >> n), ('x2w_lshl64', lambda v, n: (v << n) & (2 ** 64 - 1))):
        lo, hi = helper_expr(helper + '_lo'), helper_expr(helper + '_hi')
        bad = []
        for l, h in pairs:
            for n in list(range(64)) + [64, 65, 95, 0xffffffc1]:
                want = ref(h << 32 | l, n & 63)
                env = {'l': l, 'h': h, 'n': n}
                if (lo.run(env)[0], hi.run(env)[0]) != (want & MASK, want >> 32):
                    bad.append((l, h, n))
        check('%s matches the 64-bit reference for counts 0..63 and wrapped counts' % helper, not bad)
    w = [0xd4000000 | 0x300 << 16 | 10, 0x80 | 0x80 << 9]
    check('v_lshrrev_b64 v[10:11], 0, 0', asm_of(w) == 'v_lshrrev_b64 v[10:11], 0, 0' and
          'q = uint2(x2w_lshr64_lo(0x00000000u, 0x00000000u, 0x00000000u), '
          'x2w_lshr64_hi(0x00000000u, 0x00000000u, 0x00000000u));\nv10 = q.x;\nv11 = q.y;' in lines(w)[1])
    w = [0xd4000000 | 0x2ff << 16 | 2, 257 | 258 << 9]
    check('v_lshlrev_b64 v[2:3], v1, v[2:3] reads both words before writing',
          lines(w)[1] == 'q = uint2(x2w_lshl64_lo(v2, v3, v1), x2w_lshl64_hi(v2, v3, v1));\nv2 = q.x;\nv3 = q.y;')
    check('a negative inline 64-bit constant is sign-extended',
          'x2w_lshr64_lo(0xffffffffu, 0xffffffffu, v1)' in lines([0xd4000000 | 0x300 << 16 | 10, 257 | 0xc1 << 9])[1])
    check('a 64-bit literal operand refused', refused('v_lshrrev_b64 v[10:11], v1, 0x12345678'))
    w = [0xbf000000 | 0xd << 16 | 0x81 << 8 | 11]
    s, text = lines(w)
    check('s_bitcmp1_b32 s11, 1 sets SCC from bit 1', asm_of(w) == 's_bitcmp1_b32 s11, 1' and
          text == 'scc = ((s11 >> (0x00000001u & 31u)) & 1u) == 1u;')
    e = Expr(text[len('scc = '):-1].replace('s11', 'x'))
    check('  SCC is S0[S1[4:0]] for every bit', all(e.run({'x': x})[0] == (x >> 1) & 1 for x in EDGES))
    check('s_bitcmp0_b32 tests for zero', 'scc = ((s3 >> (s4 & 31u)) & 1u) == 0u;' in lines([0xbf000000 | 0xc << 16 | 4 << 8 | 3])[1])
    w = [0x80000000 | 0x25 << 23 | 44 << 16 | (0x80 + 54) << 8 | 0x82]
    s, text = lines(w)
    check('s_bfm_b64 s[44:45], 2, 54 folds to 0x00c00000:00000000', asm_of(w) == 's_bfm_b64 s[44:45], 2, 54' and
          s.sym.get(44) == ('const', 0) and s.sym.get(45) == ('const', 0x00c00000))
    check('s_bfm_b64 with a run-time count refused', refused('s_bfm_b64 s[4:5], s6, 0'))


def ref_class(x, half):
    """IEEE class bit (SIDefines.h ClassFlags order) of a binary32 or binary16 pattern."""
    if half:
        x &= 0xffff
        value, quiet, sign, tiny = struct.unpack('<e', struct.pack('<H', x))[0], x >> 9 & 1, x >> 15, 2.0 ** -14
    else:
        value, quiet, sign, tiny = struct.unpack('<f', struct.pack('<I', x))[0], x >> 22 & 1, x >> 31, 2.0 ** -126
    if math.isnan(value):
        return 1 if quiet else 0
    if math.isinf(value):
        return 2 if sign else 9
    if value == 0:
        return 5 if sign else 6
    if abs(value) >= tiny:
        return 3 if sign else 8
    return 4 if sign else 7


CLASS32 = [0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
           0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7f800001, 0xff800001, 0x7fbfffff, 0x7fc00000, 0xffc00000,
           MASK, 0x7fffffff]
CLASS16 = [0, 0x8000, 1, 0x8001, 0x3ff, 0x83ff, 0x400, 0x8400, 0x3c00, 0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7c01,
           0xfc01, 0x7dff, 0x7e00, 0xfe00, 0xffff, 0x7fff, 0x12340000 | 0x7c01]


def classes():
    for name, grid, half in (('x2w_class_f32', CLASS32, False), ('x2w_class_f16', CLASS16, True)):
        e = helper_expr(name)
        bad = [(x, bit) for x in grid for bit in range(10)
               if e.run({'x': x, 'mask': 1 << bit})[0] != int(ref_class(x, half) == bit)]
        bad += [x for x in grid if e.run({'x': x, 'mask': 0xfffffc00})[0]]  # mask bits above 9 never match
        check('%s matches the IEEE class of %d patterns for every mask bit' % (name, len(grid)), not bad)
    w = [0x7c000000 | 0x88 << 17 | 0xc1]
    check('v_cmp_class_f32 vcc_lo, -1, v0: value -1, mask v0', asm_of(w) == 'v_cmp_class_f32_e32 vcc_lo, -1, v0' and
          'vcc = x2w_class_f32(0xffffffffu, v0);' in lines(w)[1])
    w = [0x7c000000 | 0x8f << 17 | 0xff, 0x7c01]
    check('v_cmp_class_f16 with a literal value', asm_of(w) == 'v_cmp_class_f16_e32 vcc_lo, 0x7c01, v0' and
          'vcc = x2w_class_f16(0x00007c01u, v0);' in lines(w)[1])
    w = [0xd4000000 | 0x88 << 16 | 32, 0xc1 | 260 << 9]
    s, text = lines(w)
    check('v_cmp_class_f32_e64 writes an SGPR mask', 's32 = x2w_class_f32(0xffffffffu, v4);' in text and 32 in s.masks)
    check('  -|v| sets the f32 sign bit', '((v2 & 0x7fffffffu) ^ 0x80000000u)' in lines([0xd4000000 | 0x88 << 16 | 1 << 8 | 32,
                                                                                         258 | 260 << 9 | 1 << 29])[1])
    check('v_cmpx_class_f32 writes exec', 'exec = x2w_class_f32(v1, v2);' in lines([0x7c000000 | 0x98 << 17 | 2 << 9 | 257])[1])
    check('v_cmp_lt_f16 still refused', refused('v_cmp_lt_f16_e32 vcc_lo, v1, v2'))


# ---- cube-map face selection ---------------------------------------------------------------------------
def ref_cube(s0, s1, s2):
    """(id, sc, tc, ma) as LLVM's ConstantFoldAMDGCNCubeIntrinsic computes them."""
    def negative(x):
        return math.copysign(1.0, x) < 0 and x != 0 and not math.isnan(x)
    if abs(s2) >= abs(s0) and abs(s2) >= abs(s1):
        face, sc, tc, ma = (5, -s0, -s1, s2) if negative(s2) else (4, s0, -s1, s2)
    elif abs(s1) >= abs(s0):
        face, sc, tc, ma = (3, s0, -s2, s1) if negative(s1) else (2, s0, s2, s1)
    else:
        face, sc, tc, ma = (1, s2, -s1, s0) if negative(s0) else (0, -s2, -s1, s0)
    return float(face), sc, tc, ma + ma


def cube_helper():
    """The HLSL of x2w_cube as a Python function: if/return statements, ternaries on 'x < 0.0', float4 tuples."""
    text = G.HELPERS['x2w_cube']
    body = text[text.index('{') + 1:text.rindex('}')]
    body = re.sub(r'(\w) < 0\.0 \? ([^,]+?) : ([^,)]+)', r'(\2 if \1 < 0.0 else \3)', body)
    body = body.replace('&&', 'and').replace('float4(', '(')
    source = 'def cube(a, b, c):\n'
    for statement in filter(None, (s.strip() for s in body.split(';'))):
        if statement.startswith('if'):
            cond, ret = re.fullmatch(r'if \((.*)\) (return .*)', statement).groups()
            source += '    if %s:\n        %s\n' % (cond, ret)
        else:
            source += '    %s\n' % statement
    space = {}
    exec(source, space)
    return space['cube']


def cubes():
    cube = cube_helper()
    values = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, float('inf'), float('-inf'), float('nan')]
    bad = []
    for a in values:
        for b in values:
            for c in values:
                got, want = cube(a, b, c), ref_cube(a, b, c)
                if any(not (x == y or (math.isnan(x) and math.isnan(y))) or math.copysign(1, x) != math.copysign(1, y)
                       for x, y in zip(got, want)):
                    bad.append((a, b, c, got, want))
    check('x2w_cube matches LLVM cube folding on %d inputs (ties, -0, inf, NaN)' % len(values) ** 3, not bad)
    s, text = lines([0xd4000000 | 0x147 << 16 | 23, 276 | 278 << 9 | 274 << 18])
    check('v_cubema_f32 lowers to x2w_cube(...).w', text == 'v23 = U(x2w_cube(F(v20), F(v22), F(v18)).w);')


# ---- image_gather4_lz and the typed 16x4 float fetch ---------------------------------------------------------
def gather_setup(s):
    for i in range(8):
        s.sym[24 + i] = ('desc', ('table', 0), 0x40, i)
    for i in range(4):
        s.sym[16 + i] = ('const', 0x100 * (i + 1))


def gather():
    base = 0xf0000000 | (0x47 & 0x7f) << 18 | 1 << 3
    w1 = 2 | 8 << 8 | (24 // 4) << 16 | (16 // 4) << 21
    for dmask, channel in ((1, 'Red'), (2, 'Green'), (4, 'Blue'), (8, 'Alpha')):
        w = [base | dmask << 8, w1]
        _, text = lines(w, setup=gather_setup)
        check('image_gather4_lz dmask %#x reads Gather%s into four VGPRs' % (dmask, channel),
              'dmask:0x%x dim:SQ_RSRC_IMG_2D' % dmask in asm_of(w) and
              text == 't = r0.Gather%s(q0, float2(F(v2), F(v3)));\nv8 = U(t.x);\nv9 = U(t.y);\nv10 = U(t.z);\nv11 = U(t.w);' % channel)
    check('image_gather4_lz with two channels refused', refused(None, setup=gather_setup, words=[base | 3 << 8, w1]))
    check('image_gather4_lz tfe refused', refused(None, setup=gather_setup, words=[base | 2 << 8 | 1 << 16, w1]))
    check('image_gather4_lz d16 refused', refused(None, setup=gather_setup, words=[base | 2 << 8, w1 | 1 << 31]))
    check('image_gather4_lz on a 2D array refused',
          refused(None, setup=gather_setup, words=[(base & ~(7 << 3)) | 5 << 3 | 2 << 8, w1]))
    check('image_gather4 (implicit level) still refused', refused(None, setup=gather_setup,
                                                                 words=[0xf0000000 | (0x40 & 0x7f) << 18 | 1 << 3 | 1 << 8, w1]))


def typed_fetch():
    def setup(s):
        for i in range(4):
            s.sym[20 + i] = ('desc', ('table', 0), 0x80, i)
    s = stage([])
    setup(s)
    s.lines = []
    s.one('tbuffer_load_format_xyzw', 'tbuffer_load_format_xyzw v[0:3], v4, s[20:23], 0 format:[BUF_FMT_16_16_16_16_FLOAT] offen')
    text = '\n'.join(s.lines)
    check('BUF_FMT_16_16_16_16_FLOAT: four halves from two dwords',
          text == 'a = v4 + 0;\nw = ldu(r0, a);\nv0 = U(f16tof32(w));\nv1 = U(f16tof32(w >> 16));\n'
                  'v2 = U(f16tof32(ldu(r0, a + 4)));\nv3 = U(f16tof32(ldu(r0, a + 4) >> 16));')


# ---- control-flow joins ---------------------------------------------------------------------------------
JOIN = [0xf4000000 | 2 << 18 | 12 << 6 | 4, 125 << 25 | 0x20,       # s_load_dwordx4 s[12:15], s[8:9], 0x20
        0xbf000000 | 6 << 16 | 0x80 << 8 | 20,                      # s_cmp_eq_u32 s20, 0
        0xbf840000 | 3,                                             # s_cbranch_scc0 +0x1c
        0xf4000000 | 0xa << 18 | 12 << 6 | 6, 125 << 25 | 0x50,     # s_buffer_load_dwordx4 s[12:15], s[12:15], 0x50
        0xbf820000 | 2,                                             # s_branch +0x24
        0xf4000000 | 8 << 18 | 16 << 6 | 6, 125 << 25 | 0x10]       # s_buffer_load_dword s16, s[12:15], 0x10


def joins():
    s = stage(JOIN)
    text = '\n'.join(line for block in s.blocks for line in block[1])
    check('a branch target reached only by the branch keeps the descriptor the branch saw',
          's16 = r0.Load(0x00000010u + 0);' in text and s.pipe.resources == {(('table', 0), 0x20): dict(index=0, kind='buf')})
    fall = JOIN[:6] + [0xbf800000] + JOIN[7:]  # s_nop instead of s_branch: the clobbered path falls through
    try:
        stage(fall)
    except G.Unsupported as error:
        check('a join of a descriptor and a loaded word refused (%s)' % error, 'conflict' in str(error))
    else:
        check('a join of a descriptor and a loaded word refused', False)


# ---- v_cvt_pknorm_{u16,i16}_f32 ---------------------------------------------------------------------------
def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def roundeven(x):
    # Python's round() on a float is round-half-to-even, like _mesa_roundevenf and HLSL round()
    return float(round(x))


def ref_pknorm(x, signed_form):
    """Mesa main 13ebbc00: nir_constant_expressions.py pack_[us]norm_1x16 with util/macros.h CLAMP.

    (uint16_t)(int)_mesa_roundevenf(CLAMP(x, lo, 1.0f) * K), with CLAMP(X, MIN, MAX) = X > MIN ? (X > MAX ? MAX : X)
    : MIN, so NaN takes MIN; the product is a binary32 multiply.
    """
    lo, scale = (-1.0, 32767.0) if signed_form else (0.0, 65535.0)
    clamped = (1.0 if x > 1.0 else x) if x > lo else lo
    return int(roundeven(f32(clamped * scale))) & 0xffff


def pknorm_helper(name):
    """The HLSL of x2w_pknorm_[ui]16 as a Python function over a binary32 value (the bits come in as a float)."""
    text = G.HELPERS[name]
    body = text[text.index('{') + 1:text.rindex('}')]
    body = re.sub(r'\b(0x[0-9a-f]+|\d+)u\b', r'\1', body)
    body = re.sub(r'round\((.*) \* ([\d.]+)\)', r'rne(f32(\1 * \2))', body)
    source = 'def helper(x):\n'
    for statement in filter(None, (s.strip() for s in body.split(';'))):
        m = re.fullmatch(r'if \((.*)\) (return .*)', statement)
        source += ('    if %s:\n        %s\n' % m.groups()) if m else '    %s\n' % statement
    space = dict(asuint=lambda v: struct.unpack('<I', struct.pack('<f', v))[0], f32=f32, rne=roundeven,
                 clamp=lambda v, lo, hi: min(max(v, lo), hi), uint=lambda v: int(v) & MASK, int=int)
    exec(source, space)
    return space['helper']


def pknorm():
    def bits(b):
        return struct.unpack('<f', struct.pack('<I', b))[0]
    nans = [float('nan'), -float('nan'), bits(0x7f800001), bits(0xffc00000)]
    values = nans + [0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 1e30, -1e30, float('inf'), float('-inf'), 0.5, -0.5, 0.25,
                     bits(1), bits(0x80000001), f32(1.5 / 65535), f32(2.5 / 65535), f32(-1.5 / 32767), f32(0.5 / 32767),
                     f32(1 - 2 ** -24), f32(-1 + 2 ** -24), 0.1, -0.1, 0.999, -0.999]
    values += [f32(i / 1000.0 - 1.2) for i in range(2401)]
    for name, signed_form in (('x2w_pknorm_u16', False), ('x2w_pknorm_i16', True)):
        helper = pknorm_helper(name)
        bad = [(x, helper(x), ref_pknorm(x, signed_form)) for x in values if helper(x) != ref_pknorm(x, signed_form)]
        check('%s matches Mesa pack_%snorm_1x16 on %d inputs (NaN, +-0, ties, out of range)%s'
              % (name, 's' if signed_form else 'u', len(values), (' first %r' % (bad[0],)) if bad else ''), not bad)
    check('NaN packs as Mesa clamps it: unorm 0, snorm -32767',
          pknorm_helper('x2w_pknorm_u16')(nans[0]) == 0 and pknorm_helper('x2w_pknorm_i16')(nans[0]) == 0x8001)
    check('ties round to even: unorm 0.5 -> 32768, snorm 0.5 -> 16384',
          pknorm_helper('x2w_pknorm_u16')(0.5) == 32768 and pknorm_helper('x2w_pknorm_i16')(0.5) == 16384)
    vop3 = lambda op, extra=0, w1extra=0: [0xd4000000 | op << 16 | extra | 7, (256 + 7) | (256 + 4) << 9 | w1extra]
    w = vop3(0x369)
    _, text = lines(w)
    check('v_cvt_pknorm_u16_f32: S0 in the low half, S1 in the high half',
          asm_of(w) == 'v_cvt_pknorm_u16_f32 v7, v7, v4' and
          text == 'v7 = x2w_pknorm_u16(F(v7)) | (x2w_pknorm_u16(F(v4)) << 16);')
    w = vop3(0x368, w1extra=1 << 29)
    _, text = lines(w)
    check('v_cvt_pknorm_i16_f32 applies the source neg before packing',
          asm_of(w) == 'v_cvt_pknorm_i16_f32 v7, -v7, v4' and
          text == 'v7 = x2w_pknorm_i16((-F(v7))) | (x2w_pknorm_i16(F(v4)) << 16);')
    check('v_cvt_pknorm_u16_f32 clamp refused', refused(None, words=vop3(0x369, 1 << 15)))
    check('v_cvt_pknorm_u16_f32 omod refused', refused(None, words=vop3(0x369, 0, 1 << 27)))


# ---- scalar ALU: shift-add, abs, SOPK arithmetic, pack, and the SCC liveness check -------------------------
SOP2 = lambda op, d, a, b: 0x80000000 | op << 23 | d << 16 | b << 8 | a
LSHL2_ADD = SOP2(0x2f, 20, 21, 22)            # s_lshl2_add_u32 s20, s21, s22
CSELECT = SOP2(0x0a, 23, 20, 21)              # s_cselect_b32 s23, s20, s21 (reads SCC)
CMP_EQ = 0xbf000000 | 6 << 16 | 21 << 8 | 20  # s_cmp_eq_u32 s20, s21 (writes SCC)
NOP = 0xbf800000
SOPP = lambda op, n: 0xbf800000 | op << 16 | (n & 0xffff)  # s_branch 2, s_cbranch_scc1 5, vccz 6, vccnz 7


class SignedExpr(Expr):
    """Expr plus the signed max of s_abs_i32's lowering."""
    @staticmethod
    def call(name, args):
        if name == 'max':
            (a, sa), (b, sb) = args
            assert sa and sb
            return max(G_signed(a), G_signed(b)) & MASK, True
        return Expr.call(name, args)


def G_signed(v):
    return v - (1 << 32) if v >> 31 else v


def translates(words):
    try:
        stage(words)
    except G.Unsupported as error:
        return str(error)
    return None


def scalar_alu():
    grid = [0, 1, 2, 0x7fff, 0x8000, 0xffff, 0x10000, 0x12345678, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, MASK]
    _, text = lines([LSHL2_ADD])
    check('s_lshl2_add_u32 is (S0 << 2) + S1', asm_of([LSHL2_ADD]) == 's_lshl2_add_u32 s20, s21, s22' and
          text == 's20 = (s21 << 2) + s22;')
    e = Expr(text.split(' = ', 1)[1].rstrip(';'))
    check('s_lshl2_add_u32 wraps modulo 2^32 on the grid',
          all(e.run({'s21': a, 's22': b})[0] == ((a << 2) + b) & MASK for a in grid for b in grid))
    for n, op in ((1, 0x2e), (3, 0x30), (4, 0x31)):
        _, text = lines([SOP2(op, 20, 21, 22)])
        check('s_lshl%d_add_u32 shifts by %d' % (n, n), text == 's20 = (s21 << %d) + s22;' % n)
    w = 0xbe800000 | 20 << 16 | 0x34 << 8 | 21
    _, text = lines([w])
    e = SignedExpr(text.split(' = ', 1)[1].rstrip(';'))
    check('s_abs_i32 is smax(x, -x): INT_MIN stays INT_MIN (LLVM selects it for that pattern)',
          asm_of([w]) == 's_abs_i32 s20, s21' and
          all(e.run({'s21': a})[0] == (abs(G_signed(a)) & MASK) for a in grid) and
          e.run({'s21': 0x80000000})[0] == 0x80000000)
    w = SOP2(0x32, 20, 21, 22)
    _, text = lines([w])
    e = Expr(text.split(' = ', 1)[1].rstrip(';'))
    check('s_pack_ll_b32_b16 puts S0[15:0] low and S1[15:0] high',
          asm_of([w]) == 's_pack_ll_b32_b16 s20, s21, s22' and
          all(e.run({'s21': a, 's22': b})[0] == (a & 0xffff) | (b & 0xffff) << 16 for a in grid for b in grid))
    for op, name, sym in ((0xf, 's_addk_i32', '+'), (0x10, 's_mulk_i32', '*')):
        w = 0xb0000000 | op << 23 | 20 << 16 | 0xfff0
        _, text = lines([w])
        check('%s sign-extends its simm16 (0xfff0 is -16)' % name,
              asm_of([w]) == '%s s20, 0xfff0' % name and text == 's20 = s20 %s 0xfffffff0u;' % sym)
        w = 0xb0000000 | op << 23 | 20 << 16 | 0x150
        _, text = lines([w])
        check('%s with a positive simm16' % name, text == 's20 = s20 %s 0x00000150u;' % sym)
    # SCC: refused only when an unmodelled SCC value can reach a reader
    addk = 0xb0000000 | 0xf << 23 | 20 << 16 | 4
    mulk = 0xb0000000 | 0x10 << 23 | 20 << 16 | 4
    cases = [
        ('unmodelled SCC read straight after', [LSHL2_ADD, CSELECT], True),
        ('SCC redefined by s_cmp before the read', [LSHL2_ADD, CMP_EQ, CSELECT], False),
        ('no reader at all', [LSHL2_ADD, NOP], False),
        ('s_cbranch_scc1 reads it', [LSHL2_ADD, SOPP(5, 0), NOP], True),
        ('a branch around the redefinition reaches the reader',
         [LSHL2_ADD, SOPP(6, 1), CMP_EQ, CSELECT], True),
        ('both paths redefine it', [LSHL2_ADD, SOPP(6, 2), CMP_EQ, SOPP(2, 1), CMP_EQ, CSELECT], False),
        ('a loop carries it back to a reader at the head', [CSELECT, LSHL2_ADD, SOPP(7, -3)], True),
        ('a loop that redefines it at the head', [CMP_EQ, CSELECT, LSHL2_ADD, SOPP(7, -4)], False),
        ('s_addk_i32 defines SCC (signed overflow, not modelled)', [addk, CSELECT], True),
        ('s_mulk_i32 does not define SCC', [CMP_EQ, mulk, CSELECT], False),
        ('src_scc as an operand is a reader', [LSHL2_ADD, SOP2(0x00, 23, 0xfd, 21)], True),
    ]
    for label, words, want_refused in cases:
        reason = translates(words)
        check('SCC liveness: %s -> %s%s' % (label, 'refused' if want_refused else 'translated',
                                            (' (%s)' % reason) if reason else ''),
              (reason is not None and 'SCC of' in reason) if want_refused else reason is None)


# ---- v_fma_mix_f32 and the f16 operands of the mix family -------------------------------------------------
def mix_word(op, vdst, srcs, op_sel=0, op_sel_hi=0, neg=0, absolute=0, clamp=0):
    return [0xcc000000 | op << 16 | (op_sel_hi >> 2 & 1) << 14 | op_sel << 11 | absolute << 8 | clamp << 15 | vdst,
            srcs[0] | srcs[1] << 9 | srcs[2] << 18 | (op_sel_hi & 3) << 27 | neg << 29]


def mixes():
    # op_sel / op_sel_hi as [src0, src1, src2] bit 0, 1, 2
    cases = [
        (mix_word(0x20, 1, (258, 20, 128), op_sel=2, op_sel_hi=2, neg=4),
         'v_fma_mix_f32 v1, v2, s20, -0 op_sel:[0,1,0] op_sel_hi:[0,1,0]',
         'v1 = U(F(v2) * f16tof32(s20 >> 16) + (-0.0));',
         'f32 result; op_sel_hi makes src1 an f16 and op_sel takes its high half; -0 is +0 negated'),
        (mix_word(0x20, 1, (242, 32, 268), op_sel_hi=2, neg=6),
         'v_fma_mix_f32 v1, 1.0, -s32, -v12 op_sel_hi:[0,1,0]',
         'v1 = U(1.0 * (-f16tof32(s32)) + (-F(v12)));',
         'neg applies to the converted f16 and to the f32 operand'),
        (mix_word(0x20, 1, (258, 259, 260), op_sel_hi=1, absolute=1, clamp=1),
         'v_fma_mix_f32 v1, |v2|, v3, v4 op_sel_hi:[1,0,0] clamp',
         'v1 = U(saturate(abs(f16tof32(v2)) * F(v3) + F(v4)));',
         'abs on a low-half f16 source, clamp on the f32 result'),
        (mix_word(0x21, 1, (242, 258, 240), op_sel_hi=4),
         'v_fma_mixlo_f16 v1, 1.0, v2, 0.5 op_sel_hi:[0,0,1]',
         'v1 = (v1 & 0xffff0000u) | f32tof16(1.0 * F(v2) + f16tof32(0x3800u));',
         'an inline 0.5 read as f16 is 0x3800 (not the low half of the f32 0x3f000000)'),
    ]
    for words, want_asm, want_text, label in cases:
        got_asm = asm_of(words)
        _, text = lines(words) if got_asm == want_asm else (None, None)
        check('fma_mix: %s' % label, got_asm == want_asm and text == want_text)
    check('fma_mix: op_sel on an f32 operand refused',
          refused(None, words=mix_word(0x20, 1, (258, 259, 260), op_sel=1)))
    check('fma_mix: high half of an inline f16 constant refused',
          refused(None, words=mix_word(0x20, 1, (240, 259, 260), op_sel=1, op_sel_hi=1)))
    check('fma_mix: literal f16 operand refused',
          refused(None, words=mix_word(0x20, 1, (255, 259, 260), op_sel_hi=1) + [0x3c00]))
    check('fma_mix: inline 1/(2*pi) refused', refused(None, words=mix_word(0x20, 1, (248, 259, 260))))


# ---- SDWA select and compare, null-export classification, a16 image addresses, entry VGPR reads -------------
def sdwa_and_misc():
    w = [0x1 << 25 | 10 << 17 | 10 << 9 | 0xf9, 10 | 6 << 8 | 4 << 16 | 5 << 24]
    _, text = lines(w)
    check('v_cndmask_b32_sdwa selects between the source fields by vcc',
          asm_of(w) == 'v_cndmask_b32_sdwa v10, v10, v10, vcc_lo dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_0 '
                       'src1_sel:WORD_1' and
          text == 'v10 = (vcc & 1) ? ((v10 >> 16u) & 0xffffu) : ((v10 >> 0u) & 0xffffu);')
    w = [0x7c000000 | 0x95 << 17 | 15 << 9 | 0xf9, 20 | 6 << 16 | 1 << 23 | 4 << 24]
    _, text = lines(w)
    check('v_cmpx_ne_i32_sdwa compares the selected fields and writes exec',
          asm_of(w) == 'v_cmpx_ne_i32_sdwa s20, v15 src0_sel:DWORD src1_sel:WORD_0' and
          text == 'exec = ((int(s20) != int(((v15 >> 0u) & 0xffffu))) ? 1u : 0u);')
    w = [0x7c000000 | 0x85 << 17 | 15 << 9 | 0xf9, 20 | 6 << 16 | 1 << 23 | 4 << 24 | 1 << 27]
    _, text = lines(w)
    check('v_cmp_ne_i32_sdwa with sext() sign-extends the field into vcc',
          asm_of(w) == 'v_cmp_ne_i32_sdwa vcc_lo, s20, sext(v15) src0_sel:DWORD src1_sel:WORD_0' and
          text == 'vcc = ((int(s20) != int(asuint(asint(v15 << 16u) >> 16u))) ? 1u : 0u);')
    check('v_cmp_lt_f32_sdwa (a float SDWA compare) refused',
          refused(None, words=[0x7c000000 | 0x01 << 17 | 15 << 9 | 0xf9, 20 | 6 << 16 | 1 << 23 | 4 << 24]))
    null = words_bytes([0xf8000000 | 9 << 4 | 1 << 11 | 1 << 12, 0, 0xbf810000])
    check('a stage whose only export is "exp null ... done vm" is a pixel stage', G.classify(null) == 'ps')

    load = [0xf0000000 | 3 << 8 | 1 << 3, 2 | 8 << 8 | (24 // 4) << 16 | 1 << 30]
    _, text = lines(load, setup=gather_setup)
    check('image_load a16 takes x and y from the halves of one address dword',
          asm_of(load).endswith('dim:SQ_RSRC_IMG_2D a16') and
          text == 't = r0.Load(int3((v2 & 0xffffu), (v2 >> 16), 0));\nv8 = U(t.x);\nv9 = U(t.y);')
    sample = [0xf0000000 | 0x20 << 18 | 3 << 8 | 1 << 3, 2 | 8 << 8 | (24 // 4) << 16 | (16 // 4) << 21 | 1 << 30]
    _, text = lines(sample, setup=gather_setup)
    check('image_sample a16 takes binary16 u and v from the halves of one address dword',
          'f16tof32(v2), f16tof32(v2 >> 16)' in text and 'v3' not in text)
    load1d = [0xf0000000 | 1 << 8, 2 | 8 << 8 | (24 // 4) << 16]
    s, text = lines(load1d, setup=gather_setup)
    check('image_load on a 1D image is Texture1D.Load(int2(x, 0)) with manifest kind tex1d',
          asm_of(load1d).endswith('dmask:0x1 dim:SQ_RSRC_IMG_1D') and
          text == 't = r0.Load(int2(v2, 0));\nv8 = U(t.x);' and list(s.pipe.resources.values())[0]['kind'] == 'tex1d')
    check('image_sample d16 refused', refused(None, setup=gather_setup, words=[sample[0], sample[1] | 1 << 31]))
    check('image_sample unorm refused', refused(None, setup=gather_setup, words=[sample[0] | 1 << 12, sample[1]]))
    check('image_load tfe refused', refused(None, setup=gather_setup, words=[load[0] | 1 << 16, load[1]]))

    mov = 0x7e000000 | 3 << 17 | 1 << 9 | (256 + 1)          # v_mov_b32 v3, v1
    s = stage([0x7e000000 | 1 << 17 | 1 << 9 | 0x80, mov])  # v_mov_b32 v1, 0 first
    t = stage([mov])
    check('a pixel read of a VGPR before any write is recorded as an entry (SPI input) read',
          s.entry_reads == {} and t.entry_reads == {1: 0})
    # A whole request: the interpolated v0..v11 then four moves into the exported v12..v15. Reading v20, which no
    # instruction writes, is a read of an SPI input: translate_to writes a .fail instead of a result.
    import tempfile
    import startup_gfx2hlsl_bit_alu_check as B
    def moves(sources):
        return words_bytes([0x7e000000 | (12 + i) << 17 | 1 << 9 | (256 + r) for i, r in enumerate(sources)])
    vs = B.vs_words([(0, 0, 0)] * 4)
    results = {}
    with tempfile.TemporaryDirectory() as directory:
        for label, sources in (('control', (0, 1, 2, 3)), ('entry', (0, 1, 20, 3))):
            ps = B.PRE_WORDS + moves(sources) + B.POST_WORDS
            outcome = G.translate_to(directory, 1, vs, ps, B.root_blob(), check=False)
            results[label] = (outcome, Path(directory, '%016x.txt' % 1).exists())
            for leftover in Path(directory).iterdir():
                leftover.unlink()
    check('translate_to refuses a pixel stage that reads an entry VGPR (%s); its control translates'
          % results['entry'][0],
          results['control'] == ('ok', True) and results['entry'][1] is False and 'entry VGPR v20' in results['entry'][0])


def main():
    exports()
    output_modifiers()
    selects_and_halves()
    integer_helpers()
    classes()
    cubes()
    gather()
    typed_fetch()
    joins()
    pknorm()
    scalar_alu()
    mixes()
    sdwa_and_misc()
    if failures:
        raise SystemExit('FAILED: %s' % '; '.join(failures))
    print('startup gfx2hlsl coverage checks: 0 failed')


if __name__ == '__main__':
    main()
