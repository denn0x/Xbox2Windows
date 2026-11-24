"""Split the #670 shared comparison/ordinary sampler, compile it and check the GPU readback.

Usage: compare_sampler.py <spirv-as> <spirv-val> <spirv-cross> [<dxc> <offscreen_check> hardware|warp]

Without the last three arguments only the SPIR-V and HLSL stages and the rejection controls run.
"""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from spirv_sampler_split import split_comparison_sampler
from spirv_sampler_views import split_sampler_views

def substitute(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


ORDINARY_BINDING = 5
SHARED = Path(__file__).with_name("shared_sampler.spvasm").read_text(encoding="utf-8")
# The same module with arrayed (Texture2DArray) images and a float3 coordinate (u, v, layer 0).
SHARED_ARRAY = Path(__file__).with_name("shared_sampler_array.spvasm").read_text(encoding="utf-8")
# The arrayed module reading layer 1 instead of layer 0 (the coordinate's third component becomes 1).
SHARED_ARRAY_LAYER1 = SHARED_ARRAY.replace(
    "%15 = OpConstantComposite %v3float %float_0_25 %float_0_25 %float_0",
    "%float_1 = OpConstant %float 1\n         %15 = OpConstantComposite %v3float %float_0_25 %float_0_25 %float_1")
# The base module with valueTexture as a Texture3D: the depth texture stays 2D (comparison on 3D does not
# exist), and the ordinary sample reads a float3 coordinate (0.25, 0.25, 0.75), the second of two slices.
SHARED_VOLUME = SHARED
for old, new in (
        ("%type_sampled_image = OpTypeSampledImage %type_2d_image\n",
         "%type_sampled_image = OpTypeSampledImage %type_2d_image\n"
         "%type_3d_image = OpTypeImage %float 3D 2 0 0 1 Unknown\n"
         "%_ptr_UniformConstant_type_3d_image = OpTypePointer UniformConstant %type_3d_image\n"
         "%type_sampled_image_3d = OpTypeSampledImage %type_3d_image\n"
         "%v3float = OpTypeVector %float 3\n"
         "%uv3 = OpConstantComposite %v3float %float_0_25 %float_0_25 %float_0_75\n"),
        ("%valueTexture = OpVariable %_ptr_UniformConstant_type_2d_image UniformConstant",
         "%valueTexture = OpVariable %_ptr_UniformConstant_type_3d_image UniformConstant"),
        ("%40 = OpLoad %type_2d_image %valueTexture", "%40 = OpLoad %type_3d_image %valueTexture"),
        ("%42 = OpSampledImage %type_sampled_image %40 %41", "%42 = OpSampledImage %type_sampled_image_3d %40 %41"),
        ("%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
         "%43 = OpImageSampleExplicitLod %v4float %42 %uv3 Lod %float_0")):
    SHARED_VOLUME = substitute(SHARED_VOLUME, old, new)
# Slice 1 of the 3D texture holds 0.3125 at its top-left texel (slice 0 holds 0.375, as the 2D case does).
EXPECTED_VOLUME = [0x3f800000, 0x00000000, 0x3ea00000] + [0xc5c5c5c5] * 13
# The base module whose ordinary sample reads t1's mip 1 (LOD 1.0), and one at LOD 0.75 (point mip filter:
# the nearest mip is 1); both read 0.625 where LOD zero reads 0.375. A third computes the LOD (0.5 + 0.5).
SHARED_LOD1 = substitute(substitute(SHARED, "%float_0 = OpConstant %float 0", "%float_0 = OpConstant %float 0\n%float_1 = OpConstant %float 1"),
                         "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
                         "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_1")
SHARED_LOD075 = substitute(SHARED, "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
                           "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0_75")
SHARED_LOD_COMPUTED = substitute(substitute(SHARED, "%float_0 = OpConstant %float 0",
                                           "%float_0 = OpConstant %float 0\n%float_0_5 = OpConstant %float 0.5"),
                                 "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
                                 "%lod = OpFAdd %float %float_0_5 %float_0_5\n"
                                 "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %lod")
SHARED_VOLUME_LOD1 = substitute(substitute(SHARED_VOLUME, "%float_0 = OpConstant %float 0",
                                          "%float_0 = OpConstant %float 0\n%float_1 = OpConstant %float 1"),
                                "%43 = OpImageSampleExplicitLod %v4float %42 %uv3 Lod %float_0",
                                "%43 = OpImageSampleExplicitLod %v4float %42 %uv3 Lod %float_1")
EXPECTED_LOD1 = [0x3f800000, 0x00000000, 0x3f200000] + [0xc5c5c5c5] * 13
# Gathers (no image operands). The extra constants give components 0..3, an out-of-range and a negative one,
# a float constant and a constant offset; the module is otherwise the base module.
GATHER_TYPES = ("%type_sampled_image = OpTypeSampledImage %type_2d_image\n"
                "%int_1 = OpConstant %int 1\n%int_2 = OpConstant %int 2\n%int_3 = OpConstant %int 3\n"
                "%int_4 = OpConstant %int 4\n%int_m1 = OpConstant %int -1\n"
                "%v2int = OpTypeVector %int 2\n%offset = OpConstantComposite %v2int %int_0 %int_0\n")
ORDINARY_SAMPLE = "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0"
# A depth-comparison gather on the comparison sampler, added before the ordinary part of the base module.
DREF_GATHER = ("%g1 = OpSampledImage %type_sampled_image %32 %33\n"
               "%g2 = OpImageDrefGather %v4float %g1 %15 %float_0_25\n"
               "%g3 = OpCompositeExtract %float %g2 0\n"
               "%g4 = OpAccessChain %_ptr_StorageBuffer_float %results %int_0 %uint_3\n"
               "OpStore %g4 %g3\n")


def gather_module(operation, base=SHARED):
    """The base module with its ordinary sample replaced by the given instruction text."""
    return substitute(substitute(base, "%type_sampled_image = OpTypeSampledImage %type_2d_image\n", GATHER_TYPES),
                      ORDINARY_SAMPLE, operation)


# The ordinary sample becomes an OpImageGather of component 1 on the value texture, and the base module's
# comparison samples stay. A second module also gathers from the depth texture with OpImageDrefGather.
SHARED_GATHER = gather_module("%43 = OpImageGather %v4float %42 %15 %int_1")
SHARED_DREF_GATHER = substitute(substitute(SHARED, "%type_sampled_image = OpTypeSampledImage %type_2d_image\n",
                                           "%type_sampled_image = OpTypeSampledImage %type_2d_image\n"
                                           "%uint_3 = OpConstant %uint 3\n"),
                                "%40 = OpLoad %type_2d_image %valueTexture",
                                DREF_GATHER + "%40 = OpLoad %type_2d_image %valueTexture")
# Both gathers with the ordinary sample's sampler loaded from the plain sampler: nothing is shared, so
# nothing is split and the OpImageDrefGather stays on the comparison sampler.
DREF_GATHER_ONLY = substitute(SHARED_DREF_GATHER, "%41 = OpLoad %type_sampler %cmpSampler",
                              "%41 = OpLoad %type_sampler %plainSampler")
# The only ordinary use is a gather and the comparison uses are samples and a gather: still one mixed sampler.
SHARED_GATHER_BOTH = substitute(SHARED_DREF_GATHER, ORDINARY_SAMPLE, "%43 = OpImageGather %v4float %42 %15 %int_0")
GATHER_NAMES = ("GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha")
# Two mixed samplers (cmpA at binding 2, cmpB at 6): word 0 SampleCmp(cmpA, ref 0.25 vs 0.5) = 1, word 1
# SampleCmp(cmpB, ref 0.875 vs 0.75) = 0, word 2 ordinary A 0.375, word 3 ordinary B 0.875; 12 guards.
SHARED_MULTI = Path(__file__).with_name("shared_sampler_multi.spvasm").read_text(encoding="utf-8")
MULTI_BINDINGS = [5, 7]
EXPECTED_MULTI = [0x3f800000, 0x00000000, 0x3ec00000, 0x3f600000] + [0xc5c5c5c5] * 12
# 0.25 <= 0.5 passes, 0.75 <= 0.5 fails, ordinary sample of 0.375; the other 13 words are guards.
EXPECTED = [0x3f800000, 0x00000000, 0x3ec00000] + [0xc5c5c5c5] * 13
# Layer 1 of the arrayed textures: 0.25 <= 0.875 and 0.75 <= 0.875 both pass, ordinary sample of 0.3125.
EXPECTED_LAYER1 = [0x3f800000, 0x3f800000, 0x3ea00000] + [0xc5c5c5c5] * 13


# ---- Gather fixtures whose four returned values are read back (Windows stage only) -------------------------
# A source-built 2x2 case: t0 is a depth-like R32_FLOAT texture, t1 an R32G32_FLOAT texture. Texels are given
# by (x, y) with y growing downwards; every texel differs in each texture and R differs from G in t1.
#   t0 depth:   (0,0)=0.5    (1,0)=0.75   (0,1)=0.25   (1,1)=1.0
#   t1 (R, G):  (0,0)=(0.125, 0.5)  (1,0)=(0.25, 0.875)  (0,1)=(0.375, 0.0625)  (1,1)=(0.625, 0.75)
# Every value is a binary fraction, so the float bits are exact. B and A of an R32G32 texel read as 0 and 1.
DEPTH_TEXELS = {(0, 0): 0.5, (1, 0): 0.75, (0, 1): 0.25, (1, 1): 1.0}
VALUE_TEXELS = {(0, 0): (0.125, 0.5), (1, 0): (0.25, 0.875), (0, 1): (0.375, 0.0625), (1, 1): (0.625, 0.75)}
# Gather returns x, y, z, w from the texels (0,1), (1,1), (1,0), (0,0): bottom-left, bottom-right, top-right,
# top-left of the footprint (the order D3D12 and Vulkan share). The coordinate (0.5, 0.5) is the corner shared
# by the four texels of the 2x2 texture: 0.5 * 2 - 0.5 = 0.5, whose floor 0 starts the footprint at texel 0.
GATHER_ORDER = ((0, 1), (1, 1), (1, 0), (0, 0))
GATHER_GUARD = 0xc5c5c5c5
# (name, OpImageGather component or None, OpImageDrefGather reference or None). The component picks the
# value texture's channel, the reference is compared with the depth texels by the s2 LESS_EQUAL sampler
# (the comparison passes when reference <= texel). A reference between the texels gives a 0/1 pattern.
GATHER_CASES = (("gather_green", 1, None), ("gather_red", 0, None),
                ("compare_gather_ref_0375", None, 0.375), ("compare_gather_ref_0875", None, 0.875),
                ("gather_green_and_compare_ref_0375", 1, 0.375))


def gather_case_module(component=None, reference=None):
    """The base module (two comparison samples at words 0-1) with gathers that store all four returned values.

    A plain gather replaces the ordinary sample and fills words 2-5; a comparison gather on the depth texture
    follows (words 3-6 beside the ordinary sample at word 2, words 6-9 after a plain gather).
    """
    assert component is not None or reference is not None
    types = ("%type_sampled_image = OpTypeSampledImage %type_2d_image\n"
             "%float_0_5 = OpConstant %float 0.5\n"
             "%gather_uv = OpConstantComposite %v2float %float_0_5 %float_0_5\n"
             + "".join(f"%uint_{word} = OpConstant %uint {word}\n" for word in range(3, 10)))
    if component is not None:
        types += f"%gather_component = OpConstant %int {component}\n"
    if reference is not None:
        types += f"%gather_ref = OpConstant %float {reference}\n"

    text = substitute(SHARED, "%type_sampled_image = OpTypeSampledImage %type_2d_image\n", types)
    after = ""
    next_word = 3
    if component is not None:
        text = substitute(text, ORDINARY_SAMPLE, "%43 = OpImageGather %v4float %42 %gather_uv %gather_component")
        # Word 2 keeps the base module's extract and store of component 0 of the result; words 3-5 follow.
        after += "".join(f"%pg_e{k} = OpCompositeExtract %float %43 {k}\n"
                         f"%pg_p{k} = OpAccessChain %_ptr_StorageBuffer_float %results %int_0 %uint_{2 + k}\n"
                         f"OpStore %pg_p{k} %pg_e{k}\n" for k in range(1, 4))
        next_word = 6
    if reference is not None:
        after += ("%cg_image = OpSampledImage %type_sampled_image %32 %33\n"
                  "%cg_result = OpImageDrefGather %v4float %cg_image %gather_uv %gather_ref\n")
        after += "".join(f"%cg_e{k} = OpCompositeExtract %float %cg_result {k}\n"
                         f"%cg_p{k} = OpAccessChain %_ptr_StorageBuffer_float %results %int_0 %uint_{next_word + k}\n"
                         f"OpStore %cg_p{k} %cg_e{k}\n" for k in range(4))
    return substitute(text, "OpStore %45 %44", "OpStore %45 %44\n" + after)


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def model_gather(component):
    """The four words of a plain gather: channel `component` of the value texels in GATHER_ORDER."""
    return [float_bits((*VALUE_TEXELS[texel], 0.0, 1.0)[component]) for texel in GATHER_ORDER]


def model_gather_cmp(reference):
    """The four words of a comparison gather: 1.0 where reference <= depth texel (LESS_EQUAL), else 0.0."""
    return [float_bits(1.0 if reference <= DEPTH_TEXELS[texel] else 0.0) for texel in GATHER_ORDER]


def model_words(component=None, reference=None):
    """All 16 readback words of a gather case from its parameters, without looking at any module."""
    words = [GATHER_GUARD] * 16
    words[0] = float_bits(1.0 if 0.25 <= DEPTH_TEXELS[(0, 0)] else 0.0)
    words[1] = float_bits(1.0 if 0.75 <= DEPTH_TEXELS[(0, 0)] else 0.0)
    # The ordinary sample at (0.25, 0.25) point-samples texel (0,0): its R channel.
    words[2] = float_bits(VALUE_TEXELS[(0, 0)][0])
    next_word = 3
    if component is not None:
        words[2:6] = model_gather(component)
        next_word = 6
    if reference is not None:
        words[next_word:next_word + 4] = model_gather_cmp(reference)
    return words


def case_file(expected):
    """What the host reads for the gather variant: the 16 expected words, then t0 (4 floats) and t1 (4 R, G pairs)."""
    texels = [DEPTH_TEXELS[(x, y)] for y in range(2) for x in range(2)]
    texels += [channel for y in range(2) for x in range(2) for channel in VALUE_TEXELS[(x, y)]]
    return struct.pack("<16I", *expected) + struct.pack("<12f", *texels)


def readback_mismatches(expected, actual):
    """The lines the host prints for words that differ (the check the Windows stage makes on the readback)."""
    return [f"word={i} expected={e:08x} actual={a:08x}" for i, (e, a) in enumerate(zip(expected, actual)) if e != a]


def module_parameters(assembly):
    """The component and comparison reference a gather module really contains, read from its text."""
    def constant(name, kind):
        found = re.search(rf"^\s*{re.escape(name)} = OpConstant %{kind} (\S+)$", assembly, re.M)
        assert found, name
        return found[1]
    gather = re.search(r"OpImageGather %v4float %\w+ %gather_uv (%\w+)", assembly)
    compare = re.search(r"OpImageDrefGather %v4float %\w+ %gather_uv (%\w+)", assembly)
    assert re.search(r"^\s*%gather_uv = OpConstantComposite %v2float %float_0_5 %float_0_5$", assembly, re.M)
    assert constant("%float_0_5", "float") == "0.5"
    return (int(constant(gather[1], "int")) if gather else None,
            float(constant(compare[1], "float")) if compare else None)


def gather_module_mismatches(assembly, expected):
    """Expected words against what the module's own gather instructions should read back."""
    component, reference = module_parameters(assembly)
    return readback_mismatches(expected, model_words(component, reference))


def gather_readback_controls():
    # Hand arithmetic. Order x, y, z, w = texels (0,1), (1,1), (1,0), (0,0).
    #   Red   0.375, 0.625, 0.25, 0.125  = 3ec00000 3f200000 3e800000 3e000000
    #   Green 0.0625, 0.75, 0.875, 0.5   = 3d800000 3f400000 3f600000 3f000000
    #   Blue  0, 0, 0, 0 (missing channel)   Alpha 1, 1, 1, 1 (missing channel)
    #   Reference 0.375 <= (0.25, 1.0, 0.75, 0.5): 0, 1, 1, 1      Reference 0.875 <= same: 0, 1, 0, 0
    #   Word 0: 0.25 <= 0.5 passes = 1.0; word 1: 0.75 <= 0.5 fails = 0; ordinary sample at texel (0,0) R = 0.125
    guard = [GATHER_GUARD]
    one, zero = 0x3f800000, 0
    assert model_words(component=1) == [one, zero, 0x3d800000, 0x3f400000, 0x3f600000, 0x3f000000] + guard * 10
    assert model_words(component=0) == [one, zero, 0x3ec00000, 0x3f200000, 0x3e800000, 0x3e000000] + guard * 10
    assert model_words(component=2)[2:6] == [zero] * 4 and model_words(component=3)[2:6] == [one] * 4
    assert model_words(reference=0.375) == [one, zero, 0x3e000000, zero, one, one, one] + guard * 9
    assert model_words(reference=0.875) == [one, zero, 0x3e000000, zero, one, zero, zero] + guard * 9
    assert model_words(1, 0.375) == [one, zero, 0x3d800000, 0x3f400000, 0x3f600000, 0x3f000000,
                                     zero, one, one, one] + guard * 6
    # Every texel differs, in each channel that is read, so a wrong texel or a wrong order shows.
    assert len(set(DEPTH_TEXELS.values())) == 4
    for channel in (0, 1):
        assert len({texel[channel] for texel in VALUE_TEXELS.values()}) == 4
    assert all(texel[0] != texel[1] for texel in VALUE_TEXELS.values())
    assert len(case_file(model_words(component=1))) == 16 * 4 + 12 * 4
    # The comparison patterns are not symmetric: reversing them, swapping rows or swapping columns changes them.
    for reference in (0.375, 0.875):
        pattern = model_gather_cmp(reference)
        for other in (pattern[::-1], [pattern[3], pattern[2], pattern[1], pattern[0]],
                      [pattern[1], pattern[0], pattern[3], pattern[2]]):
            assert other != pattern, reference
    # Each case's module holds the parameters its expectation was modelled from.
    for name, component, reference in GATHER_CASES:
        assembly = gather_case_module(component, reference)
        assert module_parameters(assembly) == (component, reference), name
        assert gather_module_mismatches(assembly, model_words(component, reference)) == [], name
        assert split_comparison_sampler(assembly, ORDINARY_BINDING)[1]["ordinary_binding"] == ORDINARY_BINDING, name
    # Negative controls. A module that gathers component 2 (Blue) or 0 (Red) where Green is expected, and one
    # that compares against another reference, do not match the original expectation: the check names the
    # words and the host would print the same lines.
    green, red = model_words(component=1), model_words(component=0)
    for wrong in (0, 2, 3):
        words = gather_module_mismatches(gather_case_module(component=wrong), green)
        assert [line.split()[0] for line in words] == [f"word={i}" for i in range(2, 6)], (wrong, words)
    assert gather_module_mismatches(gather_case_module(component=0), red) == []
    assert gather_module_mismatches(gather_case_module(component=0), green) == readback_mismatches(green, red)
    assert readback_mismatches(green, red)[0] == "word=2 expected=3d800000 actual=3ec00000"
    for expected in ([*green[:2], *green[2:6][::-1], *green[6:]], [*green[:2], *red[2:6], *green[6:]]):
        assert gather_module_mismatches(gather_case_module(component=1), expected), expected
    low, high = model_words(reference=0.375), model_words(reference=0.875)
    assert gather_module_mismatches(gather_case_module(reference=0.875), low) == [
        "word=5 expected=3f800000 actual=00000000", "word=6 expected=3f800000 actual=00000000"]
    assert gather_module_mismatches(gather_case_module(reference=0.375), high) == [
        "word=5 expected=00000000 actual=3f800000", "word=6 expected=00000000 actual=3f800000"]
    assert gather_module_mismatches(gather_case_module(reference=0.375), low) == []


def rejected(assembly, binding, reason):
    try:
        split_comparison_sampler(assembly, binding)
    except ValueError as error:
        assert reason in str(error), error
    else:
        raise AssertionError(f"accepted invalid input: {reason}")


def multi_controls(directory):
    lowered, mappings = split_comparison_sampler(SHARED_MULTI, MULTI_BINDINGS)
    assert mappings == [
        {"comparison_sampler": "%cmpA", "plain_sampler": "%x2w_plain_sampler_0", "descriptor_set": 0,
         "comparison_binding": 2, "ordinary_binding": 5},
        {"comparison_sampler": "%cmpB", "plain_sampler": "%x2w_plain_sampler_1", "descriptor_set": 0,
         "comparison_binding": 6, "ordinary_binding": 7}], mappings
    lines = lowered.splitlines()
    # Each lowered sampler is its own variable at its own binding, listed in the interface, and each
    # ordinary sample loads its own variable; the comparison loads are untouched.
    for plain, binding, comparison in (("%x2w_plain_sampler_0", 5, "%cmpA"), ("%x2w_plain_sampler_1", 7, "%cmpB")):
        assert f"{plain} = OpVariable %_ptr_UniformConstant_type_sampler UniformConstant" in lines
        assert f"OpDecorate {plain} DescriptorSet 0" in lines and f"OpDecorate {plain} Binding {binding}" in lines
        assert f"{plain}_load_0 = OpLoad %type_sampler {plain}" in lines
        assert plain in next(line for line in lines if line.startswith("OpEntryPoint")).split()
        assert sum(line.endswith(f"%type_sampler {comparison}") for line in lines) == 2, comparison
    # Declaration order decides which sampler takes which binding.
    swapped, swapped_mappings = split_comparison_sampler(SHARED_MULTI, [7, 5])
    assert [m["ordinary_binding"] for m in swapped_mappings] == [7, 5]
    assert "OpDecorate %x2w_plain_sampler_0 Binding 7" in swapped.splitlines()
    # A one-element sequence uses the multi-sampler names; a plain int keeps the single-sampler names.
    single_list, single_mappings = split_comparison_sampler(SHARED, [ORDINARY_BINDING])
    assert single_mappings[0]["plain_sampler"] == "%x2w_plain_sampler_0"
    assert "%x2w_plain_sampler_0_load_0 = OpLoad %type_sampler %x2w_plain_sampler_0" in single_list.splitlines()
    # Free bindings below, between and above the occupied ones are accepted (3 is free in this module).
    assert [m["ordinary_binding"] for m in split_comparison_sampler(SHARED_MULTI, [3, 8])[1]] == [3, 8]

    # Rejections: each must fail with its own message and leave the input module untouched.
    rejected(SHARED_MULTI, 5, "found 2 samplers with mixed uses and was given 1 ordinary bindings")
    rejected(SHARED_MULTI, [5], "found 2 samplers with mixed uses and was given 1 ordinary bindings")
    rejected(SHARED_MULTI, [5, 7, 8], "found 2 samplers with mixed uses and was given 3 ordinary bindings")
    rejected(SHARED, [5, 7], "found 1 samplers with mixed uses and was given 2 ordinary bindings")
    rejected(SHARED_MULTI, [5, 5], "Ordinary sampler bindings must be distinct")
    rejected(SHARED_MULTI, [5, -1], "Ordinary sampler binding must be nonnegative")
    # Collisions with each existing variable's binding, for either sampler (0, 1, 2, 4, 6 are taken).
    for binding in (0, 1, 2, 4, 6):
        rejected(SHARED_MULTI, [binding, 7], f"binding {binding} is occupied in set 0")
        rejected(SHARED_MULTI, [5, binding], f"binding {binding} is occupied in set 0")
    # Reserved IDs: the multi-sampler names are reserved too, in any position.
    for name in ("%x2w_plain_sampler_1", "%x2w_plain_sampler_1_load_0", "%x2w_plain_sampler"):
        rejected(substitute(SHARED_MULTI, "%float_0 = OpConstant %float 0",
                            f"%float_0 = OpConstant %float 0\n{name} = OpConstant %float 0"),
                 MULTI_BINDINGS, "Reserved sampler split ID is already in use")
    # The narrow profile applies to every mixed sampler: a nonzero LOD on cmpB's ordinary sample is refused.
    rejected(substitute(SHARED_MULTI, "%49 = OpImageSampleExplicitLod %v4float %48 %uv_b Lod %float_0",
                        "%49 = OpImageSampleExplicitLod %v4float %48 %uv_b Lod %uv_b"),
             MULTI_BINDINGS, "requires a LOD operand of the sampled float type")
    assert split_comparison_sampler(substitute(
        SHARED_MULTI, "%49 = OpImageSampleExplicitLod %v4float %48 %uv_b Lod %float_0",
        "%49 = OpImageSampleExplicitLod %v4float %48 %uv_b Lod %float_0_25"), MULTI_BINDINGS)[1][1]["ordinary_binding"] == 7
    rejected(substitute(SHARED_MULTI, "%cmpA %cmpB %results", "%cmpA %results"),
             MULTI_BINDINGS, "%cmpB: sampler must be listed in the entry-point interface")
    # Occupancy is per descriptor set: binding 6 is taken in set 0 but free for a sampler moved to set 1.
    other_set = substitute(SHARED_MULTI, "OpDecorate %cmpB DescriptorSet 0", "OpDecorate %cmpB DescriptorSet 1")
    moved = split_comparison_sampler(other_set, [6, 2])[1]
    assert [(m["descriptor_set"], m["ordinary_binding"]) for m in moved] == [(0, 6), (1, 2)], moved
    rejected(other_set, [2, 7], "binding 2 is occupied in set 0")
    rejected(other_set, [5, 6], "binding 6 is occupied in set 1 by %cmpB")
    # Unmixed samplers in the same module are left alone: one mixed sampler plus one comparison-only sampler.
    comparison_only = substitute(SHARED_MULTI, "%49 = OpImageSampleExplicitLod %v4float %48 %uv_b Lod %float_0",
                                 "%49 = OpImageSampleDrefExplicitLod %float %48 %uv_b %float_0_875 Lod %float_0")
    one_text, one = split_comparison_sampler(comparison_only, [5])
    assert [m["comparison_sampler"] for m in one] == ["%cmpA"] and "%x2w_plain_sampler_1" not in one_text, one
    assert split_comparison_sampler(comparison_only, 5)[1]["ordinary_binding"] == 5

    tool = [sys.executable, "-B", str(ROOT / "tools" / "spirv_sampler_split.py")]
    source = directory / "shared_multi.spvasm"
    source.write_text(SHARED_MULTI, encoding="utf-8")
    target = directory / "lowered_multi_cli.spvasm"
    run = subprocess.run([*tool, source, target, "--ordinary-binding", "5", "--ordinary-binding", "7"],
                         capture_output=True, text=True, check=True)
    assert run.stdout.splitlines() == [
        "comparison_sampler=%cmpA plain_sampler=%x2w_plain_sampler_0 descriptor_set=0 comparison_binding=2 "
        "ordinary_binding=5",
        "comparison_sampler=%cmpB plain_sampler=%x2w_plain_sampler_1 descriptor_set=0 comparison_binding=6 "
        "ordinary_binding=7"], run.stdout
    assert target.read_text(encoding="utf-8") == lowered
    for arguments, reason in ((["--ordinary-binding", "5"], "found 2 samplers"),
                              (["--ordinary-binding", "5", "--ordinary-binding", "5"], "must be distinct"),
                              (["--ordinary-binding", "5", "--ordinary-binding", "6"], "binding 6 is occupied")):
        failed = subprocess.run([*tool, source, directory / "refused.spvasm", *arguments], capture_output=True, text=True)
        assert failed.returncode == 1 and reason in failed.stderr, (arguments, failed)
        assert not (directory / "refused.spvasm").exists()
    return lowered, swapped


def with_image_dim(module, sampled_image, dim):
    """Point one OpSampledImage line at a float32 image of another dimension (for negative controls only)."""
    old = "%type_sampled_image = OpTypeSampledImage %type_2d_image\n"
    types = (f"{old}%type_image_{dim} = OpTypeImage %float {dim} 2 0 0 1 Unknown\n"
             f"%type_sampled_image_{dim} = OpTypeSampledImage %type_image_{dim}\n")
    return substitute(substitute(module, old, types), sampled_image,
                      sampled_image.replace("%type_sampled_image ", f"%type_sampled_image_{dim} "))


def gather_controls():
    mapping = {"descriptor_set": 0, "comparison_binding": 2, "ordinary_binding": ORDINARY_BINDING}
    # The ordinary gather moves to the plain sampler like an ordinary sample: one new load, the comparison
    # sampler keeps its two loads, and the module still has the gather on the sampled image built from it.
    lowered, got = split_comparison_sampler(SHARED_GATHER, ORDINARY_BINDING)
    assert got == mapping, got
    lines = lowered.splitlines()
    assert "%x2w_plain_sampler_0 = OpLoad %type_sampler %x2w_plain_sampler" in lines
    assert "%42 = OpSampledImage %type_sampled_image %40 %x2w_plain_sampler_0" in lines
    assert sum(line.endswith("%type_sampler %cmpSampler") for line in lines) == 2  # both depth loads
    assert "%43 = OpImageGather %v4float %42 %15 %int_1" in [line.strip() for line in lines]
    # Every component constant from 0 to 3 is accepted, signed or unsigned.
    for component in ("%int_0", "%int_1", "%int_2", "%int_3", "%uint_1"):
        assert split_comparison_sampler(gather_module(f"%43 = OpImageGather %v4float %42 %15 {component}"),
                                        ORDINARY_BINDING)[1] == mapping, component
    # The comparison gather stays on the comparison sampler: a module that also has an ordinary sample is
    # split for that sample only, and one without an ordinary use is returned unchanged.
    dref_lowered, got = split_comparison_sampler(SHARED_DREF_GATHER, ORDINARY_BINDING)
    assert got == mapping, got
    dref_lines = [line.strip() for line in dref_lowered.splitlines()]
    assert "%g1 = OpSampledImage %type_sampled_image %32 %33" in dref_lines
    assert "%42 = OpSampledImage %type_sampled_image %40 %x2w_plain_sampler_0" in dref_lines
    assert split_comparison_sampler(DREF_GATHER_ONLY, ORDINARY_BINDING) == (DREF_GATHER_ONLY, None)
    # Both kinds together on one sampler: an ordinary gather plus a comparison gather is a mixed sampler.
    assert split_comparison_sampler(SHARED_GATHER_BOTH, ORDINARY_BINDING)[1] == mapping

    # Refusals, each with its own message. Image operands of any kind are refused on either gather.
    for operands in ("ConstOffset %offset", "Offset %offset", "Lod %float_0", "Bias %float_0", "Sample %int_0",
                     "MinLod %float_0", "Grad %15 %15"):
        rejected(gather_module(f"%43 = OpImageGather %v4float %42 %15 %int_1 {operands}"), ORDINARY_BINDING,
                 "OpImageGather with image operands is not supported")
        rejected(substitute(SHARED_DREF_GATHER, "%g2 = OpImageDrefGather %v4float %g1 %15 %float_0_25",
                            f"%g2 = OpImageDrefGather %v4float %g1 %15 %float_0_25 {operands}"),
                 ORDINARY_BINDING, "OpImageDrefGather with image operands is not supported")
    # A gather is only the D3D12 forms: a 1D or 3D gathered image is refused, ordinary or comparison. Only
    # the gathered sampled image gets the other dimension, so the gather is what is refused.
    gather_only = "only supported on Texture2D, Texture2DArray, TextureCube and TextureCubeArray"
    for dim in ("1D", "3D"):
        rejected(with_image_dim(SHARED_GATHER, "%42 = OpSampledImage %type_sampled_image %40 %41", dim),
                 ORDINARY_BINDING, "OpImageGather is " + gather_only)
        rejected(with_image_dim(SHARED_DREF_GATHER, "%g1 = OpSampledImage %type_sampled_image %32 %33", dim),
                 ORDINARY_BINDING, "OpImageDrefGather is " + gather_only)
    # A non-float32 sampled type is outside the profile for a gather as for any other sampling.
    rejected(substitute(SHARED_GATHER, "OpTypeImage %float 2D 2 0 0 1 Unknown", "OpTypeImage %uint 2D 2 0 0 1 Unknown"),
             ORDINARY_BINDING, "only float32 Texture1D")
    # The component must be an integer OpConstant from 0 to 3: out of range, negative, computed, float, undefined.
    component = "OpImageGather requires an integer constant component from 0 to 3"
    for operand in ("%int_4", "%int_m1", "%float_0", "%undefined_component", "%component"):
        text = gather_module(f"%43 = OpImageGather %v4float %42 %15 {operand}")
        if operand == "%component":
            text = substitute(text, "%43 = OpImageGather", "%component = OpIAdd %int %int_0 %int_1\n"
                                                           "%43 = OpImageGather")
        rejected(text, ORDINARY_BINDING, component)
    # One sampled image may not hold both kinds (a gather and a comparison sample on %42).
    rejected(substitute(SHARED_GATHER, "%43 = OpImageGather %v4float %42 %15 %int_1",
                        "%43 = OpImageGather %v4float %42 %15 %int_1\n"
                        "%dup = OpImageSampleDrefExplicitLod %float %42 %15 %float_0_25 Lod %float_0"),
             ORDINARY_BINDING, "one sampled image is used for both sampling kinds")


def controls(directory):
    gather_controls()
    gather_readback_controls()
    lowered, mapping = split_comparison_sampler(SHARED, ORDINARY_BINDING)
    assert mapping == {"descriptor_set": 0, "comparison_binding": 2, "ordinary_binding": ORDINARY_BINDING}, mapping
    # A Texture3D value image with a float3 coordinate splits like the 2D module: same mapping, only the
    # ordinary load moves, the comparison sampler keeps its two loads and the 2D depth texture is untouched.
    volume_lowered, volume_mapping = split_comparison_sampler(SHARED_VOLUME, ORDINARY_BINDING)
    assert volume_mapping == mapping, volume_mapping
    assert "%x2w_plain_sampler_0 = OpLoad %type_sampler %x2w_plain_sampler" in volume_lowered.splitlines()
    assert "OpTypeImage %float 3D 2 0 0 1 Unknown" in volume_lowered
    # The ordinary kind takes any LOD of the sampled float type: a constant or a computed value.
    for module in (SHARED_LOD1, SHARED_LOD075, SHARED_LOD_COMPUTED, SHARED_VOLUME_LOD1):
        assert split_comparison_sampler(module, ORDINARY_BINDING)[1] == mapping, module
    # The arrayed variant splits to the same mapping and only the ordinary load moves.
    array_lowered, array_mapping = split_comparison_sampler(SHARED_ARRAY, ORDINARY_BINDING)
    assert array_mapping == mapping, array_mapping
    assert "%x2w_plain_sampler_0 = OpLoad %type_sampler %x2w_plain_sampler" in array_lowered.splitlines()
    assert "OpTypeImage %float 2D 2 1 0 1 Unknown" in array_lowered
    # Modules without a mixed standalone sampler come back byte-for-byte, whatever else they contain.
    separated = substitute(SHARED, "%41 = OpLoad %type_sampler %cmpSampler", "%41 = OpLoad %type_sampler %plainSampler")
    separated_lod = substitute(separated, "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
                               "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0_25")
    table = Path(__file__).with_name("sampler.spvasm").read_text(encoding="utf-8")  # #676 indexed table
    for name, module in (("already separated", separated), ("unrelated nonzero LOD", separated_lod),
                         ("indexed sampler table", table), ("split sampler views", split_sampler_views(table))):
        assert split_comparison_sampler(module, ORDINARY_BINDING) == (module, None), name

    # An unrelated ordinary sample at nonzero LOD on another sampler does not block the split.
    unrelated = substitute(SHARED, "%44 = OpCompositeExtract %float %43 0",
                           "%44 = OpCompositeExtract %float %43 0\n"
                           "%u1 = OpLoad %type_sampler %plainSampler\n"
                           "%u2 = OpSampledImage %type_sampled_image %40 %u1\n"
                           "%u3 = OpImageSampleExplicitLod %v4float %u2 %15 Lod %float_0_25")
    split, mapping = split_comparison_sampler(unrelated, ORDINARY_BINDING)
    assert mapping["ordinary_binding"] == ORDINARY_BINDING
    assert "%u2 = OpSampledImage %type_sampled_image %40 %u1" in split

    # Unsupported uses of the sampler being split are still rejected.
    for binding in (2, 3, 4):
        rejected(SHARED, binding, f"binding {binding} is occupied in set 0")
    # A binding declared without a DescriptorSet decoration still occupies set 0.
    rejected(substitute(SHARED, "OpDecorate %plainSampler DescriptorSet 0\n", ""), 3,
             "binding 3 is occupied in set 0 by %plainSampler")
    # The ordinary sample may not reuse the comparison's sampled image.
    rejected(substitute(SHARED, "%43 = OpImageSampleExplicitLod %v4float %42 ",
                        "%43 = OpImageSampleExplicitLod %v4float %34 "),
             ORDINARY_BINDING, "one sampled image is used for both sampling kinds")
    # The depth-comparison kind stays at constant LOD zero, whatever the ordinary sample does.
    rejected(substitute(SHARED, "%float_0_75 Lod %float_0", "%float_0_75 Lod %float_0_25"),
             ORDINARY_BINDING, "OpImageSampleDrefExplicitLod requires a constant LOD of zero")
    rejected(substitute(SHARED_LOD1, "%float_0_75 Lod %float_0", "%float_0_75 Lod %float_1"),
             ORDINARY_BINDING, "OpImageSampleDrefExplicitLod requires a constant LOD of zero")
    rejected(substitute(SHARED_LOD_COMPUTED, "%float_0_75 Lod %float_0", "%float_0_75 Lod %lod"),
             ORDINARY_BINDING, "OpImageSampleDrefExplicitLod requires a constant LOD of zero")
    # An ordinary LOD that is not of the sampled float type (a float2 constant, an undefined ID) is refused.
    for operand in ("%15", "%undefined_lod"):
        rejected(substitute(SHARED, "%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod %float_0",
                            f"%43 = OpImageSampleExplicitLod %v4float %42 %15 Lod {operand}"),
                 ORDINARY_BINDING, "OpImageSampleExplicitLod requires a LOD operand of the sampled float type")
    rejected(substitute(SHARED, "%33 = OpLoad %type_sampler %cmpSampler",
                        "%copy = OpCopyObject %_ptr_UniformConstant_type_sampler %cmpSampler\n"
                        "%33 = OpLoad %type_sampler %cmpSampler"),
             ORDINARY_BINDING, "only direct loads are supported")
    rejected(substitute(SHARED, "%33 = OpLoad %type_sampler %cmpSampler",
                        "%33 = OpLoad %type_sampler %cmpSampler\n"
                        "%loaded_copy = OpCopyObject %type_sampler %33"),
             ORDINARY_BINDING, "loaded sampler feeds OpCopyObject")

    # The remaining profile diagnostics.
    rejected(SHARED, -1, "Ordinary sampler binding must be nonnegative")
    rejected(substitute(SHARED, "OpEntryPoint GLCompute", "OpEntryPoint Fragment"),
             ORDINARY_BINDING, "requires a single GLCompute entry point")
    # Multisampled images, and 3D images that are multisampled or arrayed, stay outside the profile.
    profile = ("only float32 Texture1D, Texture2D, TextureCube and non-arrayed Texture3D sampling "
               "(including the array forms of the first three) is supported")
    for image in ("OpTypeImage %float 2D 2 0 1 1 Unknown",):
        rejected(substitute(SHARED, "OpTypeImage %float 2D 2 0 0 1 Unknown", image),
                 ORDINARY_BINDING, profile)
    for image in ("OpTypeImage %float 3D 2 0 1 1 Unknown", "OpTypeImage %float 3D 2 1 0 1 Unknown"):
        rejected(substitute(SHARED_VOLUME, "OpTypeImage %float 3D 2 0 0 1 Unknown", image),
                 ORDINARY_BINDING, profile)
    # Depth comparison on a 3D image is refused (the depth texture becomes 3D here).
    rejected(substitute(SHARED, "OpTypeImage %float 2D 2 0 0 1 Unknown", "OpTypeImage %float 3D 2 0 0 1 Unknown"),
             ORDINARY_BINDING, "depth-comparison sampling is not supported on Texture3D")
    rejected(substitute(SHARED_ARRAY, "OpTypeImage %float 2D 2 1 0 1 Unknown",
                        "OpTypeImage %float 2D 2 1 1 1 Unknown"),
             ORDINARY_BINDING, profile)
    rejected(substitute(SHARED, " %cmpSampler %plainSampler %results", " %plainSampler %results"),
             ORDINARY_BINDING, "sampler must be listed in the entry-point interface")
    rejected(substitute(SHARED, "%float_0 = OpConstant %float 0",
                        "%float_0 = OpConstant %float 0\n"
                        "%x2w_plain_sampler_0 = OpConstant %float 0"),
             ORDINARY_BINDING, "Reserved sampler split ID is already in use")

    # Occupancy is per descriptor set: binding 3 is taken in set 0 and free in set 1.
    other_set = substitute(SHARED, "OpDecorate %cmpSampler DescriptorSet 0",
                           "OpDecorate %cmpSampler DescriptorSet 1")
    other_lowered, other_mapping = split_comparison_sampler(other_set, 3)
    assert other_mapping == {"descriptor_set": 1, "comparison_binding": 2, "ordinary_binding": 3}, other_mapping
    assert "OpDecorate %x2w_plain_sampler DescriptorSet 1" in other_lowered.splitlines()
    assert "OpDecorate %x2w_plain_sampler Binding 3" in other_lowered.splitlines()

    tool = [sys.executable, "-B", str(ROOT / "tools" / "spirv_sampler_split.py")]
    source = directory / "shared.spvasm"
    source.write_text(SHARED, encoding="utf-8")
    target = directory / "lowered.spvasm"
    run = subprocess.run([*tool, source, target, "--ordinary-binding", str(ORDINARY_BINDING)],
                         capture_output=True, text=True, check=True)
    assert run.stdout.split() == ["descriptor_set=0", "comparison_binding=2", f"ordinary_binding={ORDINARY_BINDING}"]
    assert target.read_text(encoding="utf-8") == lowered
    occupied = subprocess.run([*tool, source, directory / "occupied.spvasm", "--ordinary-binding", "3"],
                              capture_output=True, text=True)
    assert occupied.returncode == 1 and "occupied" in occupied.stderr, occupied
    assert not (directory / "occupied.spvasm").exists()
    return lowered


def check_gather_hlsl(source, gather, comparison_gather=None):
    """The ordinary gather reads the plain sampler at s5; the comparison calls stay on the one at s2."""
    comparison = re.search(r"SamplerComparisonState\s+(\w+)\s*:\s*register\(s2\b", source)
    ordinary = re.search(rf"SamplerState\s+(\w+)\s*:\s*register\(s{ORDINARY_BINDING}\b", source)
    assert comparison and ordinary, source
    assert source.count(f"SampleCmpLevelZero({comparison[1]},") == 2, source
    assert source.count(f"{gather}({ordinary[1]},") == 1, source
    assert not re.search(rf"Gather(Red|Green|Blue|Alpha)\({comparison[1]},", source), source
    if comparison_gather:
        assert source.count(f"GatherCmp({comparison[1]},") == 1 and "GatherCmp(" + ordinary[1] not in source, source


def detected(check, *arguments):
    try:
        check(*arguments)
    except AssertionError:
        return
    raise AssertionError("a gather left on the comparison sampler was not detected")


# Modules with a comparison gather. Their HLSL needs GatherCmp, which older SPIRV-Cross refuses to write.
COMPARISON_GATHER_MODULES = {"lowered_dref_gather", "lowered_gather_both", "unsplit_dref_gather",
                             *(f"lowered_g4_{name}" for name, _, reference in GATHER_CASES if reference is not None)}
NO_GATHER_CMP = "GatherCmp does not exist in HLSL"


def emit(directory, assembler, validator, cross):
    skipped_hlsl = set()
    lowered = controls(directory)
    multi_lowered, multi_swapped = multi_controls(directory)
    outputs = {}
    gathers = [(f"gather_{n}", gather_module(f"%43 = OpImageGather %v4float %42 %15 %int_{n}")) for n in range(4)]
    array_lowered = split_comparison_sampler(SHARED_ARRAY, ORDINARY_BINDING)[0]
    assert SHARED_ARRAY_LAYER1 != SHARED_ARRAY
    layer1_lowered, layer1_mapping = split_comparison_sampler(SHARED_ARRAY_LAYER1, ORDINARY_BINDING)
    assert layer1_mapping["ordinary_binding"] == ORDINARY_BINDING, layer1_mapping
    volume_lowered = split_comparison_sampler(SHARED_VOLUME, ORDINARY_BINDING)[0]
    lod1_lowered = split_comparison_sampler(SHARED_LOD1, ORDINARY_BINDING)[0]
    lod075_lowered = split_comparison_sampler(SHARED_LOD075, ORDINARY_BINDING)[0]
    computed_lowered = split_comparison_sampler(SHARED_LOD_COMPUTED, ORDINARY_BINDING)[0]
    for name, assembly in (("lowered_lod1", lod1_lowered), ("lowered_lod075", lod075_lowered),
                           ("lowered_lod_computed", computed_lowered),("shared", SHARED), ("lowered", lowered),
                           ("shared_volume", SHARED_VOLUME), ("lowered_volume", volume_lowered),
                           ("shared_array", SHARED_ARRAY), ("lowered_array", array_lowered),
                           ("lowered_array_layer1", layer1_lowered),
                           ("shared_multi", SHARED_MULTI), ("lowered_multi", multi_lowered),
                           ("lowered_multi_swapped", multi_swapped),
                           ("shared_gather", SHARED_GATHER),
                           ("lowered_gather", split_comparison_sampler(SHARED_GATHER, ORDINARY_BINDING)[0]),
                           ("lowered_dref_gather", split_comparison_sampler(SHARED_DREF_GATHER, ORDINARY_BINDING)[0]),
                           ("lowered_gather_both", split_comparison_sampler(SHARED_GATHER_BOTH, ORDINARY_BINDING)[0]),
                           ("unsplit_dref_gather", DREF_GATHER_ONLY),
                           *((f"lowered_{name}", split_comparison_sampler(module, ORDINARY_BINDING)[0])
                             for name, module in gathers),
                           *((f"lowered_g4_{name}",
                              split_comparison_sampler(gather_case_module(component, reference), ORDINARY_BINDING)[0])
                             for name, component, reference in GATHER_CASES)):
        text = directory / f"{name}.spvasm"
        text.write_text(assembly, encoding="utf-8")
        module = directory / f"{name}.spv"
        subprocess.run([assembler, "--target-env", "vulkan1.2", text, "-o", module], check=True)
        subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)
        hlsl = directory / f"{name}.hlsl"
        translated = subprocess.run([cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl],
                                    capture_output=True, text=True)
        if translated.returncode and name in COMPARISON_GATHER_MODULES and NO_GATHER_CMP in translated.stderr:
            # Older SPIRV-Cross (for example 2021.01.15) cannot write GatherCmp: the module was still
            # assembled and validated above, only its HLSL checks are skipped.
            skipped_hlsl.add(name)
            print(f"{name}: this SPIRV-Cross cannot write GatherCmp, HLSL checks skipped", flush=True)
            continue
        if translated.returncode:
            raise subprocess.CalledProcessError(translated.returncode, translated.args, translated.stdout, translated.stderr)
        outputs[name] = hlsl
    source = outputs["lowered"].read_text(encoding="utf-8")
    comparison = re.search(r"SamplerComparisonState\s+(\w+)\s*:\s*register\(s2\b", source)
    ordinary = re.search(rf"SamplerState\s+(\w+)\s*:\s*register\(s{ORDINARY_BINDING}\b", source)
    assert comparison and ordinary, source
    assert source.count(f"SampleCmpLevelZero({comparison[1]},") == 2, source
    assert source.count(f"SampleLevel({ordinary[1]},") == 1, source
    # The ordinary LOD survives into SampleLevel (it is not folded to zero); the comparison stays at zero.
    for name, lod in (("lowered_lod1", "1.0f"), ("lowered_lod075", "0.75f"), ("lowered_lod_computed", None)):
        lod_source = outputs[name].read_text(encoding="utf-8")
        assert lod_source.count(f"SampleCmpLevelZero({comparison[1]},") == 2, lod_source
        assert len(re.findall(rf"SampleLevel\({ordinary[1]},[^;]*, {lod or '[^;]*'}\)", lod_source)) == 1, lod_source
    array_source = outputs["lowered_array"].read_text(encoding="utf-8")
    comparison = re.search(r"SamplerComparisonState\s+(\w+)\s*:\s*register\(s2\b", array_source)
    ordinary = re.search(rf"SamplerState\s+(\w+)\s*:\s*register\(s{ORDINARY_BINDING}\b", array_source)
    assert comparison and ordinary and "Texture2DArray" in array_source, array_source
    assert array_source.count(f"SampleCmpLevelZero({comparison[1]},") == 2, array_source
    assert array_source.count(f"SampleLevel({ordinary[1]},") == 1, array_source
    volume_source = outputs["lowered_volume"].read_text(encoding="utf-8")
    comparison = re.search(r"SamplerComparisonState\s+(\w+)\s*:\s*register\(s2\b", volume_source)
    ordinary = re.search(rf"SamplerState\s+(\w+)\s*:\s*register\(s{ORDINARY_BINDING}\b", volume_source)
    assert comparison and ordinary and "Texture3D<" in volume_source, volume_source
    assert volume_source.count(f"SampleCmpLevelZero({comparison[1]},") == 2, volume_source
    assert volume_source.count(f"SampleLevel({ordinary[1]}, float3(0.25f, 0.25f, 0.75f)") == 1, volume_source
    multi_source = outputs["lowered_multi"].read_text(encoding="utf-8")
    for register, kind in ((2, "SamplerComparisonState"), (6, "SamplerComparisonState"), (5, "SamplerState"),
                           (7, "SamplerState")):
        assert re.search(rf"{kind}\s+\w+\s*:\s*register\(s{register}\b", multi_source), (register, multi_source)
    assert len(re.findall(r"SamplerState\s+\w+\s*:\s*register", multi_source)) == 2, multi_source
    assert multi_source.count("SampleCmpLevelZero(") == 2 and multi_source.count("SampleLevel(") == 2, multi_source
    # Gathers: component 1 reads GatherGreen on the plain sampler; the comparison gather stays on the
    # comparison sampler (GatherCmp); the other three components give Red, Blue and Alpha.
    check_gather_hlsl(outputs["lowered_gather"].read_text(encoding="utf-8"), "GatherGreen")
    if "lowered_dref_gather" in outputs:
        check_gather_hlsl(outputs["lowered_dref_gather"].read_text(encoding="utf-8"), "SampleLevel", comparison_gather=True)
    if "lowered_gather_both" in outputs:
        check_gather_hlsl(outputs["lowered_gather_both"].read_text(encoding="utf-8"), "GatherRed", comparison_gather=True)
    for n, name in enumerate(GATHER_NAMES):
        check_gather_hlsl(outputs[f"lowered_gather_{n}"].read_text(encoding="utf-8"), name)
    # The gather cases that are read back: the plain gather reads its component on the plain sampler, a
    # comparison gather is GatherCmp on the comparison sampler (those two have no HLSL on an old SPIRV-Cross).
    for name, component, reference in GATHER_CASES:
        if f"lowered_g4_{name}" not in outputs:
            continue
        check_gather_hlsl(outputs[f"lowered_g4_{name}"].read_text(encoding="utf-8"),
                          GATHER_NAMES[component] if component is not None else "SampleLevel",
                          comparison_gather=reference is not None)
    # Not split: the same gather on the comparison sampler is what the check rejects, and a module with no
    # ordinary use keeps its comparison gather and plain sample untouched.
    detected(check_gather_hlsl, outputs["shared_gather"].read_text(encoding="utf-8"), "GatherGreen")
    if "unsplit_dref_gather" in outputs:
        unsplit = outputs["unsplit_dref_gather"].read_text(encoding="utf-8")
        assert re.search(r"SamplerComparisonState\s+(\w+)\s*:\s*register\(s2\b", unsplit), unsplit
        assert unsplit.count("GatherCmp(cmpSampler,") == 1 and unsplit.count("SampleLevel(plainSampler,") == 1, unsplit
    print(f"shared and lowered SPIR-V validated; comparison s2, ordinary s{ORDINARY_BINDING}; controls passed",
          flush=True)
    return outputs


