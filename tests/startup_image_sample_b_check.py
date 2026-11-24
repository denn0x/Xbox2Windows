"""image_sample_b (sample with an LOD bias) in tools/startup/gfx2hlsl.py.

The fixture is the source-built pixel stage of startup_image_sample_l_check.py (v0 = 0.5, v1 = 0.25, v2 = 1.0,
v3 = 2.0; texture from descriptor table 0, all-zero constant sampler) with its MIMG opcode set to 0x25, which the
decoder names image_sample_b (LLVM MIMGInstructions.td, gfx10 IMAGE_SAMPLE_B). The bias is the FIRST address dword,
before the coordinates (LLVM AMDGPUImageDimIntrinsics: sample_b has ExtraAddrArgs [bias]), so v[0:2] is
bias v0, u v1, v v2 and the lowering must be SampleBias(q0, float2(F(v1), F(v2)), F(v0)).

SampleBias needs implicit derivatives like Sample, so it is lowered only where Sample is (straight-line code with
every lane active); elsewhere it is refused, not sampled at level 0 (which would drop the bias).
Controls: before the change the stage refused `image_sample_b` (reproduced by removing the handler); a branch
before the sample and a16 are refused; the plain image_sample of the same words keeps its
Sample lowering.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
from startup_image_sample_l_check import WORDS  # noqa: E402
from startup_sampler_heap_check import VS_WORDS, root_blob  # noqa: E402

LWE = bytes.fromhex('080f92f0')  # the sample_l lwe word of the fixture, at offset 0x30


def program(op, flags=0, prefix_branch=False):
    w = bytearray(WORDS['lwe'])
    i = w.find(LWE)
    assert i == 0x30, i
    w[i:i + 4] = struct.pack('<I', 0xf0900f08 & ~(0x7f << 18) | op << 18 | flags)  # opcode [24:18]
    if prefix_branch:
        # replace s_waitcnt lgkmcnt(0) (0x2c) by s_cbranch_scc0 +0 (to the next instruction): a block boundary
        w[0x2c:0x30] = struct.pack('<I', 0xbf840000)
    return bytes(w)


def ps_text(code):
    return G.Pipeline(VS_WORDS, code, root_blob()).hlsl_ps()


def refused(code, text):
    try:
        ps_text(code)
    except G.Unsupported as error:
        assert text in str(error), (text, str(error))
        return
    raise AssertionError('accepted, expected %r' % text)


def main():
    code = program(0x25)
    asm = G.decode(code)[10][3]
    assert asm == 'image_sample_b v[4:7], v[0:2], s[16:23], s[24:27] dmask:0xf dim:SQ_RSRC_IMG_2D', asm
    ps = ps_text(code)
    assert 't = r0.SampleBias(q0, float2(F(v1), F(v2)), F(v0));' in ps, ps
    for comp, reg in zip('xyzw', range(4, 8)):
        assert 'v%d = U(t.%s);' % (reg, comp) in ps, ps
    # the plain sample of the same address keeps its lowering (coordinates v0, v1)
    plain = ps_text(program(0x20))
    assert 't = r0.Sample(q0, float2(F(v0), F(v1)));' in plain, plain
    # negative control: without the handler the stage refuses as before
    saved = G.Stage.op_image_sample_b
    del G.Stage.op_image_sample_b
    try:
        refused(code, 'image_sample_b')
    finally:
        G.Stage.op_image_sample_b = saved
    refused(program(0x25, prefix_branch=True), 'image_sample_b outside straight-line code')
    assert G.decode(program(0x25, prefix_branch=True))[9][3].startswith('s_cbranch_scc0')
    a16 = bytearray(program(0x25))
    a16[0x34:0x38] = struct.pack('<I', struct.unpack_from('<I', a16, 0x34)[0] | 1 << 30)  # A16 (second dword bit 30)
    assert G.decode(bytes(a16))[10][3].endswith(' a16'), G.decode(bytes(a16))[10][3]
    refused(bytes(a16), 'image_sample_b form')
    print('image_sample_b: SampleBias with the bias from the first address dword; controls: refused without the '
          'handler, after a branch, with a16; plain sample unchanged')


if __name__ == '__main__':
    main()
