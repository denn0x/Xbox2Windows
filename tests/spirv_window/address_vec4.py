"""Lower, validate and run the source-built four-float address-window fixture.

Usage: address_vec4.py <spirv-as> <spirv-val> <spirv-cross> [<dxc> <offscreen_check> <hardware|warp>]

The fixture loads two four-float vectors through a direct physical pointer and through an array-element pointer,
builds a third vector from the operand and lanes of both, and stores all three through a direct pointer and two
array-element pointers. Every access is a plain 16-byte load or store, so the lowering must turn it into four DWORD
accesses under one guard: the whole vector is accessed or none of it is. A vector that is misaligned, crosses the
window end or whose index addition wraps loads as the zero vector (every lane the bit pattern zero) and a store to
it changes nothing, not even a lane.
With three arguments the test checks the lowering's shape, the guard against an independent model of the window
edge, the HLSL, the rejected forms and a Python model of the rule. With six it also runs the D3D12 readback, whose
CPU model in readback.cpp shares no code with the lowering, in the production window and in a narrow window that
ends inside the 64-byte view, and then each negative control: a mutation of the lowered module that must first
diverge at a predicted case and word, which the Python model derives independently of the mutation text.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

assembler, validator, cross, *gpu = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools"))
from spirv_address_window import lower

SOURCE = Path(__file__).with_suffix(".spvasm").read_text(encoding="utf-8")
PRODUCTION = "--spirv-address-window-vec4"
NARROW = "--spirv-address-window-vec4-narrow"
SKIP = 77

# Independent model of the rule. A vector is used only when its address is a multiple of 16 and all 16 bytes lie
# below the window end, and for an indexed pointer the 64-bit base plus the scaled index does not wrap. The 64-byte
# view then returns zero for a read past it and discards a write past it. All loads happen before any store.
ARENA_FILL, PI, NEGATIVE_NAN, NEGATIVE_ZERO = 0xcba98765, 0x40490fdb, 0xffc00001, 0x80000000
WINDOW_END, NARROW_WINDOW_END, VIEW_BYTES, MASK64 = 0xfffffffc, 44, 64, (1 << 64) - 1
ONE = 0x3f800000
CASES = (  # name, (base low, operand bits, destination byte address, base high)
    ("vec4 aligned", (16, PI, 0, 0)),
    ("vec4 NaN operand", (16, NEGATIVE_NAN, 0, 0)),
    ("vec4 negative zero operand", (16, NEGATIVE_ZERO, 0, 0)),
    ("vec4 base misaligned by 4", (20, PI, 0, 0)),
    ("vec4 base misaligned by 8", (24, PI, 0, 0)),
    ("vec4 base misaligned by 12", (28, PI, 0, 0)),
    ("vec4 load in the last vector of the view", (32, PI, 0, 0)),
    ("vec4 load across the end of the descriptor", (48, PI, 0, 0)),
    ("vec4 last vector of the window", (0xffffffd0, PI, 0, 0)),
    ("vec4 straddling the end of the window", (0xffffffe0, PI, 0, 0)),
    ("vec4 starting at the end of the window", (0xfffffff0, PI, 0, 0)),
    ("vec4 index addition wraps", (0xfffffff0, PI, 0, 0xffffffff)),
    ("vec4 misaligned write by 4", (16, PI, 4, 0)),
    ("vec4 misaligned write by 8", (16, PI, 8, 0)),
    ("vec4 misaligned write by 12", (16, PI, 12, 0)),
    ("vec4 write beyond descriptor", (16, PI, 64, 0)),
    ("vec4 write above the window", (16, PI, 0xfffffff0, 0)),
    ("vec4 write across the end of the descriptor", (16, PI, 48, 0)),
)
NARROW_CASES = (
    ("narrow vec4 below the end", (0, PI, 0, 0)),
    ("narrow vec4 load across the end", (16, PI, 0, 0)),
    ("narrow vec4 load past the end", (32, PI, 0, 0)),
    ("narrow vec4 store across the end", (0, PI, 16, 0)),
    ("narrow vec4 store past the end", (0, PI, 48, 0)),
)
CASE_SETS = {PRODUCTION: (CASES, WINDOW_END), NARROW: (NARROW_CASES, NARROW_WINDOW_END)}


def initial_arena():
    """Four vectors of four distinct DWORDs: lanes must not be confusable with each other or with the fill."""
    return [0x11112222, 0x33334444, 0x55556666, 0x77778888,
            0x7fc01234, 0x80000000, 0x3fc00000, 0xffc0abcd,
            0x3f99999a, 0xbf800000, 0x7f800000, 0x7f7fffff,
            ARENA_FILL, ARENA_FILL, ARENA_FILL, ARENA_FILL]


def fits(address, window_end):
    """Whether a 16-byte access at this address is aligned and lies wholly inside the window."""
    return address % 16 == 0 and address + 16 <= window_end


def usable(address, anchor, mutation, guard, window_end):
    """Whether the lowered guard lets this address through; a mutation removes part of one named guard."""
    aligned = address % 16 == 0 or mutation == "alignment of the first word only" and address % 4 == 0 \
        or mutation == f"{guard} alignment dropped"
    inside = address < window_end if mutation == "window end of the first word only" else address + 16 <= window_end
    return aligned and inside and not (address < anchor and mutation != f"{guard} wrap dropped")


def expected_words(case, mutation=None, window_end=WINDOW_END):
    """The arena after one case.

    First case: base 16 loads A from bytes 16..31 (the second vector) and B from bytes 32..47 (the third), builds
    M = (operand, B.z, A.y, A.x), stores A at destination 0, B at 16 and M at 32."""
    low, operand, destination, high = case
    base = (high << 32) | low
    arena = initial_arena()
    loaded = []
    for address, guard in ((base, "first load"), ((base + 16) & MASK64, "second load")):
        lanes = [0, 0, 0, 0]
        if usable(address, base, mutation, guard, window_end):
            if address + 16 <= VIEW_BYTES:
                lanes = arena[address // 4:address // 4 + 4]
        elif mutation == "invalid load keeps lane 3 nonzero":
            lanes = [0, 0, 0, ONE]
        loaded.append(lanes)
    if mutation == "load lanes swapped":
        loaded[0] = [loaded[0][1], loaded[0][0], *loaded[0][2:]]
    first, second = loaded
    mixed = [operand, second[2], first[1], first[0]]
    result = list(arena)
    for number, lanes in enumerate((first, second, mixed)):
        if number == 2 and mutation == "store lanes swapped":
            lanes = [*lanes[:3], lanes[2]]
        address = destination + 16 * number
        if usable(address, destination, mutation, f"store {number + 1}", window_end) and address + 16 <= VIEW_BYTES:
            result[address // 4:address // 4 + 4] = lanes
    return result


# name, flag, (old, new) edit of the lowered module, model mutation, case, first diverging word
LOAD = "%x2w_vec4_result_first"
CONTROLS = (
    ("load lanes swapped", PRODUCTION, (f"{LOAD}_value = OpCompositeConstruct %v4float {LOAD}_lane_0 {LOAD}_lane_1 ",
                                        f"{LOAD}_value = OpCompositeConstruct %v4float {LOAD}_lane_1 {LOAD}_lane_0 "),
     "load lanes swapped", "vec4 aligned", 0),
    ("store lanes swapped", PRODUCTION, ("%x2w_vec4_store_3_lane_3 = OpCompositeExtract %float %mixed 3",
                                         "%x2w_vec4_store_3_lane_3 = OpCompositeExtract %float %mixed 2"),
     "store lanes swapped", "vec4 aligned", 11),
    ("alignment checks only the first word", PRODUCTION, ("%x2w_mask_vec4 = OpConstant %ulong 15",
                                                          "%x2w_mask_vec4 = OpConstant %ulong 3"),
     "alignment of the first word only", "vec4 base misaligned by 4", 0),
    ("window end checks only the first word", NARROW, ("%x2w_end_vec4 = OpConstant %ulong 32",
                                                       "%x2w_end_vec4 = OpConstant %ulong 44"),
     "window end of the first word only", "narrow vec4 below the end", 8),
    ("misaligned store not dropped", PRODUCTION, ("OpBranchConditional %x2w_address_output_first_valid ",
                                                  "OpBranchConditional %x2w_address_output_first_range "),
     "store 1 alignment dropped", "vec4 misaligned write by 4", 1),
    ("second load wrap guard dropped", PRODUCTION, ("OpBranchConditional %x2w_address_second_source_checked ",
                                                    "OpBranchConditional %x2w_address_second_source_valid "),
     "second load wrap dropped", "vec4 index addition wraps", 4),
    ("invalid load keeps lane 3 nonzero", PRODUCTION,
     ("%x2w_vec4_zero = OpConstantNull %v4float", "%x2w_lane_zero = OpConstantNull %float\n"
      "%x2w_lane_one = OpConstant %float 1\n"
      "%x2w_vec4_zero = OpConstantComposite %v4float %x2w_lane_zero %x2w_lane_zero %x2w_lane_zero %x2w_lane_one"),
     "invalid load keeps lane 3 nonzero", "vec4 base misaligned by 4", 3),
)
# Each access: its generated stem, the guard that must precede its issue block, and for a store the vector stored.
LOADS = (("first", "first_source", "valid"), ("second", "second_source", "checked"))
STORES = (("1", "output_first_valid", "%first"), ("2", "tail_1_checked", "%second"), ("3", "tail_2_checked", "%mixed"))
POINTERS = ("first_source", "second_source", "output_first", "tail_1", "tail_2")
CONSTANTS = ("%x2w_shift_vec4 = OpConstant %uint 4", "%x2w_mask_vec4 = OpConstant %ulong 15",
             "%x2w_end_vec4 = OpConstant %ulong {end}", "%x2w_vec4_zero = OpConstantNull %v4float")
PRODUCTION_END, NARROW_END = 4294967280, 32


def model_errors():
    """Return what is wrong with the Python model itself; empty when it agrees with the hand-worked cases."""
    errors = []
    arena = initial_arena()
    first, second = arena[4:8], arena[8:12]
    mixed = [PI, second[2], first[1], first[0]]
    if expected_words(CASES[0][1]) != [*first, *second, *mixed, *arena[12:]]:
        errors.append("the aligned case does not store the two vectors and the mixed vector")
    zero = [0, 0, 0, 0]
    for number in (3, 4, 5):
        if expected_words(CASES[number][1]) != [*zero, *zero, PI, 0, 0, 0, *arena[12:]]:
            errors.append(f"{CASES[number][0]}: a misaligned base must load the zero vector twice")
    for number in (12, 13, 14):
        if expected_words(CASES[number][1]) != arena:
            errors.append(f"{CASES[number][0]}: a misaligned destination must not be written")
    if expected_words(CASES[1][1])[8] != NEGATIVE_NAN or expected_words(CASES[2][1])[8] != NEGATIVE_ZERO:
        errors.append("a NaN payload or a negative zero was not kept")
    if expected_words(CASES[15][1]) != arena or expected_words(CASES[16][1]) != arena:
        errors.append("a write past the descriptor or above the window must change nothing")
    # The window edge, derived from the 0xfffffffc window end and not from the lowering's constant.
    edge = [address for address in range(0xffffffc0, 0x100000000, 4) if fits(address, WINDOW_END)]
    if max(edge) != 0xffffffe0 or 0xfffffff0 in edge or 0xffffffec in edge:
        errors.append("the last valid vector must start at 0xffffffe0")
    if [a for a in range(0, 64, 4) if fits(a, NARROW_WINDOW_END)] != [0, 16]:
        errors.append("a window ending at byte 44 holds the vectors at 0 and 16 only")
    for name, flag, _, mutation, case, word in CONTROLS:
        cases, end = CASE_SETS[flag]
        found = [(label, number) for label, params in cases
                 for number, (a, b) in enumerate(zip(expected_words(params, None, end), expected_words(params, mutation, end)))
                 if a != b]
        if not found or found[0] != (case, word):
            errors.append(f"model predicts {found[:1]} for '{name}', not {(case, word)}")
    return errors


def guard_errors(lowered, window_end=WINDOW_END):
    """Compare the lowering's guard constants with the model over every DWORD address near the window end."""
    errors = []
    constants = dict(re.findall(r"(%x2w_(?:mask|end)_vec4) = OpConstant %ulong (\d+)", lowered))
    mask, end = int(constants["%x2w_mask_vec4"]), int(constants["%x2w_end_vec4"])
    for address in [*range(0xffffffa0, 0x100000000, 4), *range(0, 64, 4)]:
        if (address & mask == 0 and address < end) != fits(address, window_end):
            errors.append(f"guard and model disagree at {address:#x}")
    return errors


