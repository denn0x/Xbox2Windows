"""image_sample_c_lz with SQ_TEX_CLAMP_BORDER (6) in sampler word 0 CLAMP_X / CLAMP_Y.

Mesa src/amd/registers/gfx10-rsrc.json (bb59ce45): SQ_IMG_SAMP_WORD0 CLAMP_X [2:0], CLAMP_Y [5:3], CLAMP_Z [8:6];
SQ_IMG_SAMP_WORD3 BORDER_COLOR_PTR [11:0], BORDER_COLOR_TYPE [31:30]. radv_sampler.c maps VK CLAMP_TO_BORDER to
SQ_TEX_CLAMP_BORDER and the static border colours to BORDER_COLOR_TYPE 0 (transparent black), 1 (opaque black),
2 (opaque white); 3 is a border colour table entry.

The fixture is the recorded c_lz stage of startup_image_sample_c_check with its four s_mov_b32 sampler words
replaced. Checks:
- the measured sampler 0x30b6 0x00c00000 0x18500000 0x80000000 (CLAMP_BORDER X/Y, CLAMP_LAST_TEXEL Z, LESS_EQUAL,
  bilinear, MIP_POINT_PRECLAMP, opaque white) lowers to SampleCmpLevelZero, and the smpc manifest line keeps the
  original words (the runtime maps 6 to D3D12 BORDER and word 3's type to the static colour with X2W_SAMPLER_LOD=1);
- border in X only (Y clamp), and border colour types 0 and 1, are accepted.

Controls (refused): BORDER_COLOR_TYPE 3; MIRROR_ONCE_BORDER (7) and CLAMP_HALF_BORDER (4) in X; border with
anisotropy (the rest of the contract still applies). Negative control: pixel_aot's own decoder still refuses
CLAMP_BORDER, so without the translator change the measured sampler is refused.
Compile only; the border texel against the reference is not drawn here.
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

MEASURED = (0x30b6, 0x00c00000, 0x18500000, 0x80000000)


def s_mov(reg, value):
    return struct.pack('<II', 0xbe8003ff | reg << 16, value)


def stage(words):
    code = C.WORDS['lz']
    start = code.index(bytes.fromhex('ff0398be'))  # s_mov_b32 s24, literal
    end = start + 8 + 3 * 4                         # then s_mov_b32 s25, s26, s27 with inline 0
    assert [a for _, _, _, a in G.decode(code[start:end])] == [
        's_mov_b32 s24, 0x8001012', 's_mov_b32 s25, 0', 's_mov_b32 s26, 0', 's_mov_b32 s27, 0']
    return code[:start] + b''.join(s_mov(24 + i, w) for i, w in enumerate(words)) + code[end:]


def ps(words):
    pipe = G.Pipeline(VS_WORDS, stage(words), root_blob(flags=0))
    return pipe, pipe.hlsl_ps()


def refused(words, text):
    try:
        ps(words)
    except G.Unsupported as error:
        assert text in str(error), (hex(words[0]), str(error))
    else:
        raise AssertionError('accepted: %s' % ' '.join('%#x' % w for w in words))


def main():
    pipe, text = ps(MEASURED)
    method, u, v, ref, _, _ = C.lowered(text)
    assert (method, u, v, ref) == ('SampleCmpLevelZero', 'v1', 'v2', 'v0'), (method, u, v, ref)
    assert 'SamplerComparisonState q0 : register(s0);' in text, text
    assert C.manifest_samplers(pipe) == ['smpc 0 0x30b6 0xc00000 0x18500000 0x80000000'], C.manifest_samplers(pipe)
    x_only = (MEASURED[0] & ~0x38 | 2 << 3,) + MEASURED[1:]
    for words in (x_only, MEASURED[:3] + (0,), MEASURED[:3] + (0x40000000,)):
        ps(words)
    refused(MEASURED[:3] + (0xc0000000,), 'border colour from the border colour table (BORDER_COLOR_TYPE 3)')
    refused((MEASURED[0] & ~7 | 7,) + MEASURED[1:], 'requires CLAMP_LAST_TEXEL in X and Y')
    refused((MEASURED[0] & ~7 | 4,) + MEASURED[1:], 'requires CLAMP_LAST_TEXEL in X and Y')
    refused((MEASURED[0] | 1 << 9,) + MEASURED[1:], 'anisotropy')
    try:
        pixel_aot.comparison_sampler_state((MEASURED[0], MEASURED[1], MEASURED[2] & ~(1 << 28), MEASURED[3]))
    except ValueError as error:
        assert 'CLAMP_LAST_TEXEL' in str(error)
    else:
        raise AssertionError('pixel_aot accepts CLAMP_BORDER: the negative control no longer shows the gap')
    print('sample_c_border: measured CLAMP_BORDER opaque-white sampler lowered on c_lz with its words kept; X-only '
          'border and types 0/1 accepted; type 3, MIRROR_ONCE_BORDER, CLAMP_HALF_BORDER and anisotropy refused; '
          'pixel_aot still refuses CLAMP_BORDER (negative control)')


if __name__ == '__main__':
    main()
