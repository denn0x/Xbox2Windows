"""buffer_store_format_* to a typed buffer UAV, behind X2W_DRAW_FEATURES=uav,uavfmt (tools/startup/gfx2hlsl.py).

Measured form (a title 1x1 pass): `buffer_store_format_xyzw v[20:23], v1, s[8:11], 0 idxen`, the descriptor a table
entry. RDNA 2 ISA MUBUF addressing: base + soffset + inst offset + stride * index (+ the offen VGPR); the format is
V# word 3 [18:12]. Checked on synthetic register states:
1. feature off (uav only): refused with the op name, as before;
2. on: `a = ud<uavbase> + v1 * ud<uavstride> + offset`, then x2w_fmt_store(u0, a, ud<uavword3>, data, count),
   the exec guard when exec is not the entry mask, idxen + offen as index * stride + offset VGPR, an SGPR soffset;
3. the helper writes exactly the formats of BUF_FORMATS whose channels are all 32 bits (codes 20-22, 62-64,
   72-77), as many channels as the format has and the instruction gives; a Python model of the helper is run on
   every code 0..127 against that rule. Negative control: a model that also writes 16-bit-channel formats
   (71, 16_16_16_16_FLOAT) differs;
4. refusals: tfe, a vertex-buffer source, a modified descriptor.
ASSUMED (not measured): DST_SEL does not reorder a store; the index is in range.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
import gfx2hlsl as G  # noqa: E402


class Pipe:
    sampler_heap = False
    root = {'params': [], 'flags': 0}
    uav = G.Pipeline.uav
    resource = G.Pipeline.resource

    def __init__(self, features):
        self.features = frozenset(features)
        self.resources, self.uds, self.modified, self.heapvars, self.notes = {}, [], set(), [], set()

    def ud(self, source):
        if source not in self.uds:
            self.uds.append(source)
        return self.uds.index(source)


def stage(features=('uav', 'uavfmt'), source=('table', 1)):
    s = G.Stage(Pipe(features), 'ps', b'')
    s.lines = []
    for i in range(4):
        s.sym[8 + i] = ('desc', source, 0x60, i)
    s.pc = 0
    return s


def refused(s, ops, mods, want):
    try:
        s.op_buffer_store_format_xyzw(ops, mods)
    except G.Unsupported as error:
        assert want in str(error), (want, str(error))
    else:
        raise AssertionError('accepted: %s %s' % (ops, mods))


def helper_model(code, given, wide=False):
    """Channels the helper writes for format code `code` and `given` data components."""
    ranges = re.findall(r'f >= (\d+)u && f <= (\d+)u\) \? (\d)u', G.HELPERS['x2w_fmt_store'])
    n = 0
    for lo, hi, count in ranges:
        if int(lo) <= code <= int(hi):
            n = int(count)
    if wide and code == 71:
        n = 4
    return min(n, given)


def rule(code, given):
    widths = G.BUF_FORMATS.get(code, ((),))[0]
    return min(len(widths), given) if widths and all(w == 32 for w in widths) else 0


def main():
    STORE = ['v[20:23]', 'v1', 's[8:11]', '0']
    refused(stage(('uav',)), STORE, ['idxen'], 'buffer_store_format_xyzw')
    s = stage()
    s.op_buffer_store_format_xyzw(STORE, ['idxen'])
    assert s.lines == ['a = ud0 + v1 * ud2 + 0;', 'x2w_fmt_store(u0, a, ud1, uint4(v20, v21, v22, v23), 4u);'], s.lines
    assert s.pipe.uds == [('uavbase', 0), ('uavword3', 0), ('uavstride', 0)], s.pipe.uds
    s = stage()
    s.exec_known = False
    s.op_buffer_store_format_xy(['v[4:5]', 'v[1:2]', 's[8:11]', 's3'], ['idxen', 'offen', 'offset:8'])
    assert s.lines == ['a = ud0 + s3 + v1 * ud2 + v2 + 8;',
                       'if (exec & 1) x2w_fmt_store(u0, a, ud1, uint4(v4, v5, 0u, 0u), 2u);'], s.lines
    refused(stage(), STORE, ['idxen', 'tfe'], 'modifier tfe')
    refused(stage(source=('vbtab',)), STORE, ['idxen'], 'through vbtab')
    s = stage()
    s.pipe.modified.add((('table', 1), 0x60))
    refused(s, STORE, ['idxen'], 'modified descriptor')
    checked = 0
    for code in range(128):
        for given in (1, 2, 3, 4):
            assert helper_model(code, given) == rule(code, given), (code, given)
            checked += 1
    assert any(helper_model(c, g, wide=True) != rule(c, g) for c in range(128) for g in (1, 2, 3, 4))
    assert 'x2w_fmt_store' in G.HELPERS and 'RWByteAddressBuffer b' in G.HELPERS['x2w_fmt_store']
    print('format_store: feature off refused; idxen / idxen+offen / soffset addresses with uavbase, uavword3, '
          'uavstride; exec guard; helper writes the 32-bit-channel formats only (%d code x count cases); '
          'negative control caught; tfe, vertex buffer, modified descriptor refused' % checked)


if __name__ == '__main__':
    main()