def shape_errors(lowered, end=PRODUCTION_END):
    """Return what is wrong with the lowered module; empty when it has the intended shape."""
    errors = []
    lines = lowered.splitlines()
    for constant in CONSTANTS:
        if constant.format(end=end) not in lines:
            errors.append(f"missing {constant.format(end=end)}")
    for result, source, kind in LOADS:
        stem = f"%x2w_vec4_result_{result}"
        if f"OpBranchConditional %x2w_address_{source}_{kind} {stem}_issue {stem}_merge" not in lines:
            errors.append(f"{stem} is not guarded by its checked address")
        if not any(re.fullmatch(rf"%{result} = OpPhi %v4float {stem}_value {stem}_issue %x2w_vec4_zero \S+", line)
                   for line in lines):
            errors.append(f"%{result} does not yield the zero vector outside its branch")
        if f"{stem}_value = OpCompositeConstruct %v4float " + " ".join(f"{stem}_lane_{k}" for k in range(4)) not in lines:
            errors.append(f"{stem} does not build the vector in lane order")
        for lane in range(4):
            if f"{stem}_lane_{lane} = OpBitcast %float {stem}_bits_{lane}" not in lines:
                errors.append(f"{stem} lane {lane} is not a bit-cast of its word")
            if f"{stem}_bits_{lane} = OpLoad %uint {stem}_ptr_{lane}" not in lines:
                errors.append(f"{stem} lane {lane} is not a word load")
    for number, guard, value in STORES:
        stem = f"%x2w_vec4_store_{number}"
        if f"OpBranchConditional %x2w_address_{guard} {stem}_issue {stem}_merge" not in lines:
            errors.append(f"{stem} is not guarded by {guard}")
        for lane in range(4):
            if f"{stem}_lane_{lane} = OpCompositeExtract %float {value} {lane}" not in lines:
                errors.append(f"{stem} lane {lane} is not lane {lane} of {value}")
            if f"{stem}_bits_{lane} = OpBitcast %uint {stem}_lane_{lane}" not in lines:
                errors.append(f"{stem} lane {lane} is not bit-cast to a word")
            if f"OpStore {stem}_ptr_{lane} {stem}_bits_{lane} Aligned 4" not in lines:
                errors.append(f"{stem} lane {lane} is not a word store")
    for stem in [f"%x2w_vec4_result_{name}" for name, *_ in LOADS] + [f"%x2w_vec4_store_{n}" for n, *_ in STORES]:
        for lane in range(4):
            word = "%x2w_address_" + {"first": "first_source", "second": "second_source", "1": "output_first",
                                      "2": "tail_1", "3": "tail_2"}[stem.rsplit("_", 1)[1]] + "_word"
            want = word if lane == 0 else f"{stem}_word_{lane}"
            if f"{stem}_ptr_{lane} = OpAccessChain %_ptr_PSB_uint %x2w_heap %c0 {want}" not in lines:
                errors.append(f"{stem} lane {lane} does not address its own DWORD")
            if lane and f"{stem}_word_{lane} = OpIAdd %uint {word} {('%x2w_one', '%x2w_two', '%x2w_three')[lane - 1]}" not in lines:
                errors.append(f"{stem} lane {lane} is not {lane} DWORDs after the first")
    label = None
    for line in lines:
        if opened := re.fullmatch(r"(%\S+) = OpLabel", line):
            label = opened[1]
        if "%x2w_vec4_" in line and ("%x2w_heap" in line or line.startswith("OpStore ") or " = OpLoad " in line):
            if not re.fullmatch(r"%x2w_vec4_\w+_issue", label or ""):
                errors.append(f"{line} runs outside its guarded branch")
    if any(" %v4float " in line and (" = OpLoad " in line or line.startswith("OpStore ")) for line in lines):
        errors.append("a vector access reaches the heap without being split into DWORDs")
    if any(f"%x2w_address_{name}_selected" in lowered for name in POINTERS) or "PhysicalStorageBuffer" in lowered:
        errors.append("a vector access uses the substitute index or a physical pointer remains")
    return errors


