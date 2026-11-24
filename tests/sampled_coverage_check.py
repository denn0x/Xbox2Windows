"""A source-built indexed texture selection and coverage-only pixel export."""

import json
import math
import operator
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from pixel_aot_check import envelope, must_reject
from pixel_aot import _read_named_container
from sampled_coverage_aot import lower

# Original fixture with four independent descriptor tables. The two buffered
# floats are mapped to different slices of a single desktop upload.
PROGRAM = """
s_mov_b32 s11, 0
s_mov_b32 s21, 0
s_mov_b32 s31, 0
s_mov_b32 s41, 0
s_load_dwordx4 s[60:63], s[30:31], 0x10
s_load_dwordx4 s[64:67], s[20:21], 0x40
s_load_dwordx4 s[68:71], s[20:21], 0x70
s_load_dwordx4 s[72:75], s[10:11], null
s_mov_b64 s[48:49], exec
s_wqm_b64 exec, exec
s_mov_b64 s[52:53], exec
s_mov_b32 m0, s33
s_waitcnt lgkmcnt(0)
s_buffer_load_dword s6, s[72:75], s7
s_buffer_load_dword s9, s[68:71], 0x8
s_buffer_load_dword s3, s[64:67], 0x4
v_interp_p1_f32_e32 v8, v0, attr0.x
v_interp_p2_f32_e32 v8, v1, attr0.x
v_interp_p1_f32_e32 v9, v0, attr0.y
v_interp_p2_f32_e32 v9, v1, attr0.y
s_waitcnt lgkmcnt(0)
s_lshl_b32 s12, s6, 5
s_load_dwordx8 s[64:71], s[40:41], s12
s_waitcnt lgkmcnt(0)
image_sample v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
v_mul_legacy_f32_e32 v11, s3, v10
v_cmpx_gt_f32_e32 s9, v11
s_andn2_b64 s[48:49], s[48:49], exec
s_cbranch_scc0 6
s_wqm_b64 s[54:55], s[48:49]
s_and_b64 exec, s[52:53], s[54:55]
s_and_b64 exec, exec, s[48:49]
exp null off, off, off, off done vm
s_endpgm
s_and_b64 exec, exec, s[48:49]
s_mov_b64 exec, s[48:49]
exp null off, off, off, off done vm
s_endpgm
""".strip().splitlines()


def variant(multiply, compare):
    return [
        line.replace("v_mul_legacy_f32_e32", multiply).replace(
            "v_cmpx_gt_f32_e32", compare
        )
        for line in PROGRAM
    ]


# The IEEE multiply with each kill relation that the translator accepts. The
# legacy multiply keeps its original greater-than program above.
RELATIONS = ("lt", "le", "gt", "ge")
OP_PROGRAMS = {
    relation: variant("v_mul_f32_e32", f"v_cmpx_{relation}_f32_e32")
    for relation in RELATIONS
}


def spelled(legacy, multiply_form, compare_form, relation):
    kind = "_legacy" if legacy else ""
    return variant(
        f"v_mul{kind}_f32_{multiply_form}", f"v_cmpx_{relation}_f32_{compare_form}"
    )


# The VOP3 (e64) spellings of both multiplies and all four kills, and the two
# mixed pairings, keyed by (legacy, multiply form, compare form, relation).
E64_PROGRAMS = {
    (legacy, "e64", "e64", relation): spelled(legacy, "e64", "e64", relation)
    for legacy in (False, True)
    for relation in RELATIONS
}
MIXED_PROGRAMS = {
    (index % 2 == 1, *forms, relation): spelled(index % 2 == 1, *forms, relation)
    for index, relation in enumerate(RELATIONS)
    for forms in (("e32", "e64"), ("e64", "e32"))
}
# The explicit-level zero sample has the same operands as the implicit one.
LZ_PROGRAM = [line.replace("image_sample ", "image_sample_lz ") for line in PROGRAM]
# The explicit-level sample reads its level from the third address register,
# which the program copies from a buffered float (word 3 of the first slice).
LEVEL_LOAD = "s_buffer_load_dword s5, s[64:67], 0xc"
LEVEL_MOVE = "v_mov_b32_e32 v10, s5"
LEVEL_SAMPLE_LINE = (
    "image_sample_l v12, v[8:10], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D"
)
LEVEL_PROGRAM = (
    "\n".join(PROGRAM)
    .replace(
        "s_buffer_load_dword s3, s[64:67], 0x4",
        "s_buffer_load_dword s3, s[64:67], 0x4\n" + LEVEL_LOAD,
    )
    .replace("s_lshl_b32 s12, s6, 5", LEVEL_MOVE + "\ns_lshl_b32 s12, s6, 5")
    .replace(
        "image_sample v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D",
        LEVEL_SAMPLE_LINE,
    )
    .replace("v_mul_legacy_f32_e32 v11, s3, v10", "v_mul_legacy_f32_e32 v11, s3, v12")
    .splitlines()
)
# The biased sample takes the bias from the first address register, ahead of the
# two coordinates (bias, s, t), so the interpolated pair moves up one register
# and the buffered float is copied into v8. LLVM's own lowering of the
# image.sample.b intrinsic puts its bias argument in that first register.
BIAS_MOVE = "v_mov_b32_e32 v8, s5"
BIAS_SAMPLE_LINE = (
    "image_sample_b v12, v[8:10], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D"
)
BIAS_PROGRAM = (
    "\n".join(LEVEL_PROGRAM)
    .replace(LEVEL_MOVE, BIAS_MOVE)
    .replace("v9, v", "v10, v")
    .replace("v8, v", "v9, v")
    .replace(LEVEL_SAMPLE_LINE, BIAS_SAMPLE_LINE)
    .splitlines()
)
PROGRAMS = [
    PROGRAM,
    LZ_PROGRAM,
    LEVEL_PROGRAM,
    BIAS_PROGRAM,
    *OP_PROGRAMS.values(),
    *E64_PROGRAMS.values(),
    *MIXED_PROGRAMS.values(),
]