def compile_dxil(directory, dxc, outputs):
    # The unsplit module reproduces the original DXC rejection.
    shared = subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", directory / "shared.dxil", outputs["shared"]],
                            capture_output=True, text=True)
    if shared.returncode == 0 or "no matching member function for call to 'SampleLevel'" not in shared.stderr:
        raise AssertionError(f"Expected the shared-sampler DXC diagnostic\n{shared.stdout}\n{shared.stderr}")
    print("shared-sampler HLSL: DXC rejected SampleLevel as expected", flush=True)
    # The unsplit arrayed module is rejected the same way.
    shared = subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", directory / "shared_array.dxil",
                             outputs["shared_array"]], capture_output=True, text=True)
    if shared.returncode == 0 or "no matching member function for call to 'SampleLevel'" not in shared.stderr:
        raise AssertionError(f"Expected the arrayed shared-sampler DXC diagnostic\n{shared.stdout}\n{shared.stderr}")
    print("arrayed shared-sampler HLSL: DXC rejected SampleLevel as expected", flush=True)
    shared = subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", directory / "shared_multi.dxil",
                             outputs["shared_multi"]], capture_output=True, text=True)
    if shared.returncode == 0 or "no matching member function for call to 'SampleLevel'" not in shared.stderr:
        raise AssertionError(f"Expected the two-sampler shared DXC diagnostic\n{shared.stdout}\n{shared.stderr}")
    print("two-sampler shared HLSL: DXC rejected SampleLevel as expected", flush=True)
    shared = subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", directory / "shared_volume.dxil",
                             outputs["shared_volume"]], capture_output=True, text=True)
    if shared.returncode == 0 or "no matching member function for call to 'SampleLevel'" not in shared.stderr:
        raise AssertionError(f"Expected the Texture3D shared-sampler DXC diagnostic\n{shared.stdout}\n{shared.stderr}")
    print("Texture3D shared-sampler HLSL: DXC rejected SampleLevel as expected", flush=True)
    # The gather modules are required here: with this path DXC must accept them, so a module whose HLSL
    # SPIRV-Cross could not write (no GatherCmp in an older one) fails instead of being skipped as on Linux.
    gather_modules = ["lowered_gather", "lowered_dref_gather", "lowered_gather_both",
                      *(f"lowered_gather_{n}" for n in range(4)), *(f"lowered_g4_{name}" for name, _, _ in GATHER_CASES)]
    missing = [name for name in gather_modules if name not in outputs]
    if missing:
        raise AssertionError(f"No HLSL for {missing}: the DXC path needs a SPIRV-Cross that writes GatherCmp")
    dxil = {}
    for name in ("lowered", "lowered_lod1", "lowered_lod075", "lowered_lod_computed", "lowered_volume", "lowered_array", "lowered_array_layer1", "lowered_multi", "lowered_multi_swapped",
                 *gather_modules):
        dxil[name] = directory / f"{name}.dxil"
        subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil[name], outputs[name]], check=True)
    print("DXC compiled the lowered module, both arrayed lowered modules (layers 0 and 1) and "
          f"{len(gather_modules)} gather modules (plain and comparison)", flush=True)
    return dxil


