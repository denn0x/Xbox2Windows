"""Resource-heap (bindless) descriptors in a draw stage: manifest `heap 1`, behind X2W_DRAW_FEATURES=heap.

Synthetic code: root flag 0x400 puts the resource-heap base in s8; the pixel stage builds a byte offset in s10,
loads an 8-dword image descriptor with `s_load_dwordx8 s[16:23], s[8:9], s10` and reads one texel through it.
The HLSL reads the view token at that offset from heapmem and indexes the view-id region with the layout agreed
with the draw runtime (draw-rt, board msgs 180500): heapmem t0 space1, vinfo t1 space1, Texture2D array in space2,
index 8 + 8 * id + kind (kind 0) for ids below 90000, else the empty view `kind`. With the feature off the stage is
refused exactly as before.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from startup_image_3d_check import VS_WORDS  # noqa: E402


def heap_root():
    rts0 = struct.pack('<6I', 2, 0, 24, 0, 0, 0x400)
    part = b'RTS0' + struct.pack('<I', len(rts0)) + rts0
    return b'DXBC' + bytes(16) + struct.pack('<III', 1, 32 + 4 + len(part), 1) + struct.pack('<I', 36) + part


S_MOV_S10_64 = 0xbe800000 | 10 << 16 | 3 << 8 | 192            # s_mov_b32 s10, 64
S_LOAD_X8 = [0xf4000000 | 3 << 18 | 16 << 6 | 8 >> 1, 10 << 25]  # s_load_dwordx8 s[16:23], s[8:9], s10
V_MOV_V0_1 = 0x7e000000 | 0 << 17 | 1 << 9 | 129                 # v_mov_b32 v0, 1
V_MOV_V1_2 = 0x7e000000 | 1 << 17 | 1 << 9 | 130                 # v_mov_b32 v1, 2
IMAGE_LOAD = [0xf0000000 | 0xf << 8 | 1 << 3, 0 | 4 << 8 | (16 // 4) << 16]
EXPORT = [0xf800180f, 0x07060504]                                # exp mrt0 v4, v5, v6, v7 done vm


def words(*parts):
    flat = []
    for p in parts:
        flat += p if isinstance(p, list) else [p]
    return struct.pack('<%dI' % (len(flat) + 1), *flat, 0xbf810000)


def main():
    ps = words(S_MOV_S10_64, S_LOAD_X8, V_MOV_V0_1, V_MOV_V1_2, IMAGE_LOAD, EXPORT)
    text = [x[3] for x in G.decode(ps)]
    assert text[1] == 's_load_dwordx8 s[16:23], s[8:9], s10', text
    assert text[4] == 'image_load v[4:7], v[0:1], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D', text
    try:
        G.Pipeline(VS_WORDS, ps, heap_root(), features=())
    except G.Unsupported as error:
        assert "s_load with a register offset through ('resheap',)" in str(error), str(error)
    else:
        raise AssertionError('heap load accepted with the feature off')

    pipe = G.Pipeline(VS_WORDS, ps, heap_root(), features=('heap',))
    hlsl, manifest = pipe.hlsl_ps(), pipe.manifest()
    for line in ('ByteAddressBuffer heapmem : register(t0, space1);',
                 'ByteAddressBuffer vinfo : register(t1, space1);',
                 'uint x2w_vid(uint h) { return heapmem.Load(h) == 0x56573258u ? heapmem.Load(h + 4) : 0xffffffffu; }',
                 'uint x2w_hix(uint h, uint k) { uint i = x2w_vid(h); return i < 90000u ? 8u + 8u * i + k : k; }',
                 'Texture2D<float4> heaptex2d[720008] : register(t0, space2);',
                 'h0 = 0x00000040u;',
                 't = heaptex2d[NonUniformResourceIndex(x2w_hix(h0, 0u))].Load(int3(v0, v1, 0));'):
        assert line in hlsl, (line, hlsl)
    assert 'heaptex2da' not in hlsl and 'heapbuf' not in hlsl, 'only the kinds used are declared'
    assert 'heap 1' in manifest.split('\n') and 'srv ' not in manifest, manifest
    # The index helper, evaluated: ids below 90000 land at 8 + 8 * id + kind, others on the empty view kind.
    hix = lambda i, k: 8 + 8 * i + k if i < 90000 else k  # noqa: E731
    assert (hix(0, 0), hix(1, 0), hix(89999, 4), hix(0xffffffff, 2)) == (8, 16, 720004, 2)
    assert hix(89999, 4) < 720008, 'the bounded arrays cover the whole view-id region'
    # A heap descriptor's words are not numbers here: reading one is refused.
    numeric = words(S_MOV_S10_64, S_LOAD_X8, 0x7e000000 | 4 << 17 | 1 << 9 | 17,  # v_mov_b32 v4, s17
                    V_MOV_V1_2, 0x7e000000 | 5 << 17 | 1 << 9 | 130, 0x7e000000 | 6 << 17 | 1 << 9 | 130,
                    0x7e000000 | 7 << 17 | 1 << 9 | 130, EXPORT)
    try:
        G.Pipeline(VS_WORDS, numeric, heap_root(), features=('heap',)).hlsl_ps()
    except G.Unsupported as error:
        assert 'holding desc' in str(error), str(error)
    else:
        raise AssertionError('a heap descriptor word was read as a number')
    print('heap: texel through a run-time heap offset; manifest heap 1; feature-off and numeric-word controls refused')


if __name__ == '__main__':
    main()
