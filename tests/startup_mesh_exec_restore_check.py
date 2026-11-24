"""Check the exec-restore rule of tools/startup/mesh2hlsl.py for per-lane skipped ranges (MeshStage.exec_copies_at,
MeshStage.skipped_scalars): inside a range skipped per lane (s_cbranch_execz / vccz), `s_mov_b64 exec, sX` is
accepted when sX is proven to hold exactly the exec of the branch (on every path to it), or a copy of such a pair.

usage: startup_mesh_exec_restore_check.py [--write <dir>]

Hand-assembled gfx1013 NGG programs (no title bytes; encoded field by field, opcode numbers found by decoding with
tools/disasm_rdna2.py and the decoded text compared with the intended text). Per group g: N = 40 + 60 * (g & 1)
(wave 1 has no lane with v0 < N when N = 40), T = 20 * (g & 3), s23 = 10.

Hardware meaning (AMD RDNA 2 ISA: V_CMPX writes EXEC = EXEC & compare; S_MOV_B64 EXEC, S writes all 64 bits;
S_CBRANCH_EXECZ / VCCZ branch on the whole wave's EXEC / VCC). Restoring a pair that equals EXEC at the branch
turns on exactly the lanes that entered the range; a lane that was off at the branch stays off, which is what the
one-lane model does when that lane skips the range.

  RESTORE  (accept): exec = v0 < N; s[30:31] = exec; execz -> AFTER; exec &= v0 < T; v1 = 9;
           exec = s[30:31]; v4 = 7.            expected per lane: v1 = 9 if v0 < min(N, T) else 5; v4 = 7 if v0 < N else 0
  SHUFFLE  (accept): as RESTORE, the saved pair moved to s[34:35] inside the range and restored from there.
  VCCZ     (accept): exec = v0 < N; s[30:31] = exec; vcc = (30 < T) & exec (uniform); vccz -> AFTER; exec &= v0 < s23;
           v1 = 9; exec = s[30:31]; v4 = 7.    expected: v1 = 9 if T > 30 and v0 < min(N, 10) else 5; v4 = 7 if T > 30
           and v0 < N else 0
  WIDER    (refuse): s[30:31] = exec (all lanes); exec = v0 < N; execz; exec = s[30:31] would turn on lanes v0 >= N
           that the one-lane model left at AFTER.
  ONEPATH  (refuse): s[30:31] = exec only when s22 != 0 (a uniform branch around the copy); the other path reaches
           the execz branch with s[30:31] unrelated to exec.
  CLOBBER  (refuse): s[30:31] = exec, then v_cmp_gt_u32_e64 s29 (a wave64 mask: writes s[29:30]) before the branch.

Checks: each program decodes to its intended text; the accepted ones translate with the restore of the saved pair
(`exec = (s30) & 1` / s34) and are refused again with X2W_MESH_EXEC_COPIES=0 (negative control: the f37ad9f8 rule,
"exec written by s_mov_b64 inside the per-lane skipped range"); the refused ones are refused with the rule on.
--write keeps the programs (<dir>/<name>.bin) for the differential test (mesh_work/difftest.py <bin> <harness>).
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


# ---- a minimal gfx10 encoder (as tests/startup_mesh_lane_scc_check.py): field layout per format, opcode found by
# decoding --------------------------------------------------------------------------------------------------------
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
    I('s_add_u32 s21, s21, 40', 'sop2', 's_add_u32', d=21, s0='s21', s1='40'),
    I('s_and_b32 s22, s20, 3', 'sop2', 's_and_b32', d=22, s0='s20', s1='3'),
    I('s_mul_i32 s22, s22, 20', 'sop2', 's_mul_i32', d=22, s0='s22', s1='20'),
    I('s_mov_b32 s23, 10', 'sop1', 's_mov_b32', d=23, s0='10'),
    I('v_mov_b32_e32 v1, 5', 'vop1', 'v_mov_b32_e32', d=1, s0='5'),
    I('v_mov_b32_e32 v4, 0', 'vop1', 'v_mov_b32_e32', d=4, s0='0'),
]
EPILOGUE = [
    ('AFTER:', None),
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
CMPX_N = I('v_cmpx_gt_u32_e32 s21, v0', 'vopc', 'v_cmpx_gt_u32_e32', s0='s21', s1='v0')
CMPX_T = I('v_cmpx_gt_u32_e32 s22, v0', 'vopc', 'v_cmpx_gt_u32_e32', s0='s22', s1='v0')
SAVE = I('s_mov_b64 s[30:31], exec', 'sop1', 's_mov_b64', d=30, s0='exec_lo')
RESTORE = I('s_mov_b64 exec, s[30:31]', 'sop1', 's_mov_b64', d=126, s0='s30')
V1_9 = I('v_mov_b32_e32 v1, 9', 'vop1', 'v_mov_b32_e32', d=1, s0='9')
V4_7 = I('v_mov_b32_e32 v4, 7', 'vop1', 'v_mov_b32_e32', d=4, s0='7')

PROGRAMS = {
    'restore': (True, PROLOGUE + [CMPX_N, SAVE, ('s_cbranch_execz AFTER', 'execz'), CMPX_T, V1_9, RESTORE, V4_7]
                + EPILOGUE),
    'shuffle': (True, PROLOGUE + [CMPX_N, SAVE, ('s_cbranch_execz AFTER', 'execz'),
                                  I('s_mov_b64 s[34:35], s[30:31]', 'sop1', 's_mov_b64', d=34, s0='s30'),
                                  I('s_mov_b32 s30, 0', 'sop1', 's_mov_b32', d=30, s0='0'),
                                  CMPX_T, V1_9,
                                  I('s_mov_b64 exec, s[34:35]', 'sop1', 's_mov_b64', d=126, s0='s34'), V4_7]
                + EPILOGUE),
    'vccz': (True, PROLOGUE + [CMPX_N, SAVE,
                               I('v_mov_b32_e32 v6, s22', 'vop1', 'v_mov_b32_e32', d=6, s0='s22'),
                               I('v_cmp_lt_u32_e32 vcc_lo, 30, v6', 'vopc', 'v_cmp_lt_u32_e32', s0='30', s1='v6'),
                               ('s_cbranch_vccz AFTER', 'vccz'),
                               I('v_cmpx_gt_u32_e32 s23, v0', 'vopc', 'v_cmpx_gt_u32_e32', s0='s23', s1='v0'),
                               V1_9, RESTORE, V4_7] + EPILOGUE),
    'wider': (False, PROLOGUE + [SAVE, CMPX_N, ('s_cbranch_execz AFTER', 'execz'), RESTORE, V4_7] + EPILOGUE),
    'onepath': (False, PROLOGUE + [CMPX_N, I('s_cmp_eq_u32 s22, 0', 'sopc', 's_cmp_eq_u32', s0='s22', s1='0'),
                                   ('s_cbranch_scc1 J', 'scc1'), SAVE, ('J:', None),
                                   ('s_cbranch_execz AFTER', 'execz'), CMPX_T, V1_9, RESTORE, V4_7] + EPILOGUE),
    'clobber': (False, PROLOGUE + [CMPX_N, SAVE,
                                   I('v_cmp_gt_u32_e64 s29, s22, v0', 'vop3', 'v_cmp_gt_u32_e64', d=29, s0='s22', s1='v0'),
                                   ('s_cbranch_execz AFTER', 'execz'), CMPX_T, V1_9, RESTORE, V4_7] + EPILOGUE),
}


def translate(path, env):
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
    e.pop('X2W_MESH_EXEC_COPIES', None)
    e.update(env)
    r = subprocess.run([sys.executable, '-B', '-c', child, path], capture_output=True, text=True, env=e)
    if r.returncode:
        return 'CRASH ' + (r.stderr.strip().splitlines() or ['?'])[-1]
    return r.stdout


def main():
    out_dir = sys.argv[sys.argv.index('--write') + 1] if '--write' in sys.argv else os.path.join(
        HERE, '..', 'out', 'startup_mesh_exec_restore')
    os.makedirs(out_dir, exist_ok=True)
    refused_old = 'REFUSED exec written by s_mov_b64 inside the per-lane skipped range'
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
            reg = 's34' if name == 'shuffle' else 's30'
            check('%s: translates with the rule on' % name, on.startswith('OK'), on[:300])
            check('%s: exec restored from the saved pair %s' % (name, reg), 'exec = (%s) & 1;' % reg in on, '')
            off = translate(path, {'X2W_MESH_EXEC_COPIES': '0'})
            check('%s: negative control X2W_MESH_EXEC_COPIES=0 refuses again' % name, off.startswith(refused_old), off[:200])
        else:
            check('%s: still refused with the rule on' % name, on.startswith(refused_old), on[:200])
    print('programs written to %s (difftest: python -B difftest.py <dir>/restore.bin <harness> --groups 4 --seed 1)'
          % out_dir)
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
