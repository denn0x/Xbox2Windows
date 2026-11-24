"""buffer_store_dword* to a raw buffer UAV, behind X2W_DRAW_FEATURES=uav (tools/startup/gfx2hlsl.py).

Synthetic register states (descriptor s[16:19] from table 0 at byte 0x20). Checked:
1. feature off: refused with the old message (the op name);
2. feature on: `a = ud<uavbase> + vaddr + soffset + offset`, Store3 of the three data VGPRs, the exec guard after a
   branch made exec unknown, a literal soffset folded into the constant;
3. refusals: idxen (no UAV stride source), a descriptor also read as an SRV (either order), a vertex-buffer source,
   an unknown modifier, a ninth UAV;
4. manifest `uav 0 buf table 0 32` after the srv lines, `udsrc <n> uavbase 0`, and the HLSL declaration
   RWByteAddressBuffer u0 : register(u0).
Not checked: stores on a GPU (draw-rt's startup_draw_uav_check covers the runtime side with its own shaders).
"""
import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    sampler_heap = False
    root = {'params': [], 'flags': 0}
    uav = G.Pipeline.uav
    resource = G.Pipeline.resource

    def __init__(self, features):
        self.features = frozenset(features)
        self.resources, self.uds, self.modified, self.heapvars = {}, [], set(), []

    def ud(self, source):
        if source not in self.uds:
            self.uds.append(source)
        return self.uds.index(source)


def stage(features=('uav',), source=('table', 0), pipe=None):
    s = G.Stage(pipe or Pipe(features), 'ps', b'')
    s.lines = []
    for i in range(4):
        s.sym[16 + i] = ('desc', source, 0x20, i)
    s.pc = 0
    return s


def refused(s, op, ops, mods, want):
    try:
        getattr(s, op)(ops, mods)
    except G.Unsupported as error:
        assert want in str(error), (want, str(error))
    else:
        raise AssertionError('accepted: %s %s %s' % (op, ops, mods))


STORE = ['v[4:6]', 'v1', 's[16:19]', '0']


def main():
    refused(stage(()), 'op_buffer_store_dwordx3', STORE, ['offen'], 'buffer_store_dwordx3')
    assert str(G.Unsupported('buffer_store_dwordx3')) == 'buffer_store_dwordx3'

    s = stage()
    s.op_buffer_store_dwordx3(STORE, ['offen', 'offset:16'])
    assert s.lines == ['a = ud0 + v1 + 16;', 'u0.Store3(a, uint3(v4, v5, v6));'], s.lines
    assert s.pipe.uds == [('uavbase', 0)] and s.pipe.uavs == {(('table', 0), 0x20): 0}, (s.pipe.uds, s.pipe.uavs)
    s = stage()
    s.exec_known = False
    s.op_buffer_store_dword(['v4', 'v1', 's[16:19]', 's3'], ['offen'])
    assert s.lines == ['a = ud0 + v1 + s3 + 0;', 'if (exec & 1) u0.Store(a, v4);'], s.lines
    s = stage()
    s.op_buffer_store_dwordx2(['v[4:5]', 'off', 's[16:19]', '0x40'], [])
    assert s.lines == ['a = ud0 + 64;', 'u0.Store2(a, uint2(v4, v5));'], s.lines
    print('store: feature off refused as before; address with uavbase, exec guard, literal soffset folded')

    refused(stage(), 'op_buffer_store_dwordx3', STORE, ['idxen'], 'modifier idxen')
    refused(stage(), 'op_buffer_store_dwordx3', STORE, ['offen', 'tfe'], 'modifier tfe')
    refused(stage(source=('vbtab',)), 'op_buffer_store_dwordx3', STORE, ['offen'], 'through vbtab')
    s = stage()
    s.op_buffer_load_dword(['v8', 'v1', 's[16:19]', '0'], ['offen'])
    refused(s, 'op_buffer_store_dwordx3', STORE, ['offen'], 'read as an SRV and stored to as a UAV')
    s = stage()
    s.op_buffer_store_dwordx3(STORE, ['offen'])
    refused(s, 'op_buffer_load_dword', ['v8', 'v1', 's[16:19]', '0'], ['offen'], 'read as an SRV and stored to as a UAV')
    pipe = Pipe(('uav',))
    for n in range(8):
        pipe.uav((('table', 0), 0x100 * (n + 1)))
    refused(stage(pipe=pipe), 'op_buffer_store_dwordx3', ['v[4:6]', 'v1', 's[16:19]', '0'], ['offen'], 'more than 8 UAVs')
    print('store refusals: idxen, tfe, vertex buffer, SRV+UAV both orders, ninth UAV')

    s = stage()
    s.op_buffer_store_dwordx3(STORE, ['offen'])
    p = s.pipe
    p.resource((('table', 1), 0), 'buf')
    fake = SimpleNamespace(uds=p.uds, resources=p.resources, uavs=p.uavs, heapvars=[], samplers={},
                           uses_sampler_heap=False, params=[], ps=SimpleNamespace(exports={'mrt0': 1}))
    manifest = G.Pipeline.manifest(fake).split('\n')
    assert manifest[:5] == ['x2w-manifest 1', 'ud 1', 'udsrc 0 uavbase 0', 'srv 0 buf table 1 0', 'uav 0 buf table 0 32'], manifest
    common = SimpleNamespace(helper_text='', uds=p.uds, resources=p.resources, uavs=p.uavs, samplers={}, heapvars=[],
                             uses_sampler_heap=False)
    assert 'RWByteAddressBuffer u0 : register(u0);' in G.Pipeline.common(common)
    print('manifest: uav line after the srv lines, uavbase entry; RWByteAddressBuffer declared')


if __name__ == '__main__':
    main()
