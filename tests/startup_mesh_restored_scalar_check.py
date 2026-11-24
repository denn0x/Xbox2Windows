"""Check two refinements of the skipped-range scalar rule of tools/startup/mesh2hlsl.py (X2W_MESH_RESTORED_SCALARS):
a scalar written in a per-lane skipped range is not "live after it" when (a) every exit of the range gives it back
its value from before (MeshStage.restored_scalars: `s_mov_b32 sT, sR; ... sR = ...; ... s_mov_b32 sR, sT`), or
(b) the next access after the range is a v_cmp that writes it as the high half of a wave64 lane mask
(MeshStage.liveness with pair writes); and a v_cndmask that reads the mask pair keeps the high half live.

usage: startup_mesh_restored_scalar_check.py [--write <dir>]

Hand-assembled gfx1013 NGG programs (no title bytes; encoder of tests/startup_mesh_exec_restore_check.py). Each has
s35 = T, exec = v0 < N, an execz range to AFTER, and inside it a uniform vccz branch (vcc = exec & (50 < N)) over
an inner range that writes s35, ending at INNER (the class's shape: a vccz range inside a per-lane execz range):

  SHUFFLE (accept): s33 = s35; s35 = 0; v1 = 9; s35 = s33     INNER: v4 = s35
  MASKED  (accept): s35 = 0; v1 = 9                           INNER: v_cmp_gt_u32_e64 s34 (writes s[34:35]);
                                                                     s[12:13] = s[34:35] & exec; v4 = s12 ? 1 : 0
  LIVE    (refuse): s35 = 0; v1 = 9                           INNER: v4 = s35
  READ    (refuse): s[34:35] = a mask first; s35 = 0; v1 = 9  INNER: v_cndmask_b32_e64 v4, 0, 1, s34 (reads s[34:35])

Hardware meaning (AMD RDNA 2 ISA: S_MOV_B32 copies; a VOP3 compare in wave64 writes its SDST pair; V_CNDMASK_B32
e64 reads SSRC2 as a wave64 mask pair): in SHUFFLE and MASKED the lanes that skip the inner range see the same s35
as the wave afterwards (SHUFFLE: T; MASKED: rewritten before any read); in LIVE and READ they would not.

Checks: decode; SHUFFLE and MASKED translate with the rule on and are refused with X2W_MESH_RESTORED_SCALARS=0
(the f37ad9f8 reason); LIVE and READ are refused with the rule on.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import startup_mesh_exec_restore_check as E  # noqa: E402

I, ins, exp = E.I, E.ins, E.exp

HEAD = E.PROLOGUE + [
    I('s_mov_b32 s35, s22', 'sop1', 's_mov_b32', d=35, s0='s22'),
    I('v_mov_b32_e32 v6, s21', 'vop1', 'v_mov_b32_e32', d=6, s0='s21'),
    E.CMPX_N,
    ('s_cbranch_execz AFTER', 'execz'),
    I('v_cmp_lt_u32_e32 vcc_lo, 50, v6', 'vopc', 'v_cmp_lt_u32_e32', s0='50', s1='v6'),
    ('s_cbranch_vccz INNER', 'vccz'),
]
ZERO = I('s_mov_b32 s35, 0', 'sop1', 's_mov_b32', d=35, s0='0')
TAIL = [
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
V4_S35 = I('v_mov_b32_e32 v4, s35', 'vop1', 'v_mov_b32_e32', d=4, s0='s35')
CMP64 = I('v_cmp_gt_u32_e64 s34, s22, v0', 'vop3', 'v_cmp_gt_u32_e64', d=34, s0='s22', s1='v0')
CND = I('v_cndmask_b32_e64 v4, 0, 1, s34', 'vop3', 'v_cndmask_b32_e64', d=4, s0='0', s1='1', s2='s34')
SAVE_MASK = I('v_cmp_gt_u32_e64 s34, s23, v0', 'vop3', 'v_cmp_gt_u32_e64', d=34, s0='s23', s1='v0')

PROGRAMS = {
    'shuffle': (True, HEAD + [I('s_mov_b32 s33, s35', 'sop1', 's_mov_b32', d=33, s0='s35'), ZERO, E.V1_9,
                              I('s_mov_b32 s35, s33', 'sop1', 's_mov_b32', d=35, s0='s33'), ('INNER:', None)]
                + [V4_S35] + TAIL),
    'masked': (True, HEAD + [ZERO, E.V1_9, ('INNER:', None), CMP64,
                             I('s_and_b64 s[12:13], s[34:35], exec', 'sop2', 's_and_b64', d=12, s0='s34', s1='exec_lo'),
                             I('v_cndmask_b32_e64 v4, 0, 1, s12', 'vop3', 'v_cndmask_b32_e64', d=4, s0='0', s1='1', s2='s12')]
               + TAIL),
    'live': (False, HEAD + [ZERO, E.V1_9, ('INNER:', None), V4_S35] + TAIL),
    'read': (False, E.PROLOGUE + [SAVE_MASK] + HEAD[len(E.PROLOGUE):] + [ZERO, E.V1_9, ('INNER:', None), CND] + TAIL),
}
OLD = 'REFUSED scalar s35 written in the per-lane skipped range'


def main():
    out_dir = sys.argv[sys.argv.index('--write') + 1] if '--write' in sys.argv else os.path.join(
        HERE, '..', 'out', 'startup_mesh_restored_scalar')
    os.makedirs(out_dir, exist_ok=True)
    for name, (accept, items) in PROGRAMS.items():
        code, texts = E.assemble(items)
        path = os.path.join(out_dir, name + '.bin')
        with open(path, 'wb') as f:
            f.write(code)
        got = E.decoded(code)
        E.check('%s: decodes to the intended %d instructions' % (name, len(texts)),
                [t for _, t in got] == [t for _, t in texts],
                '\n'.join('%5x  %-40s %s' % (o, t, g[1]) for (o, t), g in zip(texts, got) if g[1] != t))
        on = E.translate(path, {'X2W_MESH_RESTORED_SCALARS': '1'})
        if accept:
            E.check('%s: translates with the rule on' % name, on.startswith('OK'), on[:300])
            off = E.translate(path, {'X2W_MESH_RESTORED_SCALARS': '0'})
            E.check('%s: negative control X2W_MESH_RESTORED_SCALARS=0 refuses again' % name, off.startswith(OLD), off[:200])
        else:
            E.check('%s: still refused with the rule on' % name, on.startswith(OLD), on[:200])
    print('programs written to %s' % out_dir)
    if E.failures:
        print('%d check(s) failed' % len(E.failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