def bindings(slot):
    return dict(
        interp=33,
        words=[[7, 16]],
        selector_byte=16,
        descriptors=[
            [10, 0, "selector", slot],
            [20, 64, "cb", 0],
            [20, 112, "cb", 4],
            [30, 16, "sampler", 0],
            [40, 0, "texture", 0],
            [40, 32, "texture", 1],
        ],
    )


def decoded(lines):
    # The relative branch only spans four-byte scalar instructions and one
    # eight-byte export. The binary test independently checks real LLVM PCs.
    pc = 0
    result = []
    for line in lines:
        result.append((pc, line))
        pc += (
            8
            if line.startswith(("exp ", "s_load_", "s_buffer_load_", "image_sample"))
            or line.split(" ")[0].endswith("_e64")
            else 4
        )
    return result


def controls():
    program = decoded(PROGRAM)
    for old, new in [
        ("s_cbranch_scc0 6", "s_cbranch_scc0 5"),
        ("vmcnt(0)", "expcnt(0)"),
        ("s[52:53], s[54:55]", "s[54:55], s[54:55]"),
        ("s[60:63] dmask", "s[72:75] dmask"),
    ]:
        must_reject(
            lower, [(pc, line.replace(old, new)) for pc, line in program], bindings(0)
        )
    must_reject(lower, program, bindings(2))
    must_reject(lower, program, bindings(0) | {"selector_byte": 12})

    source = "\n".join(PROGRAM)
    selector_load = "s_buffer_load_dword s6, s[72:75], s7"
    zero_offset = source.replace(selector_load, selector_load.replace(", s7", ", null"))
    must_reject(lower, decoded(zero_offset.splitlines()), bindings(0) | {"selector_byte": False})

    overlap = bindings(0)
    overlap["descriptors"].append([20, 64, "cb", 0])
    must_reject(lower, program, overlap)

    wide_sampler = source.replace(
        "s_load_dwordx4 s[60:63], s[30:31], 0x10",
        "s_load_dwordx8 s[56:63], s[30:31], 0x10",
    ).replace("s[60:63] dmask", "s[56:59] dmask")
    must_reject(lower, decoded(wide_sampler.splitlines()), bindings(0))

    scalar_wait = "s_waitcnt lgkmcnt(0)"
    early_selector = source.replace(
        f"{scalar_wait}\n{selector_load}", f"{selector_load}\n{scalar_wait}"
    )
    must_reject(lower, decoded(early_selector.splitlines()), bindings(0))

    swapped_coords = source.replace("attr0.x", "attr0.y").replace(
        "v9, v0, attr0.y", "v9, v0, attr0.x"
    ).replace("v9, v1, attr0.y", "v9, v1, attr0.x")
    must_reject(lower, decoded(swapped_coords.splitlines()), bindings(0))

    sample_wait = "s_waitcnt vmcnt(0)"
    product = "v_mul_legacy_f32_e32 v11, s3, v10"
    early_product = source.replace(f"{sample_wait}\n{product}", f"{product}\n{sample_wait}")
    must_reject(lower, decoded(early_product.splitlines()), bindings(0))

    for relation, program in OP_PROGRAMS.items():
        for slot in range(2):
            lower(decoded(program), bindings(slot))
    ieee = "\n".join(OP_PROGRAMS["lt"])
    for old, new in [
        ("v_cmpx_lt_f32_e32 s9, v11", "v_cmpx_lg_f32_e32 s9, v11"),
        ("v_cmpx_lt_f32_e32 s9, v11", "v_cmpx_lt_f64_e32 s9, v11"),
        ("v_cmpx_lt_f32_e32 s9, v11", "v_cmpx_lt_f32_e32 v11, s9"),
        ("v_mul_f32_e32 v11, s3, v10", "v_mul_f32_e32 v11, v10, v10"),
        ("v_mul_f32_e32 v11, s3, v10", "v_mul_f16_e32 v11, s3, v10"),
        ("v_mul_f32_e32 v11, s3, v10", "v_add_f32_e32 v11, s3, v10"),
    ]:
        must_reject(lower, decoded(ieee.replace(old, new).splitlines()), bindings(0))
    ieee_product = "v_mul_f32_e32 v11, s3, v10"
    early_ieee = ieee.replace(
        f"{sample_wait}\n{ieee_product}", f"{ieee_product}\n{sample_wait}"
    )
    must_reject(lower, decoded(early_ieee.splitlines()), bindings(0))
    # Every wrong relation, and the legacy multiply, must give a different
    # reference somewhere, otherwise the readback controls prove nothing.
    for slot in range(2):
        for relation in RELATIONS:
            for other in RELATIONS:
                if relation != other:
                    distinguishing_case(slot, relation, False, other, False)
    for relation in RELATIONS:
        distinguishing_case(0, relation, False, relation, True)
    # Each e64 readback needs a case that tells every wrong reference apart,
    # drawn from its smaller case list.
    for legacy in (False, True):
        for relation in RELATIONS:
            for other in RELATIONS:
                if relation != other:
                    distinguishing_case(0, relation, legacy, other, legacy, E64_CASES)
            distinguishing_case(0, relation, legacy, relation, not legacy, E64_CASES)

    spelling_controls()
    sample_lz_controls()
    sample_level_controls()
    sample_bias_controls()

    wrong_quad_source = source.replace(
        "s_wqm_b64 s[54:55], s[48:49]", "s_wqm_b64 s[54:55], s[52:53]"
    )
    must_reject(lower, decoded(wrong_quad_source.splitlines()), bindings(0))


