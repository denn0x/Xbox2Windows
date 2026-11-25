"""image_sample_c_lz with SQ_IMG_SAMP_WORD2 bit 28 (MIP_POINT_PRECLAMP) set.

Title pixel stages use one comparison sampler in all 1942 refused instances of the round-5 census (455 keys):
words 0x3092 0x00c00000 0x18500000 0 = CLAMP_LAST_TEXEL in X/Y/Z, DEPTH_COMPARE 3, bilinear XY, MIP_FILTER linear,
MAX_LOD 12.0, MIN_LOD 0, MIP_POINT_PRECLAMP 1. MIP_POINT_PRECLAMP (Mesa src/amd/registers/gfx10-rsrc.json
SQ_IMG_SAMP_WORD2 bit 28) only changes how a point mip filter clamps the LOD before rounding; c_lz samples level 0
with MIN_LOD 0, so the translator ignores the bit there (ASSUMED from the field definition, not measured).

Fixtures: the recorded 'lz' and 'c' words of tests/startup_image_sample_c_check.py with the s_mov_b32 s24 / s26
literals patched to the title's words (s25 = MAX_LOD is the c fixture's 0xfff limit or 0).
Checks: the title sampler on c_lz lowers to SampleCmpLevelZero and its smpc manifest line keeps the original
words (the runtime's decode_sampler reads only filters, address modes and the function). Controls (refused): the
same words with bit 29 (ANISO_OVERRIDE) on c_lz; bit 28 on image_sample_c (implicit LOD: the mip filter matters).
Negative control: without the translator change the title sampler is refused (pixel_aot's decoder still refuses
bit 28 directly).
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools' / 'startup'))
sys.path.insert(0, str(HERE.parent / 'tools'))
sys.path.insert(0, str(HERE))
import gfx2hlsl as G  # noqa: E402
import pixel_aot  # noqa: E402
import startup_image_sample_c_check as C  # noqa: E402
from startup_sampler_heap_check import VS_WORDS, root_blob  # noqa: E402

TITLE = (0x3092, 0x00c00000, 0x18500000, 0)
S24 = bytes.fromhex('ff0398be')   # s_mov_b32 s24, literal
S26_ZERO = bytes.fromhex('80039abe')  # s_mov_b32 s26, 0


def patched(name, w0, w2):
    words = C.WORDS[name]
    i = words.index(S24)
    words = words[:i + 4] + struct.pack('<I', w0) + words[i + 8:]
    assert words.count(S26_ZERO) == 1, name
    return words.replace(S26_ZERO, bytes.fromhex('ff039abe') + struct.pack('<I', w2))


def ps(name, w0, w2):
    pipe = G.Pipeline(VS_WORDS, patched(name, w0, w2), root_blob(flags=0))
    return pipe, pipe.hlsl_ps()


def refused(name, w0, w2, text):
    try:
        ps(name, w0, w2)
    except G.Unsupported as error:
        assert text in str(error), (name, str(error))
    else:
        raise AssertionError('accepted: %s %#x' % (name, w2))


def main():
    pipe, text = ps('lz', TITLE[0], TITLE[2])
    method, u, v, ref, _, _ = C.lowered(text)
    assert (method, u, v, ref) == ('SampleCmpLevelZero', 'v1', 'v2', 'v0'), (method, u, v, ref)
    assert 'SamplerComparisonState q0 : register(s0);' in text, text
    assert C.manifest_samplers(pipe) == ['smpc 0 0x3092 0x0 0x18500000 0x0'], C.manifest_samplers(pipe)
    refused('lz', TITLE[0], TITLE[2] | 1 << 29, 'word 2 bits 28-31')
    refused('c', TITLE[0], TITLE[2], 'word 2 bits 28-31')
    try:
        pixel_aot.comparison_sampler_state(TITLE)
    except ValueError as error:
        assert 'word 2 bits 28-31' in str(error)
    else:
        raise AssertionError('pixel_aot accepts bit 28: the negative control no longer shows the gap')
    print('sample_c_preclamp: title comparison sampler (MIP_POINT_PRECLAMP) lowered on c_lz with its words kept; '
          'bit 29 and implicit-LOD controls refused; pixel_aot still refuses bit 28 (negative control)')


if __name__ == '__main__':
    main()
