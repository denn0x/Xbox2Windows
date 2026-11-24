"""Check the constant sampler-heap entry of tools/startup/mesh2hlsl.py (const_heap_sampler, MeshStage.heap_sampler,
MeshPixelStage): four sampler words loaded from the sampler heap at a constant byte offset become entry offset / 16
of the table the draw path binds for run-time offsets (`SamplerState qh[2048] : register(s0, space1)`, manifest
`smpheap 2048 1 fallback 2047`), opt-in with X2W_SAMPLER_HEAP=1 like that form.

usage: startup_mesh_heap_sampler_check.py

Hand-assembled gfx1013 NGG mesh programs (no title bytes; encoded field by field, opcode numbers found by decoding
with tools/disasm_rdna2.py, the decoded text compared with the intended text). Root: parameter 0 a descriptor table,
parameter 1 one root constant, flag 0x800 (SAMPLER_HEAP_DIRECTLY_INDEXED): mesh user SGPRs s16 table, s17 constant,
s18 sampler heap. The run programs read their sampler heap the same way (`s_load_dwordx4 s[..], s[24:25], 0x410`).
Pixel stage of the pair: the source-built fixture of tests/startup_sampler_heap_check.py (CONST_WORDS: llvm-mc
words of `s_load_dwordx4 s[24:27], s[10:11], 0x10` + image_sample), whose s10 is the same sampler heap.

Hardware meaning (AMD RDNA 2 ISA, S_LOAD_DWORDX4 / IMAGE_SAMPLE): the sampler state is the four dwords at
heap base + offset; the stand-in's table entry n holds the title's heap entry at byte 16 * n (gfx2hlsl contract).

  CONST     (accept): offset 0x410 -> qh[65u]
  UNALIGNED (accept): offset 0x418 -> qh[2047u] (the reserved fallback entry, as x2w_sampler_entry does)
  PAIR      (accept): CONST mesh stage + the fixture pixel stage (offset 0x10) -> qh[1u] in the pixel stage
  EDITED    (refuse): s_and_b32 s42, s42, 0xffff between the load and the sample (sampler word changed in the shader)
  MIXED     (refuse): s_mov_b32 s41, 0 between the load and the sample (words no longer one heap entry)

Checks: each program decodes to its intended text; accepted ones translate with X2W_SAMPLER_HEAP=1, name the entry,
declare the table and write the smpheap line, and are refused without the switch (negative control: the switch off
gives the f37ad9f8 behaviour, a refusal, with the reason naming the offset); refused ones are refused with it.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'startup'))
import disasm_rdna2 as D  # noqa: E402
from startup_mesh_exec_off_check import I, assemble, decoded, exp, ins  # noqa: E402  (the same encoder)

failures = []


def check(label, condition, detail=''):
    print(f"{label}: {'ok' if condition else 'MISMATCH  ' + detail}")
    if not condition:
        failures.append(label)


def image_sample(vdata, vaddr, srsrc, ssamp):
    """MIMG image_sample, dmask 0xf, dim 2D (word 0 as the fixture's llvm-mc encoding 080f80f0); word 1: vaddr [7:0],
    vdata [15:8], srsrc / 4 [20:16], ssamp / 4 [25:21]."""
    return [0xf0800f08, vaddr | (vdata << 8) | ((srsrc // 4) << 16) | ((ssamp // 4) << 21)]


def program(offset, between=()):
    return [
        I('s_mov_b64 exec, -1', 'sop1', 's_mov_b64', d=126, s0='-1'),
        I('v_readfirstlane_b32 s20, v5', 'vop1', 'v_readfirstlane_b32', d=20, s0='v5'),
        I('s_lshr_b32 s20, s20, 7', 'sop2', 's_lshr_b32', d=20, s0='s20', s1='7'),
        I('s_bfe_u32 s28, s3, 0x40018', 'sop2', 's_bfe_u32', d=28, s0='s3', s1='0x40018'),
        I('s_lshl_b32 s29, s28, 6', 'sop2', 's_lshl_b32', d=29, s0='s28', s1='6'),
        I('v_mbcnt_lo_u32_b32 v0, -1, s29', 'vop3', 'v_mbcnt_lo_u32_b32', d=0, s0='-1', s1='s29'),
        I('v_mbcnt_hi_u32_b32 v0, -1, v0', 'vop3', 'v_mbcnt_hi_u32_b32', d=0, s0='-1', s1='v0'),
        I('s_mov_b32 s11, 0', 'sop1', 's_mov_b32', d=11, s0='0'),
        I('s_mov_b32 s10, s16', 'sop1', 's_mov_b32', d=10, s0='s16'),
        I('s_load_dwordx8 s[32:39], s[10:11], 0x0', 'smem', 's_load_dwordx8', d=32, s0='s10', imm=0),
        I('s_mov_b32 s13, 0', 'sop1', 's_mov_b32', d=13, s0='0'),
        I('s_mov_b32 s12, s18', 'sop1', 's_mov_b32', d=12, s0='s18'),
        I('s_load_dwordx4 s[40:43], s[12:13], %#x' % offset, 'smem', 's_load_dwordx4', d=40, s0='s12', imm=offset),
    ] + list(between) + [
        I('v_mov_b32_e32 v6, 0x3f000000', 'vop1', 'v_mov_b32_e32', d=6, s0='0x3f000000'),
        I('v_mov_b32_e32 v7, 0x3f000000', 'vop1', 'v_mov_b32_e32', d=7, s0='0x3f000000'),
        ('image_sample v[8:11], v[6:7], s[32:39], s[40:43] dmask:0xf dim:SQ_RSRC_IMG_2D',
         lambda: image_sample(8, 6, 32, 40)),
        I('v_mov_b32_e32 v4, 0', 'vop1', 'v_mov_b32_e32', d=4, s0='0'),
        I('s_mov_b32 m0, 0x80080', 'sop1', 's_mov_b32', d=124, s0='0x80080'),
        ('s_sendmsg 0x9', lambda: ins('sopp', 's_sendmsg', imm=9)),
        I('v_cvt_f32_u32_e32 v2, v0', 'vop1', 'v_cvt_f32_u32_e32', d=2, s0='v0'),
        I('v_mov_b32_e32 v3, 0', 'vop1', 'v_mov_b32_e32', d=3, s0='0'),
        ('exp pos0 v2, v2, v2, v2 done', lambda: exp(12, ['v2', 'v2', 'v2', 'v2'], True)),
        ('exp param0 v8, v9, v10, v11', lambda: exp(32, ['v8', 'v9', 'v10', 'v11'])),
        ('exp target_20 v3, off, off, off done', lambda: exp(20, ['v3', None, None, None], True)),
        ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
    ]


EDIT = I('s_and_b32 s42, s42, 0xffff', 'sop2', 's_and_b32', d=42, s0='s42', s1='0xffff')
MIX = I('s_mov_b32 s41, 0', 'sop1', 's_mov_b32', d=41, s0='0')
PROGRAMS = {
    'const': (65, program(0x410)),
    'unaligned': (2047, program(0x418)),
    'edited': ('sampler heap entry at 0x410 edited in the shader', program(0x410, [EDIT])),
    'mixed': ('sampler operand s[40:43] mixes sampler heap words', program(0x410, [MIX])),
}
# tests/startup_sampler_heap_check.py CONST_WORDS (its PS_WORDS with the sampler load at the constant offset 0x10)
PS_CONST = bytes.fromhex('04040cf4000000fa' '050608f4100000fa' 'ff02007e0000a03f' 'f002027e' '7fc08cbf'
                         '080f80f00000c400' '703f8cbf' '0f1800f800010203' '000081bf')


def root_blob():
    """A DXBC container with one RTS0 part: parameter 0 a descriptor table, parameter 1 one root constant, flag 0x800
    (the layout of tests/startup_sampler_heap_check.py root_blob)."""
    params = [(0, 0), (1, 0)]
    poff = 24
    coff = poff + 12 * len(params)
    rts0 = struct.pack('<6I', 2, len(params), poff, 0, 0, 0x800)
    rts0 += struct.pack('<3I', 0, 0, coff + 12) + struct.pack('<3I', 1, 0, coff)
    rts0 += struct.pack('<3I', 0, 0, 1)
    rts0 += struct.pack('<5I', 0, 0, 0, 0, 0)
    part = b'RTS0' + struct.pack('<I', len(rts0)) + rts0
    return b'DXBC' + bytes(16) + struct.pack('<III', 1, 32 + 4 + len(part), 1) + struct.pack('<I', 36) + part


def translate(ms_path, ps_path, heap):
    """The translator in a child process (X2W_SAMPLER_HEAP is read per pipeline, X2W_MESH_* at import): 'OK' with
    the mesh HLSL, the pixel HLSL and the manifest, or the refusal text."""
    child = ('import sys; sys.path[:0] = [%r, %r]\n'
             'import gfx2hlsl as G, mesh2hlsl as M\n'
             'ms = open(sys.argv[1], "rb").read(); ps = open(sys.argv[2], "rb").read() if sys.argv[2] != "-" else None\n'
             'root = open(sys.argv[3], "rb").read()\n'
             'try:\n'
             '    p = M.MeshPipeline(ms, ps, root, user_sgpr=16)\n'
             '    text = p.hlsl_ms() + "\\n=== ps\\n" + (p.hlsl_ps() if ps else "") + "\\n=== manifest\\n" + p.manifest()\n'
             '    sys.stdout.write("OK\\n" + text)\n'
             'except G.Unsupported as e:\n'
             '    sys.stdout.write("REFUSED %%s\\n" %% e)\n'
             % (os.path.join(HERE, '..', 'tools'), os.path.join(HERE, '..', 'tools', 'startup')))
    e = dict(os.environ)
    e.pop('X2W_SAMPLER_HEAP', None)
    if heap:
        e['X2W_SAMPLER_HEAP'] = '1'
    r = subprocess.run([sys.executable, '-B', '-c', child, ms_path, ps_path or '-', translate.root], capture_output=True,
                       text=True, env=e)
    if r.returncode:
        return 'CRASH ' + (r.stderr.strip().splitlines() or ['?'])[-1]
    return r.stdout


def main():
    out_dir = os.path.join(HERE, '..', 'out', 'startup_mesh_heap_sampler')
    os.makedirs(out_dir, exist_ok=True)
    translate.root = os.path.join(out_dir, 'root.bin')
    with open(translate.root, 'wb') as f:
        f.write(root_blob())
    ps_path = os.path.join(out_dir, 'ps_const.bin')
    with open(ps_path, 'wb') as f:
        f.write(PS_CONST)
    decl = 'SamplerState qh[2048] : register(s0, space1);'
    line = 'smpheap 2048 1 fallback 2047'
    for name, (expect, items) in PROGRAMS.items():
        code, texts = assemble(items)
        path = os.path.join(out_dir, name + '.bin')
        with open(path, 'wb') as f:
            f.write(code)
        got = decoded(code)
        check('%s: decodes to the intended %d instructions' % (name, len(texts)),
              [t for _, t in got] == [t for _, t in texts],
              '\n'.join('%5x  %-40s %s' % (o, t, g[1]) for (o, t), g in zip(texts, got) if g[1] != t))
        on = translate(path, None, True)
        if isinstance(expect, int):
            ms = on.split('=== ps')[0]
            check('%s: translates with X2W_SAMPLER_HEAP=1' % name, on.startswith('OK'), on[:300])
            check('%s: samples entry qh[%du]' % (name, expect), '.SampleLevel(qh[%du], ' % expect in ms,
                  [l.strip() for l in ms.splitlines() if 'Sample' in l][:2])
            check('%s: declares the table and writes `%s`' % (name, line), decl in ms and '\n%s\n' % line in on, '')
            off = translate(path, None, False)
            check('%s: negative control (switch off) refuses' % name,
                  off.startswith('REFUSED sampler from the sampler heap at constant offset 0x41'), off[:200])
            if name == 'const':
                pair = translate(path, ps_path, True)
                ps = pair.split('=== ps')[-1].split('=== manifest')[0] if pair.startswith('OK') else ''
                check('pair: translates (mesh entry 65, pixel stage at offset 0x10)', pair.startswith('OK'), pair[:300])
                check('pair: the pixel stage samples qh[1u]', 'qh[1u]' in ps and decl in ps,
                      [l.strip() for l in ps.splitlines() if 'Sample' in l][:2])
                off = translate(path, ps_path, False)
                check('pair: negative control (switch off) refuses', off.startswith('REFUSED sampler from the sampler heap'),
                      off[:200])
        else:
            check('%s: refused with X2W_SAMPLER_HEAP=1 (%s)' % (name, expect), on.startswith('REFUSED ' + expect), on[:200])
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