def reject_with(lines, message, slot=0):
    # The rejection has to come from the operand check, not an unrelated one.
    try:
        lower(decoded(lines), bindings(slot))
    except ValueError as error:
        assert message in str(error), error
    else:
        raise AssertionError("The translator accepted " + repr(lines))


MULTIPLY_MESSAGE = "Unsupported multiply operands or modifiers"
COMPARE_MESSAGE = "Unsupported compare operands or modifiers"


def modifier_mutations():
    # Every row replaces one line of an e64 program with a form LLVM can print
    # but the lowering does not model: clamp, output scale, negate, absolute
    # value, a swapped or special-register operand, or an inline constant.
    rows = []
    for mul in ("v_mul_legacy_f32_e64", "v_mul_f32_e64"):
        line = f"{mul} v11, s3, v10"
        for text in (
            f"{mul} v11, s3, v10 clamp",
            f"{mul} v11, s3, v10 mul:2",
            f"{mul} v11, s3, v10 div:2",
            f"{mul} v11, -s3, v10",
            f"{mul} v11, s3, -v10",
            f"{mul} v11, |s3|, v10",
            f"{mul} v11, s3, |v10|",
            f"{mul} v11, v10, s3",
            f"{mul} v11, vcc_lo, v10",
            f"{mul} v11, s3, null",
            f"{mul} v11, s3, 1.0",
        ):
            rows.append((mul, line, text, MULTIPLY_MESSAGE))
    for relation in RELATIONS:
        compare = f"v_cmpx_{relation}_f32_e64"
        line = f"{compare} s9, v11"
        for text in (
            f"{compare} s9, v11 clamp",
            f"{compare} s9, -v11",
            f"{compare} s9, |v11|",
            f"{compare} -s9, v11",
            f"{compare} v11, s9",
            f"{compare} vcc_lo, v11",
            f"{compare} exec_lo, v11",
            f"{compare} null, v11",
            f"{compare} 1.0, v11",
        ):
            rows.append((compare, line, text, COMPARE_MESSAGE))
    return rows


def mutation_source(line):
    legacy = "_legacy_" in line
    relation = line.split("_")[2] if line.startswith("v_cmpx_") else "lt"
    return "\n".join(spelled(legacy, "e64", "e64", relation))


def spelling_controls():
    # Every e64 and mixed spelling lowers on both textures to the same program
    # as its e32 counterpart. The readback fixtures check that program against
    # a CPU reference for each relation and multiply.
    for (legacy, multiply, compare, relation), program in [
        *E64_PROGRAMS.items(),
        *MIXED_PROGRAMS.items(),
    ]:
        for slot in range(2):
            assert lower(decoded(program), bindings(slot)) == lower(
                decoded(spelled(legacy, "e32", "e32", relation)), bindings(slot)
            ), (legacy, multiply, compare, relation)
    for _, line, text, message in modifier_mutations():
        reject_with(mutation_source(line).replace(line, text).splitlines(), message)
    legacy_e64 = "\n".join(E64_PROGRAMS[(True, "e64", "e64", "lt")])
    ieee_e64 = "\n".join(E64_PROGRAMS[(False, "e64", "e64", "lt")])
    for source in (ieee_e64, legacy_e64):
        for old, new in [
            ("v_cmpx_lt_f32_e64 s9, v11", "v_cmpx_lt_f64_e64 s9, v11"),
            ("v_cmpx_lt_f32_e64 s9, v11", "v_cmpx_lg_f32_e64 s9, v11"),
            ("v_cmpx_lt_f32_e64 s9, v11", "v_cmp_lt_f32_e64 s[2:3], s9, v11"),
            ("v_cmpx_lt_f32_e64 s9, v11", "v_cmpx_class_f32_e64 s9, v11"),
            ("v_mul_f32_e64 v11, s3, v10", "v_mul_f16_e64 v11, s3, v10"),
            ("v_mul_legacy_f32_e64 v11, s3, v10", "v_mul_f16_e64 v11, s3, v10"),
            ("v_mul_f32_e64 v11, s3, v10", "v_add_f32_e64 v11, s3, v10"),
            ("v_mul_legacy_f32_e64 v11, s3, v10", "v_sub_f32_e64 v11, s3, v10"),
            ("v_mul_f32_e64 v11, s3, v10", "v_mul_f32_e64 v11, s3"),
            ("v_cmpx_lt_f32_e64 s9, v11", "v_cmpx_lt_f32_e64 s9"),
        ]:
            if old in source:
                must_reject(lower, decoded(source.replace(old, new).splitlines()), bindings(0))
    # A product read before the sample wait is still refused in the e64 form.
    for legacy in (False, True):
        program = "\n".join(E64_PROGRAMS[(legacy, "e64", "e64", "gt")])
        mul = f"v_mul{'_legacy' if legacy else ''}_f32_e64 v11, s3, v10"
        must_reject(
            lower,
            decoded(
                program.replace(
                    f"s_waitcnt vmcnt(0)\n{mul}", f"{mul}\ns_waitcnt vmcnt(0)"
                ).splitlines()
            ),
            bindings(0),
        )


