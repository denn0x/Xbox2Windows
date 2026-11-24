"""buffer_load_format_* through a resource-heap descriptor in a draw stage: manifest `heapwords 1`.

Synthetic code (layout of startup_draw_heap_check): root flag 0x400 puts the resource-heap base in s8; the pixel
stage loads a 4-dword buffer descriptor with `s_load_dwordx4 s[16:19], s[8:9], s10` and then
`buffer_load_format_x v4, v0, s[16:19], 0 idxen`. The format and swizzle are in the descriptor's word 3, which
the title's heap slot does not hold as a number (it holds a view token). Contract (mesh_work/RUNTIME_NEEDS.md 6.3,
the same as mesh2hlsl's x2w_hword; runtime d3d12_vdesc.inl, bound for draws by d3d12_draw.inl): vdesc, 32 bytes per
view id, root SRV t2 space1, word i at id * 32 + 4 * i, 0 for an unknown slot or id >= 90000.

Checked: with X2W_DRAW_FEATURES heap,bufword3,heapwords the load is x2w_fmt_load(heapbuf[...], a,
x2w_hword(h0, 3u), 0u), the address is the index times the view's vinfo stride, vdesc and x2w_hword are declared
and the manifest has `heap 1` then `heapwords 1`. The helper is evaluated in Python on a token / id table.
Negative controls: without heapwords (heap,bufword3 only) the old refusal "through heapdyn" stays; a program with
no heap format load has no vdesc declaration and no `heapwords 1` line.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from startup_image_3d_check import VS_WORDS  # noqa: E402
from startup_draw_heap_check import heap_root, words, S_MOV_S10_64, S_LOAD_X8, V_MOV_V0_1, V_MOV_V1_2, \
    IMAGE_LOAD, EXPORT  # noqa: E402

S_LOAD_X4 = [0xf4000000 | 2 << 18 | 16 << 6 | 8 >> 1, 10 << 25]       # s_load_dwordx4 s[16:19], s[8:9], s10
FORMAT_X = [0xe0000000 | 0 << 18 | 1 << 13, 0 | 4 << 8 | 4 << 16 | 128 << 24]  # buffer_load_format_x v4, v0, s[16:19], 0 idxen
MOVS = [0x7e000000 | n << 17 | 1 << 9 | 130 for n in (5, 6, 7)]       # v_mov_b32 v5..v7, 2


def hword(heapmem, vdesc, h, i):
    """x2w_hword as emitted: token check at heapmem[h], id below 90000, else 0."""
    token, vid = heapmem.get(h, (0, 0))
    vid = vid if token == G.HEAP_TOKEN else 0xffffffff
    return vdesc.get((vid, i), 0) if vid < 90000 else 0


def main():
    ps = words(S_MOV_S10_64, S_LOAD_X4, V_MOV_V0_1, FORMAT_X, MOVS, EXPORT)
    text = [x[3] for x in G.decode(ps)]
    assert text[1] == 's_load_dwordx4 s[16:19], s[8:9], s10', text
    assert text[3] == 'buffer_load_format_x v4, v0, s[16:19], 0 idxen', text

    for features in (('heap', 'bufword3'), ('heap', 'bufword3', 'uav')):
        try:
            G.Pipeline(VS_WORDS, ps, heap_root(), features=features).hlsl_ps()
        except G.Unsupported as error:
            assert 'buffer_load_format_x through heapdyn' in str(error), str(error)
        else:
            raise AssertionError('heap format load accepted without heapwords (%s)' % (features,))

    pipe = G.Pipeline(VS_WORDS, ps, heap_root(), features=('heap', 'bufword3', 'heapwords'))
    hlsl, manifest = pipe.hlsl_ps(), pipe.manifest()
    for line in ('ByteAddressBuffer x2w_vdesc : register(t2, space1);',
                 'uint x2w_hword(uint h, uint i) { uint id = x2w_vid(h); return id < 90000u ? '
                 'x2w_vdesc.Load(id * 32u + 4u * i) : 0u; }',
                 'ByteAddressBuffer heapbuf[720008] : register(t0, space6);',
                 'a = v0 * x2w_vstride(h0) + 0;',
                 'v4 = x2w_fmt_load(heapbuf[NonUniformResourceIndex(x2w_hix(h0, 4u))], a, x2w_hword(h0, 3u), 0u);'):
        assert line in hlsl, (line, hlsl)
    assert 'uint x2w_fmt_load(' in hlsl, 'the format decoder is emitted'
    lines = manifest.split('\n')
    assert 'heap 1' in lines and 'heapwords 1' in lines and lines.index('heapwords 1') == lines.index('heap 1') + 1, manifest
    assert not any(l.startswith('udsrc') and 'bufword3' in l for l in lines), manifest

    # the helper, evaluated: slot 64 holds view 7, whose word 3 is 0x00026fac (an arbitrary value)
    heapmem = {64: (G.HEAP_TOKEN, 7), 96: (0x12345678, 7), 128: (G.HEAP_TOKEN, 90000)}
    vdesc = {(7, 3): 0x00026fac, (7, 1): 0x00100000}
    assert hword(heapmem, vdesc, 64, 3) == 0x00026fac and hword(heapmem, vdesc, 64, 1) == 0x00100000
    assert hword(heapmem, vdesc, 96, 3) == 0, 'a slot without the view token reads 0'
    assert hword(heapmem, vdesc, 128, 3) == 0, 'an id at the limit reads 0'
    assert hword(heapmem, vdesc, 32, 3) == 0, 'an empty slot reads 0'

    # Negative control: a heap program without a format load has neither vdesc nor `heapwords 1`.
    plain = G.Pipeline(VS_WORDS, words(S_MOV_S10_64, S_LOAD_X8, V_MOV_V0_1, V_MOV_V1_2, IMAGE_LOAD, EXPORT),
                       heap_root(), features=('heap', 'bufword3', 'heapwords'))
    assert 'x2w_vdesc' not in plain.hlsl_ps() and 'heapwords 1' not in plain.manifest().split('\n'), plain.manifest()
    print('heapwords: heap typed load reads word 3 from vdesc (t2 space1); manifest heap 1 + heapwords 1; '
          'refused without heapwords; helper evaluated on 4 slots; image-only heap program unchanged')


if __name__ == '__main__':
    main()