def with_words(base=None, **words):
    reference = (base or EXPECTED).copy()
    for index, value in words.items():
        reference[int(index[1:])] = value
    return reference


# Texture3D value texture (two 2x2 slices, mip 1 of 0.625): slice 1's top-left is 0.3125, slice 0's 0.375.
VOLUME_DRAWS = [
    ("volume slice 1", "lowered_volume", "volume", EXPECTED_VOLUME, []),
    # The slice 0 value in the reference: only the ordinary word differs.
    ("volume slice 0 reference", "lowered_volume", "volume", EXPECTED, ["word=2 expected=3ec00000 actual=3ea00000"]),
    ("volume comparison descriptor", "lowered_volume", "volume-comparison-greater",
     with_words(EXPECTED_VOLUME, w0=0x00000000, w1=0x3f800000), []),
    ("volume ordinary descriptor vs baseline", "lowered_volume", "volume-ordinary-min-lod1", EXPECTED_VOLUME,
     ["word=2 expected=3ea00000 actual=3f200000"]),
    ("volume ordinary descriptor", "lowered_volume", "volume-ordinary-min-lod1",
     with_words(EXPECTED_VOLUME, w2=0x3f200000), []),
]

MULTI = "lowered_multi"
# (name, module, descriptor variant, reference, mismatching words reported on stderr); the two-sampler
# cases pass both ordinary registers to the host.
MULTI_DRAWS = [
    ("multi baseline", MULTI, "multi", EXPECTED_MULTI, []),
    ("multi wrong reference", MULTI, "multi", with_words(EXPECTED_MULTI, w1=0x3f800000),
     ["word=1 expected=3f800000 actual=00000000"]),
    # One static sampler changes at a time; each moves only the word it drives.
    ("multi comparison A vs baseline", MULTI, "multi-comparison-a-greater", EXPECTED_MULTI,
     ["word=0 expected=3f800000 actual=00000000"]),
    ("multi comparison A", MULTI, "multi-comparison-a-greater", with_words(EXPECTED_MULTI, w0=0x00000000), []),
    ("multi comparison B vs baseline", MULTI, "multi-comparison-b-greater", EXPECTED_MULTI,
     ["word=1 expected=00000000 actual=3f800000"]),
    ("multi comparison B", MULTI, "multi-comparison-b-greater", with_words(EXPECTED_MULTI, w1=0x3f800000), []),
    ("multi ordinary A vs baseline", MULTI, "multi-ordinary-a-min-lod1", EXPECTED_MULTI,
     ["word=2 expected=3ec00000 actual=3f200000"]),
    ("multi ordinary A", MULTI, "multi-ordinary-a-min-lod1", with_words(EXPECTED_MULTI, w2=0x3f200000), []),
    ("multi ordinary B vs baseline", MULTI, "multi-ordinary-b-min-lod1", EXPECTED_MULTI,
     ["word=3 expected=3f600000 actual=3f200000"]),
    ("multi ordinary B", MULTI, "multi-ordinary-b-min-lod1", with_words(EXPECTED_MULTI, w3=0x3f200000), []),
    # Bindings given in the other order: register 5 now serves cmpB's ordinary sample, so clamping the
    # first register changes word 3, not word 2.
    ("multi swapped bindings", "lowered_multi_swapped", "multi-ordinary-a-min-lod1",
     with_words(EXPECTED_MULTI, w3=0x3f200000), []),
    ("multi swapped bindings vs baseline", "lowered_multi_swapped", "multi-ordinary-a-min-lod1", EXPECTED_MULTI,
     ["word=3 expected=3f600000 actual=3f200000"]),
]


