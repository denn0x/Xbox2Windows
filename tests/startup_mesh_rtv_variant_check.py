"""Check the RTV variant request of tools/startup/mesh2hlsl.py translate_request: req_<V>.rtv holds
`rtv1 <base key> <DXGI formats>` with V = fnv(that text) (gfx2hlsl.translate_request's contract; the mesh runtime
publishes it for a bound integer or 32-bit float target, d3d12_mesh.inl). The variant takes the base request's
blobs, checked against the base key; its pixel exports follow the target formats (Pipeline.mrt_kind) and its
manifest carries the rtv1 line.

usage: startup_mesh_rtv_variant_check.py [--dxc]

Hand-assembled gfx1013 programs (no title bytes; encoded field by field with the encoder of
tests/startup_mesh_exec_off_check.py, the decoded text compared with the intended text): an NGG mesh stage
(GS_ALLOC_REQ, pos0, param0, primitive export) and a pixel stage `v_mov_b32 v0, 0x3f800000; v_mov_b32 v1, 0x40000000;
exp mrt0 v0, v1, off, off done vm`. Root: the RTS0 blob of tests/startup_mesh_heap_sampler_check.py.
Without --dxc the DXC step of translate_to is stubbed (pure Python); with --dxc both stages compile.

  BASE      (accept): no .rtv -> exports F(x), no rtv1 line, no mrtbits source
  R32_FLOAT (accept): `rtv1 <base> 41` -> mrt0 F((x) ^ udN), `udsrc N mrtbits`, the rtv1 line; mesh HLSL unchanged
  R32_UINT  (accept): `rtv1 <base> 42` -> mrt0 written as uint4 words
  UNORM8    (accept): `rtv1 <base> 28` -> pixel HLSL identical to the base (only 32-bit targets change)
  negative controls (refuse): file name not fnv(text); malformed text; base key not the blobs' key;
                     a variant of a depth-only base (no pixel stage)
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'startup'))
import gfx2hlsl as G  # noqa: E402
import mesh2hlsl as M  # noqa: E402
from startup_mesh_exec_off_check import I, assemble, decoded, exp, ins  # noqa: E402  (the same encoder)
from startup_mesh_heap_sampler_check import root_blob  # noqa: E402

failures = []


def check(label, condition, detail=''):
    print(f"{label}: {'ok' if condition else 'MISMATCH  ' + str(detail)}")
    if not condition:
        failures.append(label)


MESH = [
    I('s_mov_b64 exec, -1', 'sop1', 's_mov_b64', d=126, s0='-1'),
    I('v_readfirstlane_b32 s20, v5', 'vop1', 'v_readfirstlane_b32', d=20, s0='v5'),
    I('s_lshr_b32 s20, s20, 7', 'sop2', 's_lshr_b32', d=20, s0='s20', s1='7'),
    I('s_bfe_u32 s28, s3, 0x40018', 'sop2', 's_bfe_u32', d=28, s0='s3', s1='0x40018'),
    I('s_lshl_b32 s29, s28, 6', 'sop2', 's_lshl_b32', d=29, s0='s28', s1='6'),
    I('v_mbcnt_lo_u32_b32 v0, -1, s29', 'vop3', 'v_mbcnt_lo_u32_b32', d=0, s0='-1', s1='s29'),
    I('v_mbcnt_hi_u32_b32 v0, -1, v0', 'vop3', 'v_mbcnt_hi_u32_b32', d=0, s0='-1', s1='v0'),
    I('v_mov_b32_e32 v4, 0', 'vop1', 'v_mov_b32_e32', d=4, s0='0'),
    I('s_mov_b32 m0, 0x80080', 'sop1', 's_mov_b32', d=124, s0='0x80080'),
    ('s_sendmsg 0x9', lambda: ins('sopp', 's_sendmsg', imm=9)),
    I('v_cvt_f32_u32_e32 v2, v0', 'vop1', 'v_cvt_f32_u32_e32', d=2, s0='v0'),
    I('v_mov_b32_e32 v3, 0', 'vop1', 'v_mov_b32_e32', d=3, s0='0'),
    ('exp pos0 v2, v2, v2, v2 done', lambda: exp(12, ['v2', 'v2', 'v2', 'v2'], True)),
    ('exp param0 v2, v2, v2, v2', lambda: exp(32, ['v2', 'v2', 'v2', 'v2'])),
    ('exp target_20 v3, off, off, off done', lambda: exp(20, ['v3', None, None, None], True)),
    ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
]


def exp_vm(target, vals, done=False):
    w = exp(target, vals, done)
    return [w[0] | (1 << 12), w[1]]   # VM (valid mask), bit 12 of word 0


PIXEL = [
    I('v_mov_b32_e32 v0, 0x3f800000', 'vop1', 'v_mov_b32_e32', d=0, s0='0x3f800000'),
    I('v_mov_b32_e32 v1, 0x40000000', 'vop1', 'v_mov_b32_e32', d=1, s0='0x40000000'),
    ('exp mrt0 v0, v1, off, off done vm', lambda: exp_vm(0, ['v0', 'v1', None, None], True)),
    ('s_endpgm', lambda: ins('sopp', 's_endpgm', imm=0)),
]


def code_of(name, items):
    code, texts = assemble(items)
    got = decoded(code)
    check('%s: decodes to the intended %d instructions' % (name, len(texts)),
          [t for _, t in got] == [t for _, t in texts],
          '\n'.join('%5x  %-40s %s' % (o, t, g[1]) for (o, t), g in zip(texts, got) if g[1] != t))
    return code


def write_request(out_dir, key, a, b, root):
    for ext, data in (('.a', a), ('.b', b), ('.root', root)):
        with open(os.path.join(out_dir, 'req_%016x%s' % (key, ext)), 'wb') as f:
            f.write(data)
    return key


def write_rtv(out_dir, text, key=None):
    key = G.fnv(text.encode('ascii')) if key is None else key
    with open(os.path.join(out_dir, 'req_%016x.rtv' % key), 'w', encoding='ascii', newline='') as f:
        f.write(text)
    return key


def outputs(out_dir, key):
    name = os.path.join(out_dir, '%016x' % key)
    res = []
    for ext in ('.ms.hlsl', '.ps.hlsl', '.txt'):
        res.append(open(name + ext).read() if os.path.isfile(name + ext) else '')
    return res


def main():
    if '--dxc' not in sys.argv[1:]:
        M.compile_hlsl = lambda path, profile, out: (0, '')   # pure Python: the HLSL text is checked, not compiled
    out_dir = os.path.join(HERE, '..', 'out', 'startup_mesh_rtv_variant')
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    ms, ps, root = code_of('mesh', MESH), code_of('pixel', PIXEL), root_blob()
    check('mesh: is_mesh', M.is_mesh(ms) and not M.is_mesh(ps), '')
    base = write_request(out_dir, G.pipeline_key(ps, ms, root), ps, ms, root)   # runtime order: .a pixel, .b mesh

    r = M.translate_request(out_dir, base)
    b_ms, b_ps, b_txt = outputs(out_dir, base)
    check('base: translates', r == 'ok', r)
    check('base: mrt0 is a plain float export', 'F(' in b_ps and '^ ud' not in b_ps,
          [l.strip() for l in b_ps.splitlines() if 'o.mrt0' in l])
    check('base: no rtv1 line, no mrtbits source', 'rtv1' not in b_txt and 'mrtbits' not in b_txt, b_txt)

    text = 'rtv1 %d 41' % base
    v = write_rtv(out_dir, text)
    r = M.translate_request(out_dir, v)
    v_ms, v_ps, v_txt = outputs(out_dir, v)
    src = [l for l in v_txt.splitlines() if l.startswith('udsrc ') and l.endswith(' mrtbits')]
    ud = src[0].split()[1] if src else '?'
    mrt0 = [l.strip() for l in v_ps.splitlines() if 'o.mrt0' in l]
    check('R32_FLOAT: translates', r == 'ok', r)
    check('R32_FLOAT: manifest carries `%s`' % text, v_txt.endswith('\n' + text + '\n'), v_txt[-200:])
    check('R32_FLOAT: manifest names the mrtbits source', len(src) == 1, v_txt)
    check('R32_FLOAT: mrt0 exports the register bits F((x) ^ ud%s)' % ud,
          len(mrt0) == 1 and mrt0[0].count('^ ud%s)' % ud) == 2 and 'F((' in mrt0[0], mrt0)
    # the stages share one entry constant buffer: the mesh stage declares the added source, nothing else changes
    grown = b_ms.replace(' uint ud%d; };' % (int(ud) - 1), ' uint ud%d; uint ud%s; };' % (int(ud) - 1, ud), 1) \
        if ud.isdigit() and int(ud) > 0 else None
    check('R32_FLOAT: mesh HLSL equals the base but for the shared entry cbuffer gaining ud%s' % ud,
          v_ms == grown and v_ms != b_ms, [l for l in v_ms.splitlines() if l.startswith('cbuffer entry')])
    check('R32_FLOAT: written under the variant key only', os.path.isfile(os.path.join(out_dir, '%016x.txt' % v))
          and outputs(out_dir, base)[2] == b_txt, '')

    v = write_rtv(out_dir, 'rtv1 %d 42' % base)
    r = M.translate_request(out_dir, v)
    u_ps = outputs(out_dir, v)[1]
    mrt0 = [l.strip() for l in u_ps.splitlines() if 'o.mrt0' in l]
    check('R32_UINT: translates', r == 'ok', r)
    check('R32_UINT: mrt0 is a uint4 of the register words', len(mrt0) == 1 and 'uint4(' in mrt0[0]
          and '^ ud' not in mrt0[0], mrt0)

    v = write_rtv(out_dir, 'rtv1 %d 28' % base)
    r = M.translate_request(out_dir, v)
    n_ps, n_txt = outputs(out_dir, v)[1:]
    check('UNORM8: translates', r == 'ok', r)
    check('UNORM8: pixel HLSL equals the base (negative control: only 32-bit targets change)', n_ps == b_ps, '')
    check('UNORM8: manifest carries the rtv1 line, no mrtbits', n_txt.endswith('rtv1 %d 28\n' % base)
          and 'mrtbits' not in n_txt, n_txt[-200:])

    # negative controls
    v = write_rtv(out_dir, text, key=G.fnv(text.encode('ascii')) ^ 1)
    r = M.translate_request(out_dir, v)
    check('negative: file name not fnv(text) refused', r == 'unsupported: invalid RTV variant request', r)
    check('negative: nothing written for it', not any(os.path.isfile(os.path.join(out_dir, '%016x%s' % (v, e)))
                                                      for e in ('.txt', '.ps.hlsl', '.fail')), '')
    v = write_rtv(out_dir, text + '\n')
    r = M.translate_request(out_dir, v)
    check('negative: malformed text (trailing newline) refused', r == 'unsupported: invalid RTV variant request', r)
    other = write_request(out_dir, base ^ 0x5a5a, ps, ms, root)   # the base blobs under a key that is not theirs
    v = write_rtv(out_dir, 'rtv1 %d 41' % other)
    r = M.translate_request(out_dir, v)
    check('negative: base key not the blobs\' key refused', r == 'unsupported: RTV variant source key mismatch', r)
    depth = write_request(out_dir, G.pipeline_key(b'', ms, root), b'', ms, root)
    check('depth-only base: translates without a variant', M.translate_request(out_dir, depth) == 'ok', '')
    v = write_rtv(out_dir, 'rtv1 %d 41' % depth)
    r = M.translate_request(out_dir, v)
    check('negative: variant of a depth-only base refused',
          r == 'unsupported: RTV variant of a mesh pipeline without a pixel stage', r)

    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
