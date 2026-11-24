"""buffer_load_format_* through a descriptor-table entry, behind X2W_DRAW_FEATURES=bufword3 (tools/startup/gfx2hlsl.py).

The format and DST_SEL are in the descriptor's word 3, which the draw runtime passes as `udsrc <n> bufword3 <srv>`
(draw-rt RUNTIME_DONE.md). Checked here, on synthetic register states:
1. feature off: refused with the old message (the op name), so --requests output does not change;
2. feature on: idxen address index * stride + offset, one x2w_fmt_load per destination with the word-3 constant;
   a root or vertex-buffer source (word 3 is 0 there) and an unknown modifier are refused;
3. BUF_FORMATS covers exactly the buffer FORMAT codes gfx10_view_words.h can synthesize (list copied below from its
   gfx10_buffer_format cases with a nonzero element size), and the channel layout of a few formats;
4. with fxc available (X2W_FXC or the SDK default), the helper compiles for ps_5_1 in a minimal shader.
Not checked: the decoded values on a GPU (no readback here); the 0 / 0001 fills are assumptions.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
import gfx2hlsl as G  # noqa: E402

# gfx10_view_words.h gfx10_buffer_format: codes with a nonzero element size (the buffer-capable ones)
HEADER_BUFFER_CODES = {77, 75, 76, 74, 72, 73, 71, 65, 69, 66, 70, 64, 62, 63, 50, 54, 36, 56, 60, 57, 61,
                       29, 23, 27, 24, 28, 22, 20, 21, 14, 18, 15, 19, 13, 7, 11, 8, 12, 1, 5, 2, 6}


class Pipe:
    sampler_heap = False
    root = {'params': [], 'flags': 0}

    def __init__(self, features):
        self.features = frozenset(features)
        self.resources, self.uds, self.modified = {}, [], set()

    def resource(self, key, kind):
        r = self.resources.setdefault(key, dict(index=len(self.resources), kind=kind))
        return 'r%d' % r['index']

    def ud(self, source):
        if source not in self.uds:
            self.uds.append(source)
        return self.uds.index(source)


def stage(features, source=('table', 0)):
    s = G.Stage(Pipe(features), 'ps', b'')
    s.lines = []
    for i in range(4):
        s.sym[16 + i] = ('desc', source, 0x20, i)
    s.pc = 0
    return s


def main():
    try:
        stage(()).op_buffer_load_format_x(['v2', 'v1', 's[16:19]', '0'], ['idxen'])
    except G.Unsupported as error:
        assert str(error) == 'buffer_load_format_x', str(error)
    else:
        raise AssertionError('accepted with the feature off')

    s = stage(('bufword3',))
    s.op_buffer_load_format_xy(['v[2:3]', 'v1', 's[16:19]', '0'], ['idxen', 'offset:8'])
    assert s.pipe.uds == [('stride', 0), ('bufword3', 0)], s.pipe.uds
    assert s.lines[0] == 'a = v1 * ud0 + 8;', s.lines
    assert s.lines[1:] == ['v2 = x2w_fmt_load(r0, a, ud1, 0u);', 'v3 = x2w_fmt_load(r0, a, ud1, 1u);'], s.lines
    for source, mods, want in ((('root', 2), ['offen'], 'through root'), (('vbtab',), ['idxen'], 'through vbtab'),
                               (('table', 0), ['tfe'], 'modifier tfe')):
        try:
            stage(('bufword3',), source).op_buffer_load_format_x(['v2', 'v1', 's[16:19]', '0'], mods)
        except G.Unsupported as error:
            assert want in str(error), (want, str(error))
        else:
            raise AssertionError('accepted: %s %s' % (source, mods))
    print('format load: feature off refused as before; idxen address and word-3 constant; root/vb/tfe refused')

    assert set(G.BUF_FORMATS) == HEADER_BUFFER_CODES, sorted(set(G.BUF_FORMATS) ^ HEADER_BUFFER_CODES)
    ch = G.buf_format_channel
    assert ch((8, 8, 8, 8), 'unorm', 2) == 'U(float(((ldu(b, a + 0u) >> 16u) & 0xffu)) / 255.0)'
    assert ch((10, 10, 10, 2), 'unorm', 3) == 'U(float(((ldu(b, a + 0u) >> 30u) & 0x3u)) / 3.0)'
    assert ch((11, 11, 10), 'float', 2) == 'U(f16tof32((((ldu(b, a + 0u) >> 22u) & 0x3ffu) << 5u)))'
    assert ch((11, 11, 10), 'float', 0) == 'U(f16tof32((((ldu(b, a + 0u) >> 0u) & 0x7ffu) << 4u)))'
    assert ch((16,) * 4, 'float', 3) == 'U(f16tof32(((ldu(b, a + 4u) >> 16u) & 0xffffu)))'
    assert ch((32,) * 4, 'sint', 2) == 'b.Load(a + 8u)'
    assert ch((8,), 'sint', 0) == 'uint(int(((ldu(b, a + 0u) >> 0u) & 0xffu) << 24u) >> 24u)'
    assert ch((16, 16), 'snorm', 1) == 'U(max(float(int(((ldu(b, a + 0u) >> 16u) & 0xffffu) << 16u) >> 16u) / 32767.0, -1.0))'
    print('formats: %d codes, the header buffer set exactly; channel layouts as listed' % len(G.BUF_FORMATS))

    fxc = G.find_fxc()
    if not fxc:
        print('fxc not found: compile step skipped')
        return
    src = '\n'.join(['#define U(x) asuint(x)',
                     'uint ldu(ByteAddressBuffer b, uint a) { uint k = (a & 3) * 8, lo = b.Load(a & ~3u); if (k == 0) return lo; '
                     'return (lo >> k) | (b.Load((a & ~3u) + 4) << (32 - k)); }',
                     G.HELPERS['x2w_fmt_load'],
                     'cbuffer entry : register(b0) { uint ud0; uint ud1; };',
                     'ByteAddressBuffer r0 : register(t0);',
                     'float4 main(float4 p : SV_Position) : SV_Target { uint a = uint(p.x) * 4u;',
                     '  return float4(asfloat(x2w_fmt_load(r0, a, ud0, 0u)), asfloat(x2w_fmt_load(r0, a, ud0, 1u)),',
                     '                asfloat(x2w_fmt_load(r0, a, ud1, 2u)), asfloat(x2w_fmt_load(r0, a, ud1, 3u))); }'])
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'f.hlsl')
        open(path, 'w').write(src)
        r = subprocess.run([fxc, '/nologo', '/T', 'ps_5_1', '/E', 'main', '/Fo', os.path.join(tmp, 'f.cso'), path],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[-2000:]
    print('helper compiles with fxc ps_5_1')


if __name__ == '__main__':
    main()