# (name, module, descriptor variant, reference, mismatching words reported on stderr)
DRAWS = [
    ("baseline", "lowered", "baseline", EXPECTED, []),
    # Wrong reference: claim the failing comparison passed.
    ("wrong reference", "lowered", "baseline", with_words(w1=0x3f800000),
     ["word=1 expected=3f800000 actual=00000000"]),
    # s2 switched to GREATER: only the two comparison words change, against the baseline reference.
    ("comparison descriptor vs baseline", "lowered", "comparison-greater", EXPECTED,
     ["word=0 expected=3f800000 actual=00000000", "word=1 expected=00000000 actual=3f800000"]),
    ("comparison descriptor", "lowered", "comparison-greater", with_words(w0=0x00000000, w1=0x3f800000), []),
    # Ordinary sampler clamped to MinLOD 1: only the ordinary word changes, to t1's 1x1 mip (0.625).
    ("ordinary descriptor vs baseline", "lowered", "ordinary-min-lod1", EXPECTED,
     ["word=2 expected=3ec00000 actual=3f200000"]),
    ("ordinary descriptor", "lowered", "ordinary-min-lod1", with_words(w2=0x3f200000), []),
    # Ordinary LOD 1.0 and 0.75 read t1's 1x1 mip (0.625), not mip 0 (0.375); the zero-LOD reference fails at word 2
    # only, which is also what a pass that forced the ordinary LOD to zero would produce.
    ("ordinary LOD 1.0", "lowered_lod1", "baseline", EXPECTED_LOD1, []),
    ("ordinary LOD 0.75", "lowered_lod075", "baseline", EXPECTED_LOD1, []),
    ("ordinary computed LOD 1.0", "lowered_lod_computed", "baseline", EXPECTED_LOD1, []),
    ("ordinary LOD 1.0 vs zero-LOD reference", "lowered_lod1", "baseline", EXPECTED,
     ["word=2 expected=3ec00000 actual=3f200000"]),
    ("ordinary LOD 0.75 vs zero-LOD reference", "lowered_lod075", "baseline", EXPECTED,
     ["word=2 expected=3ec00000 actual=3f200000"]),
    # Texture2DArray: two-layer t0 and t1 whose layer 1 differs from layer 0 in each sampled texel.
    ("array layer 0", "lowered_array", "array", EXPECTED, []),
    ("array layer 1", "lowered_array_layer1", "array", EXPECTED_LAYER1, []),
    # Layer 1 against layer 0's reference: the second comparison and the ordinary sample change.
    ("array layer 1 vs layer 0", "lowered_array_layer1", "array", EXPECTED,
     ["word=1 expected=00000000 actual=3f800000", "word=2 expected=3ec00000 actual=3ea00000"]),
    # On layer 1, s2 switched to GREATER changes only the comparison words (0.875 is not below 0.25 or 0.75).
    ("array comparison descriptor vs layer 1", "lowered_array_layer1", "array-comparison-greater", EXPECTED_LAYER1,
     ["word=0 expected=3f800000 actual=00000000", "word=1 expected=3f800000 actual=00000000"]),
    ("array comparison descriptor", "lowered_array_layer1", "array-comparison-greater",
     with_words(EXPECTED_LAYER1, w0=0x00000000, w1=0x00000000), []),
    # On layer 1, the ordinary sampler at MinLOD 1 reads layer 1's 1x1 mip (0.5625), not layer 0's (0.625).
    ("array ordinary descriptor vs layer 1", "lowered_array_layer1", "array-ordinary-min-lod1", EXPECTED_LAYER1,
     ["word=2 expected=3ea00000 actual=3f100000"]),
    ("array ordinary descriptor", "lowered_array_layer1", "array-ordinary-min-lod1",
     with_words(EXPECTED_LAYER1, w2=0x3f100000), []),
]


