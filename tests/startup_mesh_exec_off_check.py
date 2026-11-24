"""Check the exec-off rule of tools/startup/mesh2hlsl.py (MeshStage.merge_exec_off / exec_off_use): a register that
holds a symbolic value (here a descriptor-table pointer) before a group-uniform execz range and a lane mask written
inside it becomes ('conflict',) at the join; a 64-bit mask op with exec whose lane bit does not depend on it when
the lane's exec bit is 0 may still read it.

usage: startup_mesh_exec_off_check.py [--write <dir>]

Hand-assembled gfx1013 NGG programs (no title bytes; encoded field by field, opcode numbers found by decoding with
tools/disasm_rdna2.py, the decoded text compared with the intended text). Per group g: N = 60 * (g & 1) +
40 * ((g >> 1) & 1), T = 20 * (g & 3): g = 0 has no active lane at the branch (the whole group skips the range),
g = 1 and 2 a mixed wave 0 and a wave 1 with no active lane (it runs the range with exec 0 in the translation,
the hardware skips it), g = 3 a full wave 0 and a mixed wave 1.

Shape (the run programs' lane-mask merge after a divergent if, census class "execz range ... changes sN from desc
to ('conflict',)"): s26 = s16 (table pointer); s[30:31] = exec; exec = v0 < N; execz -> J;
v_cmp_gt_u32_e64 s[26:27] = v0 < T; J: <op> s[26:27], s[26:27], exec; exec = s[30:31]; exec &= s[26:27];
v1 = 9; (epilogue: exec = all, export v1 as param0.x).

Hardware meaning (AMD RDNA 2 ISA): V_CMP_*_E64 with an SGPR destination writes the compare bit of each active lane
and 0 for inactive lanes; S_CBRANCH_EXECZ branches when the wave's EXEC is 0; S_NAND_B64 D = ~(S0 & S1),
S_ORN2_B64 D = S0 | ~S1, S_OR_B64 D = S0 | S1, S_AND_B64 D = S0 & S1. A lane with EXEC bit 0 at J gets
nand: 1, orn2: 1 whatever s26 held, so the stale pointer word is never observed.

  NAND   (accept): expected v1 = 9.0 iff v0 >= N or v0 >= T, else 5.0
  ORN2   (accept): expected v1 = 9.0 iff v0 >= N or v0 < T, else 5.0
  (Float values: the differential test allows 4 ulp on attribute words, so the integers 5 and 9 would not differ.)
  OR     (refuse): s_or_b64 s[26:27], s[26:27], exec: a lane off at J would read the pointer word
  EXECW  (refuse): as NAND with `s_mov_b64 exec, s[30:31]` between J and the nand: the lanes that took the edge
                   are on again, so their bit of the stale word would matter
  LOAD   (refuse): s_load_dwordx4 through s[26:27] after J: a real descriptor use of the conflict register

Checks: each program decodes to its intended text; the accepted ones translate and are refused again with
X2W_MESH_EXECOFF=0 (negative control: the f37ad9f8 rule "execz range ... changes s26 from ptr to ('conflict',)");
the refused ones are refused with the rule on. --write keeps the programs (<dir>/<name>.bin) for the differential
test (mesh_work/difftest.py <bin> <harness>).
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


# ---- a minimal gfx10 encoder (as tests/startup_mesh_exec_restore_check.py): field layout per format, opcode
# found by decoding -----------------------------------------------------------------------------------------------
def src(tok):
    named = {'vcc_lo': 106, 'vcc_hi': 107, 'm0': 124, 'exec_lo': 126, 'exec_hi': 127}
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
    if fmt == 'smem':
        # gfx10 SMEM: [31:26] 0x3d, [25:18] op, [12:6] sdata, [5:0] sbase / 2; word 1: [20:0] offset, [31:25] soffset
        return [(0x3d << 26) | (op << 18) | (f['d'] << 6) | (f['s0'] >> 1), (f['imm'] & 0x1fffff) | (0x7d << 25)]
    raise ValueError(fmt)


OP_RANGE = {'sop2': 128, 'sop1': 256, 'sopc': 128, 'sopp': 128, 'vop1': 256, 'vopc': 256, 'vop3': 1024, 'smem': 256}
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


def assemble(items):
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
            words = ins('sopp', 's_cbranch_' + make if make != 'branch' else 's_branch', imm=(target - off - 4) // 4)
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


def I(text, fmt, mnemonic, **ops):
    return (text, lambda: ins(fmt, mnemonic, **ops))


PROLOGUE = [
    I('s_mov_b64 exec, -1', 'sop1', 's_mov_b64', d=126, s0='-1'),
    I('v_readfirstlane_b32 s20, v5', 'vop1', 'v_readfirstlane_b32', d=20, s0='v5'),
    I('s_lshr_b32 s20, s20, 7', 'sop2', 's_lshr_b32', d=20, s0='s20', s1='7'),
    I('s_bfe_u32 s28, s3, 0x40018', 'sop2', 's_bfe_u32', d=28, s0='s3', s1='0x40018'),
    I('s_lshl_b32 s29, s28, 6', 'sop2', 's_lshl_b32', d=29, s0='s28', s1='6'),
    I('v_mbcnt_lo_u32_b32 v0, -1, s29', 'vop3', 'v_mbcnt_lo_u32_b32', d=0, s0='-1', s1='s29'),
    I('v_mbcnt_hi_u32_b32 v0, -1, v0', 'vop3', 'v_mbcnt_hi_u32_b32', d=0, s0='-1', s1='v0'),
    I('s_and_b32 s21, s20, 1', 'sop2', 's_and_b32', d=21, s0='s20', s1='1'),
    I('s_mul_i32 s21, s21, 60', 'sop2', 's_mul_i32', d=21, s0='s21', s1='60'),
    I('s_bfe_u32 s23, s20, 0x10001', 'sop2', 's_bfe_u32', d=23, s0='s20', s1='0x10001'),
    I('s_mul_i32 s23, s23, 40', 'sop2', 's_mul_i32', d=23, s0='s23', s1='40'),
    I('s_add_u32 s21, s21, s23', 'sop2', 's_add_u32', d=21, s0='s21', s1='s23'),
    I('s_and_b32 s22, s20, 3', 'sop2', 's_and_b32', d=22, s0='s20', s1='3'),
    I('s_mul_i32 s22, s22, 20', 'sop2', 's_mul_i32', d=22, s0='s22', s1='20'),
    I('v_mov_b32_e32 v1, 0x40a00000', 'vop1', 'v_mov_b32_e32', d=1, s0='0x40a00000'),   # 5.0
    I('v_mov_b32_e32 v4, 0', 'vop1', 'v_mov_b32_e32', d=4, s0='0'),
    # the table pointer (root parameter 0 = user SGPR s16) and its zero high word, as the run programs build it
    I('s_mov_b32 s27, 0', 'sop1', 's_mov_b32', d=27, s0='0'),
    I('s_mov_b32 s26, s16', 'sop1', 's_mov_b32', d=26, s0='s16'),
    # a load through it (its result unused) makes s16 a table for the synthesized root of the differential test
    I('s_load_dwordx4 s[32:35], s[26:27], 0x0', 'smem', 's_load_dwordx4', d=32, s0='s26', imm=0),
    I('s_mov_b64 s[30:31], exec', 'sop1', 's_mov_b64', d=30, s0='exec_lo'),
    I('v_cmpx_gt_u32_e32 s21, v0', 'vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0'),
    ('s_cbranch_execz J', 'execz'),
    I('v_cmp_gt_u32_e64 s26, s22, v0', 'vop3', 'v_cmp_gt_u32_e64', d=26, s0='s22', s1='v0'),
    ('J:', None),
]
EPILOGUE = [
    I('s_mov_b64 exec, -1', 'sop1', 's_mov_b64', d=126, s0='-1'),
    I('s_mov_b32 m0, 0x80080', 'sop1', 's_mov_b32', d=124, s0='0x80080'),
    ('s_sendmsg 0x9', lambda: ins('sopp', 's_sendmsg', imm=9)),
    I('v_cvt_f32_u32_e32 v2, v0', 'vop1', 'v_cvt_f32_u32_e32', d=2, s0='v0'),
    I('v_mov_b32_e32 v3, 0', 'vop1', 'v_mov_b32_e32', d=3, s0='0'),
    ('exp pos0 v2, v2, v2, v2 done', lambda: exp(12, ['v2', 'v2', 'v2', 'v2'], True)),
    ('exp param0 v1, v0, v4, v0', lambda: exp(32, ['v1', 'v0', 'v4', 'v0'])),
    ('exp target_20 v3, off, off, off done', lambda: exp(20, ['v3', None, None, None], True)),
    ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
]
RESTORE = I('s_mov_b64 exec, s[30:31]', 'sop1', 's_mov_b64', d=126, s0='s30')
TAIL = [RESTORE, I('s_and_b64 exec, s[26:27], exec', 'sop2', 's_and_b64', d=126, s0='s26', s1='exec_lo'),
        I('v_mov_b32_e32 v1, 0x41100000', 'vop1', 'v_mov_b32_e32', d=1, s0='0x41100000')]   # 9.0


def merge_op(mnemonic):
    return I('%s s[26:27], s[26:27], exec' % mnemonic, 'sop2', mnemonic, d=26, s0='s26', s1='exec_lo')


PROGRAMS = {
    'nand': (True, PROLOGUE + [merge_op('s_nand_b64')] + TAIL + EPILOGUE),
    'orn2': (True, PROLOGUE + [merge_op('s_orn2_b64')] + TAIL + EPILOGUE),
    'or': (False, PROLOGUE + [merge_op('s_or_b64')] + TAIL + EPILOGUE),
    'execw': (False, PROLOGUE + [RESTORE, merge_op('s_nand_b64')] + TAIL + EPILOGUE),
    'load': (False, PROLOGUE + [I('s_load_dwordx4 s[36:39], s[26:27], 0x0', 'smem', 's_load_dwordx4', d=36,
                                  s0='s26', imm=0)] + [merge_op('s_nand_b64')] + TAIL + EPILOGUE),
}
REFUSAL = {'or': 'numeric use of s26 holding conflict', 'execw': 'numeric use of s26 holding conflict',
           'load': 's_load through s26 holding (\'conflict\',)'}


def translate(path, env):
    """Run the translator in a child process (its switches are read at import): HLSL or the refusal text. Root:
    parameter 0 a descriptor table (user SGPR s16), parameter 1 one root constant (s17)."""
    child = ('import sys; sys.path[:0] = [%r, %r]\n'
             'import gfx2hlsl as G, mesh2hlsl as M\n'
             'code = open(sys.argv[1], "rb").read()\n'
             'try:\n'
             '    p = M.MeshPipeline(code, None, M.synth_layout({16}, set(), {16, 17}, 16), user_sgpr=16)\n'
             '    sys.stdout.write("OK\\n" + p.hlsl_ms())\n'
             'except G.Unsupported as e:\n'
             '    sys.stdout.write("REFUSED %%s\\n" %% e)\n'
             % (os.path.join(HERE, '..', 'tools'), os.path.join(HERE, '..', 'tools', 'startup')))
    e = dict(os.environ)
    e.pop('X2W_MESH_EXECOFF', None)
    e.update(env)
    r = subprocess.run([sys.executable, '-B', '-c', child, path], capture_output=True, text=True, env=e)
    if r.returncode:
        return 'CRASH ' + (r.stderr.strip().splitlines() or ['?'])[-1]
    return r.stdout


def main():
    out_dir = sys.argv[sys.argv.index('--write') + 1] if '--write' in sys.argv else os.path.join(
        HERE, '..', 'out', 'startup_mesh_exec_off')
    os.makedirs(out_dir, exist_ok=True)
    refused_old = "REFUSED execz range 0x"
    for name, (accept, items) in PROGRAMS.items():
        code, texts = assemble(items)
        path = os.path.join(out_dir, name + '.bin')
        with open(path, 'wb') as f:
            f.write(code)
        got = decoded(code)
        check('%s: decodes to the intended %d instructions' % (name, len(texts)),
              [t for _, t in got] == [t for _, t in texts],
              '\n'.join('%5x  %-40s %s' % (o, t, g[1]) for (o, t), g in zip(texts, got) if g[1] != t))
        on = translate(path, {})
        if accept:
            check('%s: translates with the rule on' % name, on.startswith('OK'), on[:300])
            formula = {'nand': 's26 = (~(s26 & exec)) & 1;', 'orn2': 's26 = (s26 | ~exec) & 1;'}[name]
            check('%s: the merge reads the run-time s26 (%s)' % (name, formula), formula in on,
                  [l.strip() for l in on.splitlines() if 's26 =' in l][:4])
            off = translate(path, {'X2W_MESH_EXECOFF': '0'})
            check('%s: negative control X2W_MESH_EXECOFF=0 refuses again' % name,
                  off.startswith(refused_old) and "changes s26 from ptr to ('conflict',)" in off, off[:200])
        else:
            check('%s: still refused with the rule on (%s)' % (name, REFUSAL[name]),
                  on.startswith('REFUSED ' + REFUSAL[name]), on[:200])
    print('programs written to %s (difftest: python -B difftest.py <dir>/nand.bin <harness> --groups 4 --seed 1)'
          % out_dir)
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
