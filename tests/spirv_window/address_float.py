"""Lower, validate and run the source-built float address-window fixture.

Usage: address_float.py <spirv-as> <spirv-val> <spirv-cross> [<dxc> <offscreen_check> <hardware|warp>]

The fixture loads three floats through a float-array physical pointer, copies them through two direct float
pointers and a float-array pointer, and stores a fourth float taken from the push constants. Every access is a
plain float load or store, so the lowering must move the bits between the unsigned heap words and the float
values with OpBitcast and must not change one of them: a NaN payload and the sign of a zero have to survive.
Outside the window or when misaligned, a load yields the bit pattern zero (0.0) and a store changes nothing.
With three arguments the test checks the lowering's shape, the HLSL, the rejected forms and a Python model of the
rule. With six it also runs the D3D12 readback, whose CPU model in readback.cpp shares no code with the lowering,
and then each negative control: a mutation of the lowered module that must first diverge at a predicted case
and word, which the Python model derives independently of the mutation text.
"""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

assembler, validator, cross, *gpu = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools"))
from spirv_address_window import lower

SOURCE = Path(__file__).with_suffix(".spvasm").read_text(encoding="utf-8")
FLAG = "--spirv-address-window-float"

# Independent model of the rule. An address is used only when it is a multiple of four, below 0xfffffffc and, for
# an indexed pointer, the 64-bit base plus the scaled index does not wrap. The 64-byte view then returns zero for a
# read past it and discards a write past it. All loads happen before any store.
ARENA_FILL, NAN_PAYLOAD, NEGATIVE_ZERO, ONE_AND_A_HALF = 0xcba98765, 0x7fc01234, 0x80000000, 0x3fc00000
PI, NEGATIVE_NAN = 0x40490fdb, 0xffc00001
WINDOW_END, VIEW_BYTES, MASK64 = 0xfffffffc, 64, (1 << 64) - 1
CASES = (  # name, (base low, operand bits, destination byte address, base high)
    ("float aligned", (8, PI, 44, 0)),
    ("float NaN operand", (8, NEGATIVE_NAN, 44, 0)),
    ("float negative zero operand", (8, NEGATIVE_ZERO, 44, 0)),
    ("float misaligned base", (9, PI, 44, 0)),
    ("float window end", (0xfffffff4, PI, 44, 0)),
    ("float base at the end of the window", (0xfffffffc, PI, 44, 0)),
    ("float index addition wraps", (0xfffffffc, PI, 44, 0xffffffff)),
    ("float misaligned write", (8, PI, 45, 0)),
    ("float write beyond descriptor", (8, PI, 64, 0)),
    ("float write above the window", (8, PI, 0xfffffffc, 0)),
    ("float write straddling the end of the descriptor", (8, PI, 60, 0)),
)


def initial_arena():
    words = [ARENA_FILL] * 16
    words[3], words[4], words[5] = NAN_PAYLOAD, NEGATIVE_ZERO, ONE_AND_A_HALF
    return words


def usable(address, base, mutation, guard):
    """Whether the lowered guard lets this address through; a mutation removes part of one named guard."""
    aligned = address % 4 == 0 or mutation == f"{guard} alignment dropped"
    wrapped = address < base and mutation != f"{guard} wrap dropped"
    return aligned and address < WINDOW_END and not wrapped and mutation != f"{guard} all dropped"