def gather_draw(name, component, reference, expected_component, expected_reference):
    """One gather case; the expectation may be modelled from other parameters than the module's (a control)."""
    module = f"lowered_g4_{[n for n, c, r in GATHER_CASES if (c, r) == (component, reference)][0]}"
    actual = model_words(component, reference)
    expected = model_words(expected_component, expected_reference)
    return (name, module, "gather", expected, readback_mismatches(expected, actual))


# The gathers are read back from source-built 2x2 textures (see case_file), all four returned values per case.
# The controls model the expectation from a wrong component or reference and expect exactly the words that differ.
GATHER_DRAWS = [
    gather_draw("gather green", 1, None, 1, None),
    gather_draw("gather red", 0, None, 0, None),
    gather_draw("gather green vs red expectation", 1, None, 0, None),
    # The right words in the wrong order (texel order reversed) differ at every gathered word.
    ("gather green vs reversed order expectation", "lowered_g4_gather_green", "gather",
     [*model_words(component=1)[:2], *model_words(component=1)[2:6][::-1], *model_words(component=1)[6:]],
     readback_mismatches([*model_words(component=1)[:2], *model_words(component=1)[2:6][::-1],
                          *model_words(component=1)[6:]], model_words(component=1))),
    gather_draw("compare gather ref 0.375", None, 0.375, None, 0.375),
    gather_draw("compare gather ref 0.875", None, 0.875, None, 0.875),
    gather_draw("compare gather ref 0.375 vs ref 0.875 expectation", None, 0.375, None, 0.875),
    gather_draw("gather green and compare gather", 1, 0.375, 1, 0.375),
]


