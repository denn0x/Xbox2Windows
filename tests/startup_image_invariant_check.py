"""Lane-invariant image reads and the s_brev_b32 constant fold (tools/startup/gfx2hlsl.py).

1. image_load / image_sample_lz of a bound texture (rN) through a fixed sampler at lane-invariant coordinates gives
   the same value in every lane (no implicit derivatives: same texel or same filtered value), so the destination
   VGPRs join uniform_v and a later v_readfirstlane_b32 of them is exact (measured form: a 1x1 pass loading texel
   (0, 0) with a16 and v0 = 0, then sampling a 1D texture at a coordinate computed from it). Controls: a
   coordinate VGPR not proven invariant, and image_sample (implicit derivatives), leave the destination out.
2. s_brev_b32 of an inline constant or a constant SGPR is folded (RDNA 2 ISA SOP1 S_BREV_B32: D = S0 with bits
   reversed; 40 -> 0x14000000, an inline sampler word); a run-time source keeps reversebits().
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'startup'))
sys.path.insert(0, HERE)
import gfx2hlsl as G  # noqa: E402
from startup_scalar_bits_check import sop1  # noqa: E402


class Pipe:
    sampler_heap = False
    root = {'params': [], 'flags': 0}
    resource = G.Pipeline.resource

    def __init__(self):
        self.features = frozenset()
        self.resources, self.uds, self.modified, self.heapvars, self.notes = {}, [], set(), [], set()
        self.samplers = {}

    def ud(self, source):
        if source not in self.uds:
            self.uds.append(source)
        return self.uds.index(source)

    def sampler(self, key, comparison=False):
        return 'q0'


def stage():
    s = G.Stage(Pipe(), 'ps', b'')
    s.lines, s.pc = [], 0
    s.uniform_v, s.partial_uniform, s.lane_s, s.masks = set(), {}, set(), set()
    s.exec_known = True
    for i in range(8):
        s.sym[24 + i] = ('desc', ('table', 1), 0x20, i)
    for i in range(4):
        s.sym[20 + i] = ('const', 0)
    for i in range(8):
        s.sym[32 + i] = ('desc', ('table', 1), 0x40, i)  # a 1D texture
    return s


def main():
    s = stage()
    s.set_v(0, '0u')
    assert 0 in s.uniform_v
    s.one('image_load', 'image_load v[2:3], v[0:1], s[24:31] dmask:0xa dim:SQ_RSRC_IMG_2D unorm a16')
    assert {2, 3} <= s.uniform_v, (s.lines, s.uniform_v)
    s.one('image_sample_lz', 'image_sample_lz v1, v2, s[32:39], s[20:23] dmask:0x1 dim:SQ_RSRC_IMG_1D')
    assert 1 in s.uniform_v, (s.lines, s.uniform_v)
    s.one('v_readfirstlane_b32', 'v_readfirstlane_b32 s22, v1')
    assert s.lines[-1] == 's22 = v1;', s.lines
    t = stage()
    t.one('image_load', 'image_load v[2:3], v[5:6], s[24:31] dmask:0xa dim:SQ_RSRC_IMG_2D unorm a16')
    assert not {2, 3} & t.uniform_v, t.uniform_v
    try:
        t.one('v_readfirstlane_b32', 'v_readfirstlane_b32 s22, v2')
    except G.Unsupported as error:
        assert 'proven uniform' in str(error)
    else:
        raise AssertionError('readfirstlane of a lane-dependent load accepted')
    u = stage()
    u.set_v(0, '0u')
    u.one('image_sample', 'image_sample v1, v0, s[32:39], s[20:23] dmask:0x1 dim:SQ_RSRC_IMG_1D')
    assert 1 not in u.uniform_v, u.uniform_v
    print('image_invariant: load and lz at invariant coordinates join uniform_v (readfirstlane exact); a '
          'lane-dependent coordinate and an implicit-derivative sample do not')

    for words, want in (([sop1(0x0b, 10, 0x80 + 40)], ('const', 0x14000000)),):
        _, _, name, text = G.decode(struct.pack('<I', words[0]))[0]
        assert text == 's_brev_b32 s10, 40', text
        v = stage()
        v.one(name, text)
        assert v.sym.get(10) == want, (v.sym.get(10), v.lines)
    w = stage()
    w.one('s_brev_b32', 's_brev_b32 s10, s3')
    assert w.lines == ['s10 = reversebits(s3);'], w.lines
    for x in (0, 1, 40, 0x80000000, 0x12345678, 0xffffffff):
        v = stage()
        v.sym[3] = ('const', x)
        v.one('s_brev_b32', 's_brev_b32 s10, s3')
        assert v.sym[10] == ('const', int('{:032b}'.format(x)[::-1], 2)), (x, v.sym[10])
    print('brev: constant sources folded (6 values + the inline 40 -> 0x14000000); run-time source unchanged')


if __name__ == '__main__':
    main()
