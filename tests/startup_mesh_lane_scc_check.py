"""Check the lane-mask SCC lowering of tools/startup/mesh2hlsl.py (group-uniform execz range + wave "any lane" SCC).

usage: startup_mesh_lane_scc_check.py [--write <dir>]

A hand-assembled gfx1013 NGG program (no title bytes; encoded below field by field, opcode numbers found by
decoding with tools/disasm_rdna2.py and the decoded text compared with the intended text) does, per group g:

    N = 40 + 60 * (g & 1)          vertices of the per-lane range (40: wave 1 has NO active lane in it)
    T = 20 * (g & 3)               lanes with v0 < T set vcc
    s40 = 64
    exec = v0 < N; s_cbranch_execz after      (group-uniform range: s40 is written inside and read after)
      vcc = v0 < T
      s_and_b64 s[12:13], vcc, exec           SCC = (vcc & exec) != 0 over the WHOLE wave
      s_cselect_b32 s40, 1, 33                s40 = SCC ? 1 : 33
      s_cbranch_scc0 skip                     branch on that SCC (the class refused before)
      s_add_u32 s40, s40, 100
    skip/after: exec = -1; every thread exports param0 = (s40, v0, s40, v0)

Hardware meaning (AMD RDNA 2 ISA, S_AND_B64: D = S0 & S1, SCC = D != 0; S_CBRANCH_SCC0 reads that one wave-wide
bit; S_CBRANCH_EXECZ skips the range for a wave whose exec is 0, so its s40 stays 7). Expected s40 per wave:
    no lane of the wave in the range -> 64; some lane with v0 < T -> 101; else -> 33.
(Values at least 31 apart: difftest.py tolerates small differences in output words as float ulp noise.)
The translation runs one HLSL thread per lane: SCC must be the wave's value (published through groupshared
memory and a group barrier), and a wave with no active lane must get s40 = 64 back at the range end.

Checks (pure Python; the differential run on WARP is mesh_work/difftest.py with --write's program):
  1. the program decodes back to the intended text;
  2. the translation succeeds and contains the publish, the wave read and the restore;
  3. negative controls: X2W_MESH_LANE_SCC=0 refuses the program again ("branch on an SCC that depends on lane
     masks"); X2W_MESH_TEST_DROP=publish / restore remove the publish / restore statements (the WARP difftest
     must then FAIL; see the --write output).
Program B (active-uniform SCC in a per-lane range, no publish) and program C (a lane-dependent SCC inside a
per-lane execz range, made group-uniform for the publish, with a vccz branch and an s_branch over the other side:
the taint at the join is merged; X2W_MESH_JOIN_TAINT=0, the linear scan, translates it WITHOUT the publish, which
the WARP difftest shows wrong) are described at their definitions.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'startup'))
import disasm_rdna2 as D  # noqa: E402

failures = []


def check(label, condition, detail=''):
    print(f"{label}: {'ok' if condition else 'MISMATCH  ' + detail}")
    if not condition:
        failures.append(label)


# ---- a minimal gfx10 encoder: field layout per format, opcode found by decoding -----------------------------
def src(tok):
    """9-bit source operand code (VOP); SALU uses the low 8 bits of the same table."""
    named = {'vcc_lo': 106, 'vcc_hi': 107, 'm0': 124, 'exec_lo': 126, 'exec_hi': 127, 'src_scc': 253}
    if tok in named:
        return named[tok], None
    if tok[0] == 's':
        return int(tok[1:]), None
    if tok[0] == 'v':
        return 256 + int(tok[1:]), None
    v = int(tok, 0)
    if 0 <= v <= 64:
        return 128 + v, None
    if -16 <= v < 0:
        return 192 - v, None
    return 255, v & 0xffffffff


def fmt_words(fmt, op, f):
    if fmt == 'sop2':
        return [(0b10 << 30) | (op << 23) | (f['d'] << 16) | (f['s1'] << 8) | f['s0']]
    if fmt == 'sop1':
        return [(0x17d << 23) | (f['d'] << 16) | (op << 8) | f['s0']]
    if fmt == 'sopc':
        return [(0x17e << 23) | (op << 16) | (f['s1'] << 8) | f['s0']]
    if fmt == 'sopp':
        return [(0x17f << 23) | (op << 16) | (f['imm'] & 0xffff)]
    if fmt == 'vop1':
        return [(0x3f << 25) | (f['d'] << 17) | (op << 9) | f['s0']]
    if fmt == 'vopc':
        return [(0x3e << 25) | (op << 17) | ((f['s1'] - 256) << 9) | f['s0']]
    if fmt == 'vop3':
        return [(0x35 << 26) | (op << 16) | f['d'], f['s0'] | (f['s1'] << 9) | (f.get('s2', 0) << 18)]
    raise ValueError(fmt)


OP_RANGE = {'sop2': 128, 'sop1': 256, 'sopc': 128, 'sopp': 128, 'vop1': 256, 'vopc': 256, 'vop3': 1024}
_op_cache = {}


def find_op(fmt, mnemonic, fields):
    key = (fmt, mnemonic)
    if key in _op_cache:
        return _op_cache[key]
    for op in range(OP_RANGE[fmt]):
        words = fmt_words(fmt, op, fields)
        code = struct.pack('<%dI' % len(words), *words) + b'\0' * 8
        try:
            _, name, _, _ = D.decode_instruction(code, 0)
        except Exception:
            continue
        if name == mnemonic:
            _op_cache[key] = op
            return op
    raise KeyError('%s %s: no opcode decodes to it' % (fmt, mnemonic))


def ins(fmt, mnemonic, **ops):
    """One instruction: (words, literal or None). Operand values are register/constant spellings."""
    fields, literal = {}, None
    for k, tok in ops.items():
        if k in ('d', 'imm'):
            fields[k] = tok if isinstance(tok, int) else src(tok)[0]
            continue
        code, lit = src(tok)
        fields[k] = code
        if lit is not None:
            literal = lit
    words = fmt_words(fmt, find_op(fmt, mnemonic, fields), fields)
    return words + ([literal] if literal is not None else [])


def exp(target, vals, done=False):
    en = sum(1 << i for i, v in enumerate(vals) if v is not None)
    w0 = (0x3e << 26) | (int(done) << 11) | (target << 4) | en
    w1 = 0
    for i, v in enumerate(vals):
        if v is not None:
            w1 |= int(v[1:]) << (8 * i)
    return [w0, w1]


PROLOGUE = [
    ('s_mov_b64 exec, -1', lambda: ins('sop1', 's_mov_b64', d=126, s0='-1')),
    ('v_readfirstlane_b32 s20, v5', lambda: ins('vop1', 'v_readfirstlane_b32', d=20, s0='v5')),
    ('s_lshr_b32 s20, s20, 7', lambda: ins('sop2', 's_lshr_b32', d=20, s0='s20', s1='7')),
    ('s_bfe_u32 s28, s3, 0x40018', lambda: ins('sop2', 's_bfe_u32', d=28, s0='s3', s1='0x40018')),
    ('s_lshl_b32 s29, s28, 6', lambda: ins('sop2', 's_lshl_b32', d=29, s0='s28', s1='6')),
    ('v_mbcnt_lo_u32_b32 v0, -1, s29', lambda: ins('vop3', 'v_mbcnt_lo_u32_b32', d=0, s0='-1', s1='s29')),
    ('v_mbcnt_hi_u32_b32 v0, -1, v0', lambda: ins('vop3', 'v_mbcnt_hi_u32_b32', d=0, s0='-1', s1='v0')),
    ('s_and_b32 s21, s20, 1', lambda: ins('sop2', 's_and_b32', d=21, s0='s20', s1='1')),
    ('s_mul_i32 s21, s21, 60', lambda: ins('sop2', 's_mul_i32', d=21, s0='s21', s1='60')),
    ('s_add_u32 s21, s21, 40', lambda: ins('sop2', 's_add_u32', d=21, s0='s21', s1='40')),
    ('s_and_b32 s22, s20, 3', lambda: ins('sop2', 's_and_b32', d=22, s0='s20', s1='3')),
    ('s_mul_i32 s22, s22, 20', lambda: ins('sop2', 's_mul_i32', d=22, s0='s22', s1='20')),
]
EPILOGUE = [
    ('AFTER:', None),
    ('s_mov_b64 exec, -1', lambda: ins('sop1', 's_mov_b64', d=126, s0='-1')),
    ('s_mov_b32 m0, 0x80080', lambda: ins('sop1', 's_mov_b32', d=124, s0='0x80080')),
    ('s_sendmsg 0x9', lambda: ins('sopp', 's_sendmsg', imm=9)),
    ('v_cvt_f32_u32_e32 v2, v0', lambda: ins('vop1', 'v_cvt_f32_u32_e32', d=2, s0='v0')),
    ('v_mov_b32_e32 v3, 0', lambda: ins('vop1', 'v_mov_b32_e32', d=3, s0='0')),
    ('exp pos0 v2, v2, v2, v2 done', lambda: exp(12, ['v2', 'v2', 'v2', 'v2'], True)),
    ('exp param0 v1, v0, v1, v0', lambda: exp(32, ['v1', 'v0', 'v1', 'v0'])),
    ('exp target_20 v3, off, off, off done', lambda: exp(20, ['v3', None, None, None], True)),
    ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
]
# Program B: the SCC is the same in every active lane (vcc from a compare of uniform values, masked with exec)
# and the branch sits right inside a per-lane execz range that writes no scalar read after it, so every thread
# that runs there is an active lane: the per-lane branch is the wave's (no publish). Expected param0.x per lane:
# lanes v0 < N: 60 when T > 30 (groups 2, 3), else 5; lanes v0 >= N: 5.
UNIFORM_B = PROLOGUE + [
    ('v_mov_b32_e32 v1, 5', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='5')),
    ('v_mov_b32_e32 v4, s22', lambda: ins('vop1', 'v_mov_b32_e32', d=4, s0='s22')),
    ('v_cmpx_gt_u32_e32 s21, v0', lambda: ins('vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0')),
    ('s_cbranch_execz AFTER', 'execz'),
    ('v_cmp_lt_u32_e32 vcc_lo, 30, v4', lambda: ins('vopc', 'v_cmp_lt_u32_e32', s0='30', s1='v4')),
    ('s_and_b64 s[12:13], vcc, exec', lambda: ins('sop2', 's_and_b64', d=12, s0='vcc_lo', s1='exec_lo')),
    ('s_cbranch_scc0 AFTER', 'scc0'),
    ('v_mov_b32_e32 v1, 60', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='60')),
] + EPILOGUE
# Program C: a lane-dependent SCC inside a per-lane execz range that writes no scalar read after it (a plain
# per-lane range before), then a per-lane vccz branch with an s_branch over the other side, both sides copying
# SCC to s20 (the shape of the run's programs). The publish needs every thread of the group at its barrier, so
# the range becomes group-uniform; the s_lshl on the first side leaves a lane-tainted SCC that the linear scan
# would carry into the other side (the join merge takes the state of the vccz branch instead).
# N = 40 + 60 * (g & 1), T = 30 * (g & 3). Expected param0.x: lanes v0 < N of a wave with a lane v0 < min(N, T):
# 60; every other lane 5. (g = 2: wave 1 has no lane in the range; g = 3: wave 1 lanes 64..99 get 60.)
# Program D: inside a per-lane execz range, an SCC from a saved exec copy and NOT exec (`s_andn2_b64 s, saved,
# exec`): the active lanes' own bits are all 0, the wave's SCC is 1 when the wave has a lane outside the range. Not
# an active-uniform SCC: it must be published. Expected param0.x: lanes v0 < N of a wave with a lane v0 >= N: 60;
# every other lane 5 (g even: lanes 0..39; g odd: lanes 64..99).
ANDN2_D = PROLOGUE + [
    ('v_mov_b32_e32 v1, 5', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='5')),
    ('s_mov_b64 s[8:9], exec', lambda: ins('sop1', 's_mov_b64', d=8, s0='exec_lo')),
    ('v_cmpx_gt_u32_e32 s21, v0', lambda: ins('vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0')),
    ('s_cbranch_execz AFTER', 'execz'),
    ('s_andn2_b64 s[12:13], s[8:9], exec', lambda: ins('sop2', 's_andn2_b64', d=12, s0='s8', s1='exec_lo')),
    ('s_cbranch_scc0 AFTER', 'scc0'),
    ('v_mov_b32_e32 v1, 60', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='60')),
] + EPILOGUE
PROLOGUE_C = PROLOGUE[:-1] + [
    ('s_mul_i32 s22, s22, 30', lambda: ins('sop2', 's_mul_i32', d=22, s0='s22', s1='30')),
]
SPLIT_C = PROLOGUE_C + [
    ('v_mov_b32_e32 v1, 5', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='5')),
    ('v_mov_b32_e32 v4, s22', lambda: ins('vop1', 'v_mov_b32_e32', d=4, s0='s22')),
    ('v_cmpx_gt_u32_e32 s21, v0', lambda: ins('vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0')),
    ('s_cbranch_execz AFTER', 'execz'),
    ('v_cmp_gt_u32_e32 vcc_lo, s22, v0', lambda: ins('vopc', 'v_cmp_gt_u32_e32', s0='s22', s1='v0')),
    ('s_and_b64 s[12:13], vcc, exec', lambda: ins('sop2', 's_and_b64', d=12, s0='vcc_lo', s1='exec_lo')),
    ('v_cmp_lt_u32_e32 vcc_lo, 30, v4', lambda: ins('vopc', 'v_cmp_lt_u32_e32', s0='30', s1='v4')),
    ('s_cbranch_vccz ELSE', 'vccz'),
    ('s_mov_b32 s20, src_scc', lambda: ins('sop1', 's_mov_b32', d=20, s0='src_scc')),
    ('s_lshl_b32 s23, vcc_lo, 2', lambda: ins('sop2', 's_lshl_b32', d=23, s0='vcc_lo', s1='2')),
    ('s_branch JOIN', 'branch'),
    ('ELSE:', None),
    ('s_mov_b32 s20, src_scc', lambda: ins('sop1', 's_mov_b32', d=20, s0='src_scc')),
    ('JOIN:', None),
    ('s_cmp_lg_i32 s20, 0', lambda: ins('sopc', 's_cmp_lg_i32', s0='s20', s1='0')),
    ('s_cbranch_scc0 AFTER', 'scc0'),
    ('v_mov_b32_e32 v1, 60', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='60')),
] + EPILOGUE


def program(items=None):
    """(code bytes, intended text per instruction). Branch targets are resolved in a second pass."""
    if items is not None:
        return assemble(items)
    items = [
        ('s_mov_b64 exec, -1', lambda: ins('sop1', 's_mov_b64', d=126, s0='-1')),
        ('v_readfirstlane_b32 s20, v5', lambda: ins('vop1', 'v_readfirstlane_b32', d=20, s0='v5')),
        ('s_lshr_b32 s20, s20, 7', lambda: ins('sop2', 's_lshr_b32', d=20, s0='s20', s1='7')),
        ('s_bfe_u32 s28, s3, 0x40018', lambda: ins('sop2', 's_bfe_u32', d=28, s0='s3', s1='0x40018')),
        ('s_lshl_b32 s29, s28, 6', lambda: ins('sop2', 's_lshl_b32', d=29, s0='s28', s1='6')),
        ('v_mbcnt_lo_u32_b32 v0, -1, s29', lambda: ins('vop3', 'v_mbcnt_lo_u32_b32', d=0, s0='-1', s1='s29')),
        ('v_mbcnt_hi_u32_b32 v0, -1, v0', lambda: ins('vop3', 'v_mbcnt_hi_u32_b32', d=0, s0='-1', s1='v0')),
        ('s_and_b32 s21, s20, 1', lambda: ins('sop2', 's_and_b32', d=21, s0='s20', s1='1')),
        ('s_mul_i32 s21, s21, 60', lambda: ins('sop2', 's_mul_i32', d=21, s0='s21', s1='60')),
        ('s_add_u32 s21, s21, 40', lambda: ins('sop2', 's_add_u32', d=21, s0='s21', s1='40')),
        ('s_and_b32 s22, s20, 3', lambda: ins('sop2', 's_and_b32', d=22, s0='s20', s1='3')),
        ('s_mul_i32 s22, s22, 20', lambda: ins('sop2', 's_mul_i32', d=22, s0='s22', s1='20')),
        ('s_mov_b32 s40, 64', lambda: ins('sop1', 's_mov_b32', d=40, s0='64')),
        ('v_cmpx_gt_u32_e32 s21, v0', lambda: ins('vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0')),
        ('s_cbranch_execz AFTER', 'execz'),
        ('v_cmp_gt_u32_e32 vcc_lo, s22, v0', lambda: ins('vopc', 'v_cmp_gt_u32_e32', s0='s22', s1='v0')),
        ('s_and_b64 s[12:13], vcc, exec', lambda: ins('sop2', 's_and_b64', d=12, s0='vcc_lo', s1='exec_lo')),
        ('s_cselect_b32 s40, 1, 33', lambda: ins('sop2', 's_cselect_b32', d=40, s0='1', s1='33')),
        ('s_cbranch_scc0 AFTER', 'scc0'),
        ('s_add_u32 s40, s40, 0x64', lambda: ins('sop2', 's_add_u32', d=40, s0='s40', s1='100')),
        ('AFTER:', None),
        ('s_mov_b64 exec, -1', lambda: ins('sop1', 's_mov_b64', d=126, s0='-1')),
        ('s_mov_b32 m0, 0x80080', lambda: ins('sop1', 's_mov_b32', d=124, s0='0x80080')),
        ('s_sendmsg 0x9', lambda: ins('sopp', 's_sendmsg', imm=9)),
        ('v_mov_b32_e32 v1, s40', lambda: ins('vop1', 'v_mov_b32_e32', d=1, s0='s40')),
        ('v_cvt_f32_u32_e32 v2, v0', lambda: ins('vop1', 'v_cvt_f32_u32_e32', d=2, s0='v0')),
        ('v_mov_b32_e32 v3, 0', lambda: ins('vop1', 'v_mov_b32_e32', d=3, s0='0')),
        ('exp pos0 v2, v2, v2, v2 done', lambda: exp(12, ['v2', 'v2', 'v2', 'v2'], True)),
        ('exp param0 v1, v0, v1, v0', lambda: exp(32, ['v1', 'v0', 'v1', 'v0'])),
        ('exp target_20 v3, off, off, off done', lambda: exp(20, ['v3', None, None, None], True)),
        ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
    ]
    return assemble(items)


def assemble(items):
    # pass 1: sizes and label offsets
    off, labels, sized = 0, {}, []
    for text, make in items:
        if make is None:
            labels[text[:-1]] = off
            continue
        words = make() if callable(make) else [0]
        sized.append((off, text, make, len(words)))
        off += 4 * len(words)
    out, texts = [], []
    for off, text, make, n in sized:
        if callable(make):
            words = make()
        else:
            target = labels[text.split()[1]]
            words = ins('sopp', 's_branch' if make == 'branch' else 's_cbranch_' + make, imm=(target - off - 4) // 4)
            text = '%s label_%#x' % (text.split()[0], target)
        out += words
        texts.append((off, text))
    return struct.pack('<%dI' % len(out), *out), texts


def decoded(code):
    off, res = 0, []
    while off + 4 <= len(code):
        n, name, asm, _ = D.decode_instruction(code, off)
        res.append((off, asm))
        off += n
    return res


def translate(env):
    """Run the translator in a child process (its switches are read at import): HLSL or the refusal text."""
    child = ('import sys; sys.path[:0] = [%r, %r]\n'
             'import gfx2hlsl as G, mesh2hlsl as M\n'
             'code = open(sys.argv[1], "rb").read()\n'
             'try:\n'
             '    p = M.MeshPipeline(code, None, M.synth_root(code), user_sgpr=16)\n'
             '    sys.stdout.write("OK\\n" + p.hlsl_ms())\n'
             'except G.Unsupported as e:\n'
             '    sys.stdout.write("REFUSED %%s\\n" %% e)\n'
             % (os.path.join(HERE, '..', 'tools'), os.path.join(HERE, '..', 'tools', 'startup')))
    e = dict(os.environ)
    e.pop('X2W_MESH_LANE_SCC', None)
    e.pop('X2W_MESH_TEST_DROP', None)
    e.pop('X2W_MESH_JOIN_TAINT', None)
    e.pop('X2W_MESH_CONFINED_SCC', None)
    e.update(env)
    r = subprocess.run([sys.executable, '-B', '-c', child, BIN], capture_output=True, text=True, env=e)
    if r.returncode:
        return 'CRASH ' + r.stderr.strip().splitlines()[-1]
    return r.stdout


def main():
    global BIN
    code, texts = program()
    out_dir = sys.argv[sys.argv.index('--write') + 1] if '--write' in sys.argv else os.path.join(
        HERE, '..', 'out', 'startup_mesh_lane_scc')
    os.makedirs(out_dir, exist_ok=True)
    BIN = os.path.join(out_dir, 'lane_scc.bin')
    with open(BIN, 'wb') as f:
        f.write(code)
    got = decoded(code)
    check('decodes to the intended %d instructions' % len(texts), [t for _, t in got] == [t for _, t in texts],
          '\n'.join('%5x  %-40s %s' % (o, t, g[1] if i < len(got) else '?') for i, ((o, t), g)
                    in enumerate(zip(texts, got + [(0, '?')] * len(texts))) if i >= len(got) or got[i][1] != t))
    on = translate({})
    check('translates with the lowering on', on.startswith('OK'), on[:300])
    check('SCC published per wave and read back', 'x2w_sany[0][wave] = 1u' in on and 'scc = x2w_sany[0][wave]' in on, '')
    check('hit scalar s40 restored for a wave with no active lane', 'x2w_any[0][wave] == 0u) s40 = ' in on, '')
    off = translate({'X2W_MESH_LANE_SCC': '0'})
    check('negative control: X2W_MESH_LANE_SCC=0 refuses again',
          off.startswith('REFUSED branch on an SCC that depends on lane masks'), off[:200])
    nopub = translate({'X2W_MESH_TEST_DROP': 'publish'})
    check('negative control switch: publish dropped', nopub.startswith('OK') and 'x2w_sany[0][wave] = 1u' not in nopub, nopub[:200])
    nores = translate({'X2W_MESH_TEST_DROP': 'restore'})
    check('negative control switch: restore dropped', nores.startswith('OK') and ') s40 = ' not in nores, nores[:200])
    print('program written to %s (difftest: python -B difftest.py %s <harness> --groups 4 --seed 1)' % (BIN, BIN))
    # program B: active-uniform SCC inside a per-lane range
    code_b, texts_b = program(UNIFORM_B)
    BIN = os.path.join(out_dir, 'lane_scc_uniform.bin')
    with open(BIN, 'wb') as f:
        f.write(code_b)
    got = decoded(code_b)
    check('B decodes to the intended %d instructions' % len(texts_b), [t for _, t in got] == [t for _, t in texts_b],
          ' | '.join('%x %s / %s' % (o, t, g) for (o, t), (_, g) in zip(texts_b, got) if t != g))
    on = translate({})
    check('B translates as a per-lane branch, no publish', on.startswith('OK') and 'x2w_sany' not in on, on[:300])
    off = translate({'X2W_MESH_LANE_SCC': '0'})
    check('B negative control: X2W_MESH_LANE_SCC=0 refuses again',
          off.startswith('REFUSED branch on an SCC that depends on lane masks'), off[:200])
    print('program B written to %s' % BIN)
    # program C: publish inside a formerly per-lane range (made group-uniform) + taint merged at the join
    code_c, texts_c = program(SPLIT_C)
    BIN = os.path.join(out_dir, 'lane_scc_split.bin')
    with open(BIN, 'wb') as f:
        f.write(code_c)
    got = decoded(code_c)
    check('C decodes to the intended %d instructions' % len(texts_c), [t for _, t in got] == [t for _, t in texts_c],
          ' | '.join('%x %s / %s' % (o, t, g) for (o, t), (_, g) in zip(texts_c, got) if t != g))
    on = translate({})
    check('C translates: range made group-uniform, SCC published',
          on.startswith('OK') and 'x2w_any[0][wave] = 1u' in on and 'x2w_sany[0][wave] = 1u' in on
          and 'scc = x2w_sany[0][wave]' in on, on[:300])
    off = translate({'X2W_MESH_LANE_SCC': '0'})
    check('C negative control: X2W_MESH_LANE_SCC=0 refuses again',
          off.startswith('REFUSED branch on an SCC that depends on lane masks'), off[:200])
    lin = translate({'X2W_MESH_JOIN_TAINT': '0'})
    # the linear scan carries the s_lshl's active-uniform SCC into the ELSE side and takes the branch per lane
    # without a publish: a WRONG translation (the WARP difftest of it must FAIL)
    check('C negative control: X2W_MESH_JOIN_TAINT=0 (linear taint) translates WITHOUT the publish',
          lin.startswith('OK') and 'x2w_sany' not in lin, lin[:200])
    print('program C written to %s (difftest also with X2W_MESH_TEST_DROP=publish: must FAIL)' % BIN)
    # program D: an SCC of the inactive lanes' bits (saved exec & ~exec) is not active-uniform: published
    code_d, texts_d = program(ANDN2_D)
    BIN = os.path.join(out_dir, 'lane_scc_andn2.bin')
    with open(BIN, 'wb') as f:
        f.write(code_d)
    got = decoded(code_d)
    check('D decodes to the intended %d instructions' % len(texts_d), [t for _, t in got] == [t for _, t in texts_d],
          ' | '.join('%x %s / %s' % (o, t, g) for (o, t), (_, g) in zip(texts_d, got) if t != g))
    on = translate({})
    check('D translates with the SCC published (not taken per lane)', on.startswith('OK') and 'x2w_sany' in on, on[:300])
    loose = translate({'X2W_MESH_CONFINED_SCC': '0'})
    check('D negative control: X2W_MESH_CONFINED_SCC=0 takes the branch per lane (no publish)',
          loose.startswith('OK') and 'x2w_sany' not in loose, loose[:200])
    print('program D written to %s (difftest also with X2W_MESH_CONFINED_SCC=0: must FAIL)' % BIN)
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