def draw(directory, dxil, renderer, adapter):
    reference = directory / "reference.bin"
    for name, module, variant, expected, mismatches in [*DRAWS, *VOLUME_DRAWS, *MULTI_DRAWS, *GATHER_DRAWS]:
        registers = ",".join(map(str, MULTI_BINDINGS)) if variant.startswith("multi") else str(ORDINARY_BINDING)
        reference.write_bytes(case_file(expected) if variant == "gather" else struct.pack("<16I", *expected))
        run = subprocess.run([renderer, adapter, "--spirv-compare-sampler", dxil[module], registers,
                              reference, variant], capture_output=True, text=True)
        print(f"[{name}]", run.stdout, run.stderr, flush=True)
        if run.returncode == 77 and name == "baseline":
            return 77
        reported = [line for line in run.stderr.splitlines() if line.startswith("word=")]
        if (run.returncode != int(bool(mismatches)) or reported != mismatches
                or f"descriptors={variant} " not in run.stdout
                or f"compared_words=16 mismatches={len(mismatches)}" not in run.stdout):
            raise RuntimeError(f"Unexpected {name} readback")
    print("wrong reference fails only at word 1; s2 changes only words 0-1; "
          f"s{ORDINARY_BINDING} changes only word 2", flush=True)
    print("Texture2DArray: layer 0 and layer 1 each read their own texels (layer 1 vs layer 0 differs at words 1-2); "
          f"on layer 1, s2 changes only words 0-1 and s{ORDINARY_BINDING} only word 2 (layer 1's mip 1)", flush=True)
    print("gathers: all four returned values of a plain gather and of a comparison gather are read back from 2x2 "
          "textures; a wrong component or reference differs at exactly the modelled words", flush=True)
    print("Texture3D: the ordinary sample reads slice 1 (a slice 0 reference differs at word 2 only); "
          "s2 changes only words 0-1 and the ordinary sampler's MinLOD 1 only word 2", flush=True)
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (4, 7):
        sys.exit(__doc__)
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        outputs = emit(directory, *sys.argv[1:4])
        if len(sys.argv) == 7:
            dxil = compile_dxil(directory, sys.argv[4], outputs)
            sys.exit(draw(directory, dxil, *sys.argv[5:]))
