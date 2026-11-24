"""SPI_PS_INPUT_ENA and PA_CL_VS_OUT_CNTL read from the runtime's req_<key>.vsregs dump (state block A).

Synthetic requests only. The dump offsets (+0x58 u16 ENA, +0x98 u32 PA_CL_VS_OUT_CNTL) were measured on run-84
dumps; ADDR is taken equal to ENA. Checks:
- no dump: results unchanged (an entry read is refused; a stage without entry reads translates byte-identically
  with and without a dump that carries no relevant bits);
- plain dump with ENA 0x8002: the packed position is lowered at v2;
- an explicit .psin takes precedence over the dump (0x8002/0x8003 -> refused although the dump says 0x8002);
- only per-state dumps: used when they agree, ignored (refused as unknown) when they disagree;
- a too-short dump is ignored;
- PA_CL_VS_OUT_CNTL 0x01400303 from the dump lowers pos1 to SV_ClipDistance0; an explicit .vsout wins.
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gfx2hlsl as G  # noqa: E402
from startup_image_3d_check import VS_WORDS, root_blob  # noqa: E402
from startup_clip_distance_check import VS as CLIP_VS  # noqa: E402
from startup_spi_input_check import pixel_words, packed  # noqa: E402


def dump(ena=0, cntl=0, size=0x184):
    blob = bytearray(size)
    if size >= 0x5a:
        struct.pack_into('<H', blob, 0x58, ena)
    if size >= 0x9c:
        struct.pack_into('<I', blob, 0x98, cntl)
    return bytes(blob)


class Request:
    def __init__(self, tmp, vs, ps):
        self.tmp, self.key = tmp, G.pipeline_key(vs, ps, root_blob())
        self.base = os.path.join(tmp, 'req_%016x' % self.key)
        for ext, blob in (('.a', vs), ('.b', ps), ('.root', root_blob())):
            open(self.base + ext, 'wb').write(blob)

    def side(self, ext, data):
        mode = 'wb' if isinstance(data, bytes) else 'w'
        with open(self.base + ext, mode, **({} if mode == 'wb' else {'newline': '\n'})) as f:
            f.write(data)

    def drop(self, *exts):
        for ext in exts:
            for p in (self.base + ext, os.path.join(self.tmp, '%016x%s' % (self.key, ext))):
                if os.path.exists(p):
                    os.remove(p)

    def run(self):
        self.drop('.txt', '.fail')
        return G.translate_request(self.tmp, self.key)

    def out(self, ext):
        return open(os.path.join(self.tmp, '%016x%s' % (self.key, ext)), 'rb').read()


def main():
    G.FXC = os.path.join(tempfile.gettempdir(), 'no-such-fxc.exe')  # translation only, no compiler
    with tempfile.TemporaryDirectory() as tmp:
        r = Request(tmp, VS_WORDS, pixel_words(2))
        assert r.run().startswith('unsupported: pixel stage reads entry VGPR v2'), 'no dump: refused as before'
        r.side('.vsregs', dump(0x8002))
        assert r.run() == 'ok' and packed(2).encode() in r.out('.ps.hlsl'), 'ENA from the dump'
        r.side('.psin', 'spi_ps_input 1 00008002 00008003\n')
        assert r.run().startswith('unsupported: pixel stage reads SPI input persp_center'), '.psin wins'
        r.drop('.psin', '.vsregs')
        r.side('.0badf00d.vsregs', dump(0x8002))
        r.side('.12345678.vsregs', dump(0x8002))
        assert r.run() == 'ok', 'agreeing per-state dumps'
        r.side('.12345678.vsregs', dump(0x8080))
        assert r.run().startswith('unsupported: pixel stage reads entry VGPR v2'), 'disagreeing dumps ignored'
        r.drop('.0badf00d.vsregs', '.12345678.vsregs')
        r.side('.vsregs', dump(0x8002, size=0x40))
        assert r.run().startswith('unsupported: pixel stage reads entry VGPR v2'), 'short dump ignored'
        r.drop('.vsregs')

        # A stage that reads no entry VGPR: identical output with no dump and with a dump.
        r2 = Request(tmp, VS_WORDS, struct.pack('<5I', 0x7e000280, 0xf800180f, 0, 0xbf810000, 0xbf810000)[:16])
        assert r2.run() == 'ok'
        before = [r2.out(e) for e in ('.vs.hlsl', '.ps.hlsl', '.txt')]
        r2.side('.vsregs', dump(0x0002, 0))
        assert r2.run() == 'ok' and [r2.out(e) for e in ('.vs.hlsl', '.ps.hlsl', '.txt')] == before, 'byte-identical'

        r3 = Request(tmp, CLIP_VS, pixel_words(2))
        r3.side('.vsregs', dump(0x8002, 0x01400303))
        assert r3.run() == 'ok' and b'SV_ClipDistance0' in r3.out('.vs.hlsl'), 'PA_CL_VS_OUT_CNTL from the dump'
        r3.side('.vsout', 'pa_cl_vs_out_cntl 1 01600303\n')
        assert r3.run().startswith('unsupported: pos1 export with PA_CL_VS_OUT_CNTL 0x01600303'), '.vsout wins'
    print('vsregs state: ENA and PA_CL_VS_OUT_CNTL from the dump; side files win; per-state agreement; '
          'short dump ignored; no-dump output unchanged')


if __name__ == '__main__':
    main()