def assemble(source, module):
    subprocess.run([assembler, "--target-env", "vulkan1.2", source, "-o", module], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)


def mutate(assembly, old, new):
    if assembly.count(old) != 1:
        raise ValueError(f"expected exactly one {old!r} in the lowered module")
    return assembly.replace(old, new)


def to_hlsl(directory, name, assembly):
    source, module, hlsl = (directory / f"{name}{suffix}" for suffix in (".spvasm", ".spv", ".hlsl"))
    source.write_text(assembly, encoding="utf-8")
    assemble(source, module)
    subprocess.run([cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl], check=True)
    return hlsl


# The source's entry block, branching into a loop whose header loads a vector before OpLoopMerge.
LOOP_HEADER = """OpBranch %header
%header = OpLabel
%in_header = OpLoad %v4float %first_source Aligned 16
%in_x = OpCompositeExtract %float %in_header 0
%in_loop = OpFOrdLessThan %bool %in_x %operand
OpLoopMerge %exit %back None
OpBranch %body
%body = OpLabel
OpBranchConditional %in_loop %exit %back
%back = OpLabel
OpBranch %header
%exit = OpLabel
OpReturn
OpFunctionEnd"""
MISSING_ARRAY = "unsupported physical pointer or scalar layout"
UNHANDLED_USE = "unhandled physical word pointer use"
UNHANDLED_POINTER = "unhandled physical pointer operation"