SAMPLE_LINE = "image_sample v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D"
# Rewrites of the sample line that the pattern must refuse for every opcode. The
# format is a template for the opcode. Every row assembles with LLVM 19 except
# the last two, which only the translator-level check uses.
SAMPLE_REFUSED = (
    "{op} v[10:11], v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D tfe",
    "{op} v10, v8, s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D a16",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D lwe",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D d16",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D r128",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D unorm",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D glc",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D slc",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D dlc",
    "{op} v[10:11], v[8:9], s[64:71], s[60:63] dmask:0x3 dim:SQ_RSRC_IMG_2D",
    "{op} v10, v[8:10], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_3D",
    "{op}_o v10, v[8:10], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D da",
    "{op} v10, v[8:9], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D_ARRAY",
)


def sample_refusals(assembled_only=False):
    # (opcode, program, expected message) for each refused rewrite.
    rows = []
    extra = {
        "image_sample_l": (LEVEL_PROGRAM, LEVEL_SAMPLE_LINE),
        "image_sample_b": (BIAS_PROGRAM, BIAS_SAMPLE_LINE),
    }
    for op in ("image_sample", "image_sample_lz", "image_sample_l", "image_sample_b"):
        level = op in extra
        program, line = extra.get(
            op,
            (
                PROGRAM if op == "image_sample" else LZ_PROGRAM,
                SAMPLE_LINE.replace("image_sample", op, 1),
            ),
        )
        base = "\n".join(program)
        for template in SAMPLE_REFUSED[: -2 if assembled_only else None]:
            text = template.format(op=op)
            if level:
                # Three address registers: two coordinates, then the level.
                text = text.replace("v[8:9]", "v[8:10]").replace(" v8,", " v[8:9],")
                if "_3D" in text or "_o " in text:
                    # A third dimension or an offset word adds a fourth register.
                    text = text.replace("v[8:10]", "v[8:11]")
            message = (
                "Unsupported sample"
                if text.split(" ")[0] == op
                else "Unsupported coverage instruction"
            )
            rows.append((text, base.replace(line, text).splitlines(), message))
    return rows


def lowers_as_level(source):
    return "SampleLevel(samp,uv,0)" in source and ".Sample(" not in source


def sample_lz_controls():
    # The zero-level form lowers to the same program with only the sampling call
    # replaced, for both textures and for the red channel.
    red_lz = [line.replace("image_sample ", "image_sample_lz ") for line in RED_PROGRAM]
    for slot in range(2):
        implicit = lower(decoded(PROGRAM), bindings(slot))
        explicit = lower(decoded(LZ_PROGRAM), bindings(slot))
        assert lowers_as_level(explicit) and not lowers_as_level(implicit)
        assert explicit == implicit.replace(".Sample(samp,uv)", ".SampleLevel(samp,uv,0)")
        assert f"tex{slot}.SampleLevel(samp,uv,0).a" in explicit
        implicit = lower(decoded(RED_PROGRAM), red_bindings(slot))
        explicit = lower(decoded(red_lz), red_bindings(slot))
        assert explicit == implicit.replace(".Sample(samp,uv)", ".SampleLevel(samp,uv,0)")
        assert f"tex{slot}.SampleLevel(samp,uv,0).r" in explicit
    # Every modifier, dmask, dimension and opcode the lowering does not model is
    # refused for the zero-level form exactly as for the implicit one.
    for _, lines, message in sample_refusals():
        reject_with(lines, message)
    # One sampled value per program, in either order and either spelling.
    for first, second in (
        ("image_sample", "image_sample_lz"),
        ("image_sample_lz", "image_sample"),
        ("image_sample_lz", "image_sample_lz"),
    ):
        lines = "\n".join(PROGRAM).replace(
            SAMPLE_LINE,
            SAMPLE_LINE.replace("image_sample", first, 1)
            + "\n"
            + SAMPLE_LINE.replace("image_sample", second, 1).replace("v10", "v12", 1),
        )
        reject_with(lines.splitlines(), "Unsupported sample")