def expected_words(case, mutation=None):
    """The arena after one case.

    First case: base 8 loads the addresses 12, 16 and 20, which are words 3, 4 and 5 (NaN with a payload, -0.0 and
    1.5). It stores them at destination 44, 48 and 52 (words 11, 12 and 13) and the operand at 56 (word 14)."""
    low, operand, destination, high = case
    base = (high << 32) | low
    arena = initial_arena()
    loaded = []
    for k, guard in ((1, "first load"), (2, "second load"), (3, "third load")):
        address = (base + 4 * k) & MASK64
        bits = 0
        if usable(address, base, mutation, guard) and address + 4 <= VIEW_BYTES:
            bits = arena[address // 4]
        elif mutation == f"{guard} reads garbage":
            bits = 0x3fffffff
        if mutation == "loads converted":
            bits = struct.unpack("<I", struct.pack("<f", float(bits)))[0]
        loaded.append(bits)
    stored = [loaded[1] if mutation == "first store swapped" else loaded[0], loaded[1], loaded[2], operand]
    if mutation == "operand store converted":
        # The conversion saturates at zero and truncates toward zero, as D3D does for a float to unsigned cast.
        value = struct.unpack("<f", struct.pack("<I", operand))[0]
        stored[3] = 0 if value != value or value < 0 else int(value)
    result = list(arena)
    for i, bits in enumerate(stored):
        address = destination + 4 * i
        if usable(address, destination, mutation, f"store {i + 1}") and address + 4 <= VIEW_BYTES:
            result[address // 4] = bits
    return result


# name, (old, new) edit of the lowered module, model mutation, case, first diverging word
LOAD_1, LOAD_2 = "%x2w_float_result_first", "%x2w_float_result_second"
CONTROLS = (
    ("load bit-cast replaced by a numeric conversion", ("%first = OpBitcast %float " + LOAD_1 + "_old",
                                                        "%first = OpConvertUToF %float " + LOAD_1 + "_old"),
     "loads converted", "float aligned", 11),
    ("operand store bit-cast replaced by a numeric conversion", ("%x2w_float_store_4_value = OpBitcast %uint %operand",
                                                                "%x2w_float_store_4_value = OpConvertFToU %uint %operand"),
     "operand store converted", "float aligned", 14),
    ("first store writes the second value", ("%x2w_float_store_1_value = OpBitcast %uint %first",
                                             "%x2w_float_store_1_value = OpBitcast %uint %second"),
     "first store swapped", "float aligned", 11),
    ("first load alignment guard dropped", ("OpBranchConditional %x2w_address_first_source_checked ",
                                            "OpBranchConditional %x2w_address_first_source_range "),
     "first load alignment dropped", "float misaligned base", 11),
    ("second load wrap guard dropped", ("OpBranchConditional %x2w_address_second_source_checked ",
                                        "OpBranchConditional %x2w_address_second_source_valid "),
     "second load wrap dropped", "float index addition wraps", 12),
    ("out-of-window load yields garbage instead of zero", (LOAD_2 + "_issue %c0 ", LOAD_2 + "_issue %x2w_invalid "),
     "second load reads garbage", "float misaligned base", 12),
    ("first store alignment guard dropped", ("OpBranchConditional %x2w_address_output_first_valid ",
                                             "OpBranchConditional %x2w_address_output_first_range "),
     "store 1 alignment dropped", "float misaligned write", 11),
)
# Each access: its generated stem, the guard that must precede its issue block, and for a store the bit-cast source.
LOADS = (("first", "first_source"), ("second", "second_source"), ("third", "third_source"))
STORES = (("1", "output_first_valid", "%first"), ("2", "output_second_valid", "%second"),
          ("3", "tail_0_checked", "%third"), ("4", "tail_1_checked", "%operand"))


FLOAT_POINTERS = ("first_source", "second_source", "third_source", "output_first", "output_second", "tail_0", "tail_1")


def model_errors():
    """Return what is wrong with the Python model itself; empty when it agrees with the hand-worked cases."""
    errors = []
    first = expected_words(CASES[0][1])
    if first[11:15] != [NAN_PAYLOAD, NEGATIVE_ZERO, ONE_AND_A_HALF, PI] or first[:11] != initial_arena()[:11]:
        errors.append("the aligned case does not copy the three words and the operand")
    if expected_words(CASES[3][1])[11:15] != [0, 0, 0, PI]:
        errors.append("a misaligned base must load 0.0 three times")
    if expected_words(CASES[1][1])[14] != NEGATIVE_NAN or expected_words(CASES[2][1])[14] != NEGATIVE_ZERO:
        errors.append("a NaN payload or a negative zero was not kept")
    # Destination 45 is misaligned for the first store and for the next two; only the fourth, at 57, is also misaligned.
    if expected_words(CASES[7][1]) != initial_arena():
        errors.append("a misaligned destination must not be written")
    for name, _, mutation, case, word in CONTROLS:
        found = [(label, number) for label, params in CASES
                 for number, (a, b) in enumerate(zip(expected_words(params), expected_words(params, mutation)))
                 if a != b]
        if not found or found[0] != (case, word):
            errors.append(f"model predicts {found[:1]} for '{name}', not {(case, word)}")
    return errors


def shape_errors(lowered):
    """Return what is wrong with the lowered module; empty when it has the intended shape."""
    errors = []
    lines = lowered.splitlines()
    for result, source in LOADS:
        stem = f"%x2w_float_result_{result}"
        if f"OpBranchConditional %x2w_address_{source}_checked {stem}_issue {stem}_merge" not in lines:
            errors.append(f"{stem} is not guarded by its checked address")
        if not any(re.fullmatch(rf"{stem}_old = OpPhi %uint {stem}_loaded {stem}_issue %c0 \S+", line) for line in lines):
            errors.append(f"{stem} does not yield the bit pattern zero outside its branch")
        if f"%{result} = OpBitcast %float {stem}_old" not in lines:
            errors.append(f"%{result} is not a bit-cast of the loaded word")
    for number, guard, value in STORES:
        stem = f"%x2w_float_store_{number}"
        if f"OpBranchConditional %x2w_address_{guard} {stem}_issue {stem}_merge" not in lines:
            errors.append(f"{stem} is not guarded by {guard}")
        if f"{stem}_value = OpBitcast %uint {value}" not in lines:
            errors.append(f"{stem} does not store the bits of {value}")
    label = None
    for line in lines:
        if opened := re.fullmatch(r"(%\S+) = OpLabel", line):
            label = opened[1]
        if "%x2w_float_" in line and ("%x2w_heap" in line or line.startswith("OpStore ") or " = OpLoad " in line):
            if not re.fullmatch(r"%x2w_float_\w+_issue", label or ""):
                errors.append(f"{line} runs outside its guarded branch")
    if any(" %float " in line and (" = OpLoad " in line or line.startswith("OpStore ")) for line in lines):
        errors.append("a float access reaches the heap without a bit-cast")
    if any(f"%x2w_address_{name}_selected" in lowered for name in FLOAT_POINTERS) or "PhysicalStorageBuffer" in lowered:
        errors.append("a float access uses the substitute index or a physical pointer remains")
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


# The source's entry block, branching into a loop whose header loads through a float pointer before OpLoopMerge.
LOOP_HEADER = """OpBranch %header
%header = OpLabel
%in_header = OpLoad %float %first_source Aligned 4
%in_loop = OpFOrdLessThan %bool %in_header %operand
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
# name, (old, new) edit of the source, message the lowering must give. None of these is valid float lowering input.
REJECTIONS = (
    ("a volatile load", ("OpLoad %float %first_source Aligned 4", "OpLoad %float %first_source Volatile"), UNHANDLED_USE),
    ("an unaligned store", ("OpStore %output_first %first Aligned 4", "OpStore %output_first %first Aligned 2"),
     UNHANDLED_USE),
    ("a float atomic exchange", ("%first = OpLoad %float %first_source Aligned 4",
                                 "%first = OpAtomicExchange %float %first_source %c1 %c0 %operand"), UNHANDLED_USE),
    ("a float load through an unsigned pointer", ("%first = OpLoad %float %first_source Aligned 4",
                                                  "%word_pointer = OpConvertUToPtr %_ptr_PSB_uint %base\n"
                                                  "%first = OpLoad %float %word_pointer Aligned 4"), UNHANDLED_USE),
    ("a half-float pointer", ("%array = OpConvertUToPtr", "%half = OpTypeFloat 16\n"
                              "%_ptr_PSB_half = OpTypePointer PhysicalStorageBuffer %half\n"
                              "%half_pointer = OpConvertUToPtr %_ptr_PSB_half %base\n%array = OpConvertUToPtr"),
     "unhandled physical pointer operation"),
    ("a double pointer", ("%array = OpConvertUToPtr", "%double = OpTypeFloat 64\n"
                          "%_ptr_PSB_double = OpTypePointer PhysicalStorageBuffer %double\n"
                          "%double_pointer = OpConvertUToPtr %_ptr_PSB_double %base\n%array = OpConvertUToPtr"),
     "unhandled physical pointer operation"),
    ("a two-index float access chain", ("%tail_0 = OpAccessChain %_ptr_PSB_float %output_tail %c0",
                                        "%tail_0 = OpAccessChain %_ptr_PSB_float %output_tail %c0 %c1"),
     "unhandled physical pointer operation"),
    ("a float access chain on a scalar pointer", ("%tail_0 = OpAccessChain %_ptr_PSB_float %output_tail %c0",
                                                  "%tail_0 = OpAccessChain %_ptr_PSB_float %output_first %c0"),
     "unknown physical array pointer"),
    ("a float load in a loop header", ("OpReturn\nOpFunctionEnd", LOOP_HEADER), "share a block with OpLoopMerge"),
    ("a missing float array stride", ("OpDecorate %rt_float ArrayStride 4\n", ""), MISSING_ARRAY),
    ("a float pointer to the wrong type", ("%_ptr_PSB_float = OpTypePointer PhysicalStorageBuffer %float",
                                           "%_ptr_PSB_float = OpTypePointer PhysicalStorageBuffer %uint"), MISSING_ARRAY),
)
# A uint array use next to the float accesses: the two lowerings must not disturb each other.
MIXED = ("OpReturn\nOpFunctionEnd", """%word_array = OpConvertUToPtr %_ptr_PSB_arr %base
%word_pointer = OpAccessChain %_ptr_PSB_uint %word_array %c1
%word = OpLoad %uint %word_pointer Aligned 4
OpStore %word_pointer %word Aligned 4
OpReturn
OpFunctionEnd""")


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
    if len(re.findall(r"\basuint\(", hlsl)) < 4 or "asfloat(" not in hlsl or len(re.findall(r"\.Store\(", hlsl)) != 4:
        sys.exit("expected four stores of asuint values and an asfloat")
    if "Interlocked" in hlsl:
        sys.exit("a float access must not become an atomic")


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
    problems = shape_errors(lowered)
    if problems:
        sys.exit("lowered module: " + "; ".join(problems))
    check_hlsl(to_hlsl(directory, "lowered", lowered).read_text(encoding="utf-8"))
    print("lowered module: validated, guarded, bit-cast")
    for name, (old, new), message in REJECTIONS:
        expect_rejection(old, new, message)
        print(f"rejected {name}")
    mixed = SOURCE.replace(*MIXED)
    source.write_text(mixed, encoding="utf-8")
    assemble(source, directory / "mixed.spv")
    mixed_lowered = lower(mixed)
    to_hlsl(directory, "mixed", mixed_lowered)
    if shape_errors(mixed_lowered):
        sys.exit("a uint access beside the float accesses changed the float lowering")
    print("validated a module that mixes uint and float accesses")
    for index, (name, (old, new), *_) in enumerate(CONTROLS):
        mutated = mutate(lowered, old, new)
        if not shape_errors(mutated):
            sys.exit(f"shape check missed '{name}'")
        to_hlsl(directory, f"mutation{index}", mutated)  # a mutation must still validate, or the GPU control proves nothing
    print(f"shape check caught all {len(CONTROLS)} mutations, each still a valid module")
    if gpu:
        dxc, renderer, adapter = gpu

        def run_gpu(assembly, name):
            hlsl = to_hlsl(directory, name, assembly)
            dxil = directory / f"{name}.dxil"
            subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil, hlsl], check=True)
            return subprocess.run([renderer, adapter, FLAG, dxil], capture_output=True, text=True)

        result = run_gpu(lowered, "window")
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode:
            sys.exit(result.returncode)
        for index, (name, (old, new), _, case, word) in enumerate(CONTROLS):
            result = run_gpu(mutate(lowered, old, new), f"control{index}")
            text = result.stdout + result.stderr
            if result.returncode == 77:
                sys.exit(77)
            if not result.returncode or not re.search(rf"^{re.escape(case)} word {word} expected ", text, re.M):
                sys.stderr.write(text)
                sys.exit(f"negative control '{name}' did not first diverge at '{case}' word {word}")
            print(f"negative control '{name}': caught at '{case}' word {word}")