def other_vector(declaration, name):
    """A cast to a physical pointer of another vector type, which is not part of this lowering."""
    pointer = f"%_ptr_PSB_{name}"
    return ("%array = OpConvertUToPtr", f"{declaration}\n{pointer} = OpTypePointer PhysicalStorageBuffer %{name}\n"
            f"%other_pointer = OpConvertUToPtr {pointer} %base\n%array = OpConvertUToPtr")


# name, (old, new) edit of the source, message the lowering must give. None of these is valid vec4 lowering input.
REJECTIONS = (
    ("a volatile load", ("OpLoad %v4float %first_source Aligned 16", "OpLoad %v4float %first_source Volatile"), UNHANDLED_USE),
    ("a load aligned to four", ("OpLoad %v4float %first_source Aligned 16", "OpLoad %v4float %first_source Aligned 4"),
     UNHANDLED_USE),
    ("a load with no alignment", ("OpLoad %v4float %first_source Aligned 16", "OpLoad %v4float %first_source"),
     UNHANDLED_USE),
    ("a store aligned to four", ("OpStore %output_first %first Aligned 16", "OpStore %output_first %first Aligned 4"),
     UNHANDLED_USE),
    ("an atomic exchange", ("%first = OpLoad %v4float %first_source Aligned 16",
                            "%first = OpAtomicExchange %v4float %first_source %c1 %c0 %mixed"), UNHANDLED_USE),
    ("a copy of a vector pointer", ("%first_x = OpCompositeExtract", "%alias = OpCopyObject %_ptr_PSB_v4float %first_source\n"
                                    "%first_x = OpCompositeExtract"), UNHANDLED_USE),
    ("a vec2 float pointer", other_vector("%v2float = OpTypeVector %float 2", "v2float"), UNHANDLED_POINTER),
    ("a vec3 float pointer", other_vector("%v3float = OpTypeVector %float 3", "v3float"), UNHANDLED_POINTER),
    ("a vec4 unsigned pointer", other_vector("%v4uint = OpTypeVector %uint 4", "v4uint"), UNHANDLED_POINTER),
    ("a vec4 signed pointer", other_vector("%int = OpTypeInt 32 1\n%v4int = OpTypeVector %int 4", "v4int"),
     UNHANDLED_POINTER),
    ("a two-index vector access chain", ("%tail_1 = OpAccessChain %_ptr_PSB_v4float %output_tail %c1",
                                         "%tail_1 = OpAccessChain %_ptr_PSB_v4float %output_tail %c1 %c0"),
     UNHANDLED_POINTER),
    ("a vector access chain on a direct pointer", ("%tail_1 = OpAccessChain %_ptr_PSB_v4float %output_tail %c1",
                                                   "%tail_1 = OpAccessChain %_ptr_PSB_v4float %output_first %c1"),
     "unknown physical array pointer"),
    ("a vector load in a loop header", ("OpReturn\nOpFunctionEnd", LOOP_HEADER), "share a block with OpLoopMerge"),
    ("a missing vector array stride", ("OpDecorate %rt_v4float ArrayStride 16\n", ""), MISSING_ARRAY),
    ("a vector pointer to the wrong type", ("%_ptr_PSB_v4float = OpTypePointer PhysicalStorageBuffer %v4float",
                                            "%_ptr_PSB_v4float = OpTypePointer PhysicalStorageBuffer %uint"), MISSING_ARRAY),
)
# Unsigned, float and vector accesses in one module: the three lowerings must not disturb one another.
MIXED = (("OpDecorate %words ArrayStride 4\n", "OpDecorate %words ArrayStride 4\nOpDecorate %rt_float ArrayStride 4\n"),
         ("%words = OpTypeArray", "%rt_float = OpTypeRuntimeArray %float\n"
          "%_ptr_PSB_arrf = OpTypePointer PhysicalStorageBuffer %rt_float\n"
          "%_ptr_PSB_float = OpTypePointer PhysicalStorageBuffer %float\n%words = OpTypeArray"),
         ("OpReturn\nOpFunctionEnd", """%word_array = OpConvertUToPtr %_ptr_PSB_arr %base
%word_pointer = OpAccessChain %_ptr_PSB_uint %word_array %c1
%word = OpLoad %uint %word_pointer Aligned 4
OpStore %word_pointer %word Aligned 4
%float_array = OpConvertUToPtr %_ptr_PSB_arrf %base
%float_pointer = OpAccessChain %_ptr_PSB_float %float_array %c2
%float_value = OpLoad %float %float_pointer Aligned 4
OpStore %float_pointer %float_value Aligned 4
OpReturn
OpFunctionEnd"""))