# Distinct words, so a level read from the wrong word cannot match by accident.
LEVEL_CONSTANTS = [k + 0.25 for k in range(64)]


def level_of(source):
    # The level a lowered program passes, evaluated against LEVEL_CONSTANTS by a
    # separate reader of the HLSL, not by the translator's word arithmetic.
    call = re.search(r"Sample(?:Level|Bias)\(samp,uv,([^)]*)\)", source)
    assert call, source
    word = re.fullmatch(r"params\[(\d+)\]\.([xyzw])", call[1])
    if word:
        return LEVEL_CONSTANTS[4 * int(word[1]) + "xyzw".index(word[2])]
    return float(call[1])


def level_variant(*replacements, program=LEVEL_PROGRAM):
    source = "\n".join(program)
    for old, new in replacements:
        assert source.count(old) == 1, old
        source = source.replace(old, new)
    return source.splitlines()


def level_refusals():
    # (text, program, message): the level must be a buffered float copied into
    # the third address register before the sample, with the load waited for.
    wait = "s_waitcnt lgkmcnt(0)\n" + LEVEL_MOVE
    moves = [
        (f"v_mov_b32_e32 v10, {source}", "Unsupported move operands")
        for source in ("v9", "1.0", "0x40000000", "null", "vcc_lo")
    ]
    moves += [("v_mov_b32_e32 v10, s7", "Unresolved move source")]
    rows = [
        ("no level move", level_variant((LEVEL_MOVE + "\n", "")), "Unresolved sample level"),
        (
            "level in the wrong register",
            level_variant((LEVEL_MOVE, "v_mov_b32_e32 v11, s5")),
            "Unresolved sample level",
        ),
        (
            "level before the load wait",
            level_variant((wait, LEVEL_MOVE + "\ns_waitcnt lgkmcnt(0)")),
            "Scalar load consumed before wait",
        ),
        (
            "VOP3 move",
            level_variant((LEVEL_MOVE, "v_mov_b32_e64 v10, s5")),
            "Unsupported coverage instruction",
        ),
        (
            "level as the second coordinate",
            level_variant(("v[8:10]", "v[9:11]")),
            "Unresolved sample coordinates",
        ),
        (
            "level without a definition",
            "\n".join(PROGRAM)
            .replace(SAMPLE_LINE, SAMPLE_LINE.replace("image_sample v10, v[8:9]", "image_sample_l v10, v[8:10]"))
            .splitlines(),
            "Unresolved sample level",
        ),
    ]
    for text, message in moves:
        rows.append((text, level_variant((LEVEL_MOVE, text)), message))
    return rows


def sample_level_controls():
    # The explicit level lowers to SampleLevel with the level read from the
    # buffered word the program loads, on both textures and for the red channel.
    for offset in (0, 4, 8, 12):
        load = LEVEL_LOAD.replace("0xc", hex(offset))
        program = level_variant((LEVEL_LOAD, load))
        for slot in range(2):
            source = lower(decoded(program), bindings(slot))
            assert f"tex{slot}.SampleLevel(samp,uv," in source and ".Sample(" not in source
            assert f"tex{slot}.SampleLevel(samp,uv,params[0]" in source and ").a;" in source
            assert level_of(source) == LEVEL_CONSTANTS[offset // 4], (offset, source)
    # The same program with the zero-level opcode, or with the implicit opcode,
    # must not read the buffered level: each is told apart from the explicit one.
    explicit = lower(decoded(LEVEL_PROGRAM), bindings(0))
    zero = lower(
        decoded(level_variant((LEVEL_SAMPLE_LINE, LEVEL_SAMPLE_LINE.replace("_l v12, v[8:10]", "_lz v12, v[8:9]")))),
        bindings(0),
    )
    implicit = lower(
        decoded(level_variant((LEVEL_SAMPLE_LINE, LEVEL_SAMPLE_LINE.replace("_l v12, v[8:10]", " v12, v[8:9]")))),
        bindings(0),
    )
    assert level_of(explicit) == LEVEL_CONSTANTS[3] != level_of(zero) == 0
    assert "Sample(samp,uv)" in implicit and "SampleLevel" not in implicit
    # The last move before the sample defines the level.
    again = level_variant((LEVEL_MOVE, LEVEL_MOVE + "\nv_mov_b32_e32 v10, s3"))
    assert level_of(lower(decoded(again), bindings(0))) == LEVEL_CONSTANTS[1]
    # A level copied from a different word is a different program.
    other = level_variant((LEVEL_MOVE, "v_mov_b32_e32 v10, s3"))
    assert level_of(lower(decoded(other), bindings(0))) != level_of(explicit)
    red = [
        line.replace("s_lshl_b32 s12, s6, 5", "s_lshl_b32 s12, s6, 8").replace(
            "dmask:0x8", "dmask:0x1"
        )
        for line in LEVEL_PROGRAM
    ]
    for slot in range(2):
        source = lower(decoded(red), red_bindings(slot))
        assert f"tex{slot}.SampleLevel(samp,uv,params[0].w).r" in source
    for _, lines, message in level_refusals():
        reject_with(lines, message)


def bias_refusals():
    # (text, program, message): the bias must be a buffered float copied into the
    # first address register before the sample, with the load waited for.
    wait = "s_waitcnt lgkmcnt(0)\n" + BIAS_MOVE
    rows = [
        ("no bias move", bias_variant((BIAS_MOVE + "\n", "")), "Unresolved sample level"),
        (
            "bias in the wrong register",
            bias_variant((BIAS_MOVE, "v_mov_b32_e32 v11, s5")),
            "Unresolved sample level",
        ),
        (
            "bias before the load wait",
            bias_variant((wait, BIAS_MOVE + "\ns_waitcnt lgkmcnt(0)")),
            "Scalar load consumed before wait",
        ),
        (
            "VOP3 move",
            bias_variant((BIAS_MOVE, "v_mov_b32_e64 v8, s5")),
            "Unsupported coverage instruction",
        ),
        (
            "bias as the third register",
            level_variant(
                (LEVEL_SAMPLE_LINE, LEVEL_SAMPLE_LINE.replace("_l v12", "_b v12"))
            ),
            "Unresolved sample coordinates",
        ),
        (
            "coordinates shifted past the bias",
            bias_variant(("v[8:10]", "v[9:11]")),
            "Unresolved sample coordinates",
        ),
    ]
    for source in ("v9", "1.0", "0x40000000", "null", "vcc_lo"):
        text = f"v_mov_b32_e32 v8, {source}"
        rows.append((text, bias_variant((BIAS_MOVE, text)), "Unsupported move operands"))
    rows.append(
        (
            "v_mov_b32_e32 v8, s7",
            bias_variant((BIAS_MOVE, "v_mov_b32_e32 v8, s7")),
            "Unresolved move source",
        )
    )
    # The clamped and compare forms add a register or a value the lowering does
    # not model, so they are refused by name.
    for text in (
        "image_sample_b_cl v12, v[8:11], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D",
        "image_sample_c_b v12, v[8:11], s[64:71], s[60:63] dmask:0x8 dim:SQ_RSRC_IMG_2D",
    ):
        rows.append(
            (text, bias_variant((BIAS_SAMPLE_LINE, text)), "Unsupported coverage instruction")
        )
    return rows


def bias_variant(*replacements):
    return level_variant(*replacements, program=BIAS_PROGRAM)


def sample_bias_controls():
    # The bias lowers to SampleBias with the bias read from the buffered word
    # the program loads, on both textures, and never to Sample or SampleLevel.
    for offset in (0, 4, 8, 12):
        load = LEVEL_LOAD.replace("0xc", hex(offset))
        program = bias_variant((LEVEL_LOAD, load))
        for slot in range(2):
            source = lower(decoded(program), bindings(slot))
            assert f"tex{slot}.SampleBias(samp,uv,params[0]" in source and ").a;" in source
            assert ".Sample(" not in source and "SampleLevel" not in source
            assert level_of(source) == LEVEL_CONSTANTS[offset // 4], (offset, source)
    biased = lower(decoded(BIAS_PROGRAM), bindings(0))
    # The same registers read as a level or without an address register are other
    # programs, so each wrong opcode is told apart from the biased one.
    implicit = lower(
        decoded(bias_variant((BIAS_SAMPLE_LINE, BIAS_SAMPLE_LINE.replace("_b v12, v[8:10]", " v12, v[9:10]")))),
        bindings(0),
    )
    zero = lower(
        decoded(bias_variant((BIAS_SAMPLE_LINE, BIAS_SAMPLE_LINE.replace("_b v12, v[8:10]", "_lz v12, v[9:10]")))),
        bindings(0),
    )
    assert level_of(biased) == LEVEL_CONSTANTS[3]
    assert "SampleBias" not in implicit and "SampleLevel" not in implicit
    assert "SampleBias" not in zero and level_of(zero) == 0
    # A bias copied from a different word is a different program, and the last
    # move before the sample defines it.
    other = lower(decoded(bias_variant((BIAS_MOVE, "v_mov_b32_e32 v8, s3"))), bindings(0))
    assert level_of(other) == LEVEL_CONSTANTS[1] != level_of(biased)
    again = bias_variant((BIAS_MOVE, BIAS_MOVE + "\nv_mov_b32_e32 v8, s3"))
    assert level_of(lower(decoded(again), bindings(0))) == LEVEL_CONSTANTS[1]
    red = [
        line.replace("s_lshl_b32 s12, s6, 5", "s_lshl_b32 s12, s6, 8").replace(
            "dmask:0x8", "dmask:0x1"
        )
        for line in BIAS_PROGRAM
    ]
    for slot in range(2):
        source = lower(decoded(red), red_bindings(slot))
        assert f"tex{slot}.SampleBias(samp,uv,params[0].w).r" in source
    for _, lines, message in bias_refusals():
        reject_with(lines, message)


def binary(tmp, *tools):
    for program in PROGRAMS:
        binary_program(tmp, program, *tools)
    binary_modifiers(tmp, *tools)
    binary_samples(tmp, *tools)
    binary_levels(tmp, *tools)


def binary_levels(tmp, assembler, objcopy, library):
    # The refused level and bias programs, assembled and disassembled by the
    # real LLVM.
    for text, lines, _ in [*level_refusals(), *bias_refusals()]:
        refused(tmp, lines, text, assembler, objcopy, library)


def binary_samples(tmp, assembler, objcopy, library):
    # The refused sample rewrites, assembled and disassembled by the real LLVM.
    for text, lines, _ in sample_refusals(assembled_only=True):
        refused(tmp, lines, text, assembler, objcopy, library)


def binary_modifiers(tmp, assembler, objcopy, library):
    # The same mutations, assembled by the real LLVM and disassembled again, so
    # the refusal does not depend on this file's guess of how LLVM prints them.
    for _, line, text, _ in modifier_mutations():
        mutated = mutation_source(line).replace(line, text).splitlines()
        refused(tmp, mutated, text, assembler, objcopy, library)


def refused(tmp, lines, text, assembler, objcopy, library):
    tool = Path(__file__).resolve().parents[1] / "tools/sampled_coverage_aot.py"
    container = assemble(tmp, lines, assembler, objcopy)
    manifest = tmp / "bindings.json"
    manifest.write_text(json.dumps(bindings(0)))
    out = tmp / "refused.hlsl"
    out.unlink(missing_ok=True)
    result = subprocess.run(
        [sys.executable, "-B", tool, container, "--bindings", manifest,
         "--llvm", library, "--output", out],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2 and "Unsupported coverage input" in result.stderr, (
        text,
        result.returncode,
        result.stderr,
    )
    assert not out.exists(), text


def assemble(tmp, program, assembler, objcopy):
    src, obj, raw = (tmp / n for n in ("synthetic.s", "synthetic.o", "synthetic.bin"))
    src.write_text("\n".join(program) + "\n")
    subprocess.run(
        [assembler, "-triple=amdgcn--amdpal", "-mcpu=gfx1013", "-filetype=obj", src, "-o", obj],
        check=True,
    )
    subprocess.run([objcopy, "-O", "binary", "--only-section=.text", obj, raw], check=True)
    code = raw.read_bytes()
    padded = code + bytes.fromhex("00009fbf") * (4 - (len(code) // 4 + 10) % 4)
    container = tmp / "test.dxbc"
    record = envelope(padded, directory_end=(40 + len(padded), 288 + len(padded)))
    container.write_bytes(_read_named_container(record, len(b"fixture") + 2))
    return container


def binary_program(tmp, program, assembler, objcopy, library):
    container = assemble(tmp, program, assembler, objcopy)
    for slot in range(2):
        manifest = tmp / "bindings.json"
        manifest.write_text(json.dumps(bindings(slot)))
        out = tmp / "compiled.hlsl"
        tool = Path(__file__).resolve().parents[1] / "tools/sampled_coverage_aot.py"
        subprocess.run(
            [
                sys.executable,
                "-B",
                tool,
                container,
                "--bindings",
                manifest,
                "--llvm",
                library,
                "--output",
                out,
            ],
            check=True,
        )
        assert out.read_text() == lower(decoded(program), bindings(slot))


def red_bindings(slot, scale_base=0):
    config = bindings(0x00800000)
    config["descriptors"][1] = [20, 64, "cb", scale_base]
    config["descriptors"][-4] = [20, 112, "cb", 8]
    config["descriptors"][-2:] = [
        [40, 0x80000000, "texture", slot],
        [40, 0x00800000, "texture", 1 - slot],
        [40, 0, "texture", 1 - slot],
    ]
    return config


RED_PROGRAM = [
    line.replace("s_lshl_b32 s12, s6, 5", "s_lshl_b32 s12, s6, 8")
    .replace("dmask:0x8", "dmask:0x1")
    for line in PROGRAM
]


def case(slot, scale, threshold, scale_word, threshold_word, red=False, level=None):
    cb = bytearray(256)
    if level is not None:
        struct.pack_into("<f", cb, 12, level)
    struct.pack_into("<f", cb, 4 * scale_word, scale)
    struct.pack_into("<f", cb, 4 * threshold_word, threshold)
    ref = bytearray()
    for y in range(8):
        for x in range(8):
            byte = (41 * x + 19 * y) & 255
            sample = 1 - slot if red else (255 - byte if slot else byte) / 255
            ref.append(int(sample * scale >= threshold))
    return cb + ref


OP_CASES = (
    (1, 0),
    (1, 1),
    (1, 0.5),
    (-1, -1),
    (-1, 0),
    (2, 1.5),
    (math.inf, 0.25),
    (math.inf, -0.25),
)


def f32(value):
    if math.isnan(value) or math.isinf(value):
        return value
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def op_case(slot, scale, limit, relation, legacy=False):
    # An independent CPU model: round the unorm sample and the product to
    # binary32, then kill a pixel when the relation holds with the limit first.
    kill = getattr(operator, relation)
    cb = bytearray(256)
    struct.pack_into("<f", cb, 4, scale)
    struct.pack_into("<f", cb, 24, limit)
    ref = bytearray()
    for y in range(8):
        for x in range(8):
            byte = (41 * x + 19 * y) & 255
            sample = f32((255 - byte if slot else byte) / 255)
            product = 0.0 if legacy and (scale == 0 or sample == 0) else f32(scale * sample)
            ref.append(int(not kill(limit, product)))
    return cb + ref


def distinguishing_case(slot, relation, legacy, other, other_legacy, cases=OP_CASES):
    for scale, limit in cases:
        if op_case(slot, scale, limit, relation, legacy) != op_case(
            slot, scale, limit, other, other_legacy
        ):
            return scale, limit
    raise AssertionError(f"{relation} and {other} are not told apart on texture {slot}")


# The e64 readbacks use the cases that separate equality, sign, infinity and
# the legacy zero rule, so the larger program set stays inside the CTest limit.
E64_CASES = (OP_CASES[1], OP_CASES[2], OP_CASES[4], OP_CASES[6], OP_CASES[7])


def draw_ops(tmp, dxc, renderer, adapter):
    # (program, texture slots, legacy multiply, relation, cases)
    runs = [
        (program, range(2), False, relation, OP_CASES)
        for relation, program in OP_PROGRAMS.items()
    ] + [
        (program, range(1), legacy, relation, E64_CASES)
        for (legacy, _, _, relation), program in E64_PROGRAMS.items()
    ]
    for program, slots, legacy, relation, cases in runs:
        for slot in slots:
            src, ps, ref = tmp / "draw.hlsl", tmp / "draw.dxil", tmp / "samples.bin"
            src.write_text(lower(decoded(program), bindings(slot)))
            subprocess.run([dxc, "-T", "ps_6_0", "-E", "main", "-Fo", ps, src], check=True)
            args = [renderer, adapter, "--sampled-coverage", ps, ref]
            for scale, limit in cases:
                ref.write_bytes(op_case(slot, scale, limit, relation, legacy))
                result = subprocess.run(args)
                if result.returncode == 77:
                    return 77
                result.check_returncode()
            wrong = [(other, legacy) for other in RELATIONS if other != relation]
            if slot == 0:
                wrong.append((relation, not legacy))
            for other, other_legacy in wrong:
                scale, limit = distinguishing_case(
                    slot, relation, legacy, other, other_legacy, cases
                )
                ref.write_bytes(op_case(slot, scale, limit, other, other_legacy))
                result = subprocess.run(args, capture_output=True, text=True)
                if result.returncode != 1 or "Sample coverage mismatch" not in result.stderr:
                    raise AssertionError(
                        f"A {other} reference must fail the {relation} coverage readback"
                    )
    return 0


def draw(tmp, dxc, renderer, adapter, sources, scale_word, threshold_word, red=False, level=None):
    cases = ((1, 0.75),) if red else (
        (1, 0.5), (0.5, 0.375), (1, 0), (0, 0.25), (-1, -0.5)
    )
    for slot, source in enumerate(sources):
        src, ps = tmp / "draw.hlsl", tmp / "draw.dxil"
        src.write_text(source)
        subprocess.run([dxc, "-T", "ps_6_0", "-E", "main", "-Fo", ps, src], check=True)
        for scale, threshold in cases:
            ref = tmp / "samples.bin"
            ref.write_bytes(case(slot, scale, threshold, scale_word, threshold_word, red, level))
            args = [renderer, adapter, "--sampled-coverage", ps, ref]
            result = subprocess.run(args)
            if result.returncode == 77:
                return 77
            result.check_returncode()
        scale, threshold = cases[0]
        ref.write_bytes(case(1 - slot, scale, threshold, scale_word, threshold_word, red, level))
        wrong = subprocess.run(args, capture_output=True, text=True)
        if wrong.returncode != 1 or "Sample coverage mismatch" not in wrong.stderr:
            raise AssertionError(
                "A reference for the other texture must fail at coverage readback"
            )
    return 0


if __name__ == "__main__":
    controls()
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        if sys.argv[1] == "binary":
            binary(tmp, *sys.argv[2:])
        else:
            sys.exit(
                draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(PROGRAM), bindings(slot)) for slot in range(2)],
                    1,
                    6,
                )
                or draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(RED_PROGRAM), red_bindings(slot)) for slot in range(2)],
                    1,
                    10,
                    red=True,
                )
                or draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(RED_PROGRAM), red_bindings(0, scale_base=2))],
                    3,
                    10,
                    red=True,
                )
                or draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(LZ_PROGRAM), bindings(0))],
                    1,
                    6,
                )
                # The level word holds 3, past the only mip, so it clamps to mip 0.
                or draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(LEVEL_PROGRAM), bindings(0))],
                    1,
                    6,
                    level=3.0,
                )
                # A bias of 3 added to the only mip's level still selects mip 0.
                or draw(
                    tmp,
                    *sys.argv[1:],
                    [lower(decoded(BIAS_PROGRAM), bindings(0))],
                    1,
                    6,
                    level=3.0,
                )
                or draw_ops(tmp, *sys.argv[1:])
            )