def expect_rejection(old, new, message):
    try:
        lower(mutate(SOURCE, old, new))
    except ValueError as error:
        if message not in str(error):
            raise SystemExit(f"rejection of {new!r} gave {error}")
        return
    raise SystemExit(f"{new!r} was accepted")


def check_hlsl(hlsl):
    """The HLSL of the lowered module; SPIRV-Cross spells the bit-casts differently across releases."""
    if len(re.findall(r"\basuint\(", hlsl)) < 12 or "asfloat(" not in hlsl or len(re.findall(r"\.Store\(", hlsl)) != 12:
        sys.exit("expected twelve DWORD stores of asuint values (four for each of three vectors) and an asfloat")
    if len(re.findall(r"\.Load\(", hlsl)) < 8 + 4:
        sys.exit("expected eight DWORD loads for the two vectors")
    if "Interlocked" in hlsl:
        sys.exit("a vector access must not become an atomic")


with tempfile.TemporaryDirectory() as folder:
    directory = Path(folder)
    problems = model_errors()
    if problems:
        sys.exit("model: " + "; ".join(problems))
    print(f"model: hand-worked cases hold, and each of {len(CONTROLS)} mutations diverges at its predicted case and word")
    source = directory / "source.spvasm"
    source.write_text(SOURCE, encoding="utf-8")
    assemble(source, directory / "source.spv")
    lowered = lower(SOURCE)
    problems = guard_errors(lowered) + shape_errors(lowered)
    if problems:
        sys.exit("lowered module: " + "; ".join(problems))
    check_hlsl(to_hlsl(directory, "lowered", lowered).read_text(encoding="utf-8"))
    print("lowered module: validated, guarded as a whole, bit-cast lane by lane; guard agrees with the model at the window edge")
    for name, (old, new), message in REJECTIONS:
        expect_rejection(old, new, message)
        print(f"rejected {name}")
    mixed = SOURCE
    for old, new in MIXED:
        mixed = mutate(mixed, old, new)
    source.write_text(mixed, encoding="utf-8")
    assemble(source, directory / "mixed.spv")
    mixed_lowered = lower(mixed)
    mixed_hlsl = to_hlsl(directory, "mixed", mixed_lowered).read_text(encoding="utf-8")
    if shape_errors(mixed_lowered) or guard_errors(mixed_lowered) or "Interlocked" in mixed_hlsl:
        sys.exit("a uint or float access beside the vector accesses changed the vector lowering")
    if "%x2w_float_result_float_value_old" not in mixed_lowered or "%x2w_atomic_" in mixed_lowered:
        sys.exit("the mixed module did not lower its float access")
    print("validated a module that mixes uint, float and vec4 accesses")
    narrow = mutate(lowered, CONSTANTS[2].format(end=PRODUCTION_END), CONSTANTS[2].format(end=NARROW_END))
    to_hlsl(directory, "narrow", narrow)
    for index, (name, flag, (old, new), *_) in enumerate(CONTROLS):
        mutated = mutate(narrow if flag == NARROW else lowered, old, new)
        end, window_end = (NARROW_END, NARROW_WINDOW_END) if flag == NARROW else (PRODUCTION_END, WINDOW_END)
        if not shape_errors(mutated, end) + guard_errors(mutated, window_end):
            sys.exit(f"shape check missed '{name}'")
        to_hlsl(directory, f"mutation{index}", mutated)  # a mutation must still validate, or the GPU control proves nothing
    print(f"shape check caught all {len(CONTROLS)} mutations, each still a valid module")
    if gpu:
        dxc, renderer, adapter = gpu

        def run_gpu(flag, assembly, name):
            hlsl = to_hlsl(directory, name, assembly)
            dxil = directory / f"{name}.dxil"
            subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil, hlsl], check=True)
            return subprocess.run([renderer, adapter, flag, dxil], capture_output=True, text=True)

        for flag, assembly in ((PRODUCTION, lowered), (NARROW, narrow)):
            result = run_gpu(flag, assembly, flag.removeprefix("--spirv-address-window-"))
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode:
                sys.exit(result.returncode)
        for index, (name, flag, (old, new), _, case, word) in enumerate(CONTROLS):
            result = run_gpu(flag, mutate(narrow if flag == NARROW else lowered, old, new), f"control{index}")
            text = result.stdout + result.stderr
            if result.returncode == SKIP:
                sys.exit(SKIP)
            if not result.returncode or not re.search(rf"^{re.escape(case)} word {word} expected ", text, re.M):
                sys.stderr.write(text)
                sys.exit(f"negative control '{name}' did not first diverge at '{case}' word {word}")
            print(f"negative control '{name}': caught at '{case}' word {word}")
