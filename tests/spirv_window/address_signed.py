"""Lower, validate and run the source-built signed DWORD address-window fixture.

Usage: address_signed.py <spirv-as> <spirv-val> <spirv-cross> [<dxc> <offscreen_check> <hardware|warp>]

The fixture performs OpAtomicSMin, OpAtomicSMax and a load through signed physical pointers, then runs OpAtomicIAdd,
OpAtomicExchange, OpAtomicAnd, OpAtomicOr, OpAtomicXor and two OpAtomicCompareExchange (one missing, one hitting) in
sequence on one signed word, and stores the ten old values through signed output pointers. With three arguments the test checks the lowering's shape and
that SPIRV-Cross emits a signed compare-exchange loop. With six it also runs the D3D12 readback, whose CPU
model in readback.cpp shares no code with the lowering, and then each negative control: a mutation of the
lowered module that must first diverge at a predicted case and word.
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
FLAG = "--spirv-address-window-signed"
MIN, MAX = "%x2w_signed_result_old_min", "%x2w_signed_result_old_max"
ADD, EXCHANGE, AND, OR, XOR, MISS, HIT = (f"%x2w_signed_result_old_{name}" for name in (
    "add", "exchange", "and", "or", "xor", "miss", "hit"))
# stem, atomic, value operand, comparator operand: the sequence runs on one word, so each step feeds the next.
SEQUENCE = ((ADD, "OpAtomicIAdd", "%operand", None), (EXCHANGE, "OpAtomicExchange", "%operand", None),
            (AND, "OpAtomicAnd", "%mask_high", None), (OR, "OpAtomicOr", "%bits_low", None),
            (XOR, "OpAtomicXor", "%xor_bits", None), (MISS, "OpAtomicCompareExchange", "%next_first", "%wrong"),
            (HIT, "OpAtomicCompareExchange", "%next_second", "%expected"))

# name, (old, new) edit of the lowered module, case, first diverging word
CONTROLS = (
    ("SMin compared as unsigned", (f"{MIN}_pick = OpSLessThan", f"{MIN}_pick = OpULessThan"),
     "signed positive operand", 2),
    ("SMax compared as unsigned", (f"{MAX}_pick = OpSGreaterThan", f"{MAX}_pick = OpUGreaterThan"),
     "signed positive operand", 3),
    ("SMin keeps the larger value", (f"{MIN}_pick = OpSLessThan", f"{MIN}_pick = OpSGreaterThan"),
     "signed positive operand", 2),
    ("SMax keeps the smaller value", (f"{MAX}_pick = OpSGreaterThan", f"{MAX}_pick = OpSLessThan"),
     "signed positive operand", 3),
    # The sequence's last word is word 5; an operation that changes it first diverges there.
    ("Xor replaced by Add", ("OpAtomicXor %uint", "OpAtomicIAdd %uint"), "signed positive operand", 5),
    ("Or replaced by And", ("OpAtomicOr %uint", "OpAtomicAnd %uint"), "signed positive operand", 5),
    ("Exchange replaced by Add", ("OpAtomicExchange %uint", "OpAtomicIAdd %uint"), "signed positive operand", 5),
    ("CompareExchange swaps value and comparator", (f"%c0 %c0 {HIT}_value {HIT}_cmp", f"%c0 %c0 {HIT}_cmp {HIT}_value"),
     "signed positive operand", 5),
    ("sequence alignment guard dropped", (f"OpBranchConditional %x2w_address_chain_target_checked {ADD}_issue",
                                          f"OpBranchConditional %x2w_address_chain_target_range {ADD}_issue"),
     "signed misaligned base", 5),
    ("tail store alignment guard dropped", ("OpBranchConditional %x2w_address_tail_0_checked ",
                                            "OpBranchConditional %x2w_address_tail_0_range "),
     "signed misaligned write", 9),
    ("atomic wrap guard dropped", (f"OpBranchConditional %x2w_address_min_target_checked {MIN}_issue",
                                   f"OpBranchConditional %x2w_address_min_target_valid {MIN}_issue"),
     "signed index addition wraps", 6),
    ("load alignment guard dropped", ("OpBranchConditional %x2w_address_load_target_checked ",
                                      "OpBranchConditional %x2w_address_load_target_range "),
     "signed misaligned base", 8),
    ("store alignment guard dropped", ("OpBranchConditional %x2w_address_output_first_valid ",
                                       "OpBranchConditional %x2w_address_output_first_range "),
     "signed misaligned write", 6),
)
# A guarded access: the branch condition that must precede its issue block.
GUARDS = {MIN: "%x2w_address_min_target_checked", MAX: "%x2w_address_max_target_checked",
          "%x2w_signed_result_loaded": "%x2w_address_load_target_checked",
          "%x2w_signed_store_1": "%x2w_address_output_first_valid",
          "%x2w_signed_store_2": "%x2w_address_output_second_valid",
          "%x2w_signed_store_3": "%x2w_address_output_third_valid",
          **{stem: "%x2w_address_chain_target_checked" for stem, *_ in SEQUENCE},
          **{f"%x2w_signed_store_{4 + n}": f"%x2w_address_tail_{n}_checked" for n in range(7)}}


def shape_errors(lowered):
    """Return what is wrong with the lowered module; empty when it has the intended shape."""
    errors = []
    lines = lowered.splitlines()
    for stem, condition in GUARDS.items():
        if f"OpBranchConditional {condition} {stem}_issue {stem}_merge" not in lines:
            errors.append(f"{stem} is not guarded by {condition}")
    for stem, compare in ((MIN, "OpSLessThan"), (MAX, "OpSGreaterThan")):
        if f"{stem}_pick = {compare} %bool {stem}_cur_int %operand" not in lines:
            errors.append(f"{stem} does not use {compare}")
    for stem, atomic, value, comparator in SEQUENCE:
        wiring = [f"{stem}_value = OpBitcast %uint {value}"]
        if comparator:
            wiring.append(f"{stem}_cmp = OpBitcast %uint {comparator}")
        tail = f"{stem}_value" + (f" {stem}_cmp" if comparator else "")
        if any(line not in lines for line in wiring) or not any(
                line.startswith(f"{stem}_old = {atomic} %uint ") and line.endswith(" " + tail) for line in lines):
            errors.append(f"{stem} is not {atomic} on {tail} with {value}")
    label = None
    for line in lines:
        if opened := re.fullmatch(r"(%\S+) = OpLabel", line):
            label = opened[1]
        if "%x2w_signed_" in line and ("%x2w_heap" in line or "OpAtomic" in line or " = OpLoad " in line
                                       or line.startswith("OpStore ")):
            if not re.fullmatch(r"%x2w_signed_\w+_(issue|head|body|cont|exit)", label or ""):
                errors.append(f"{line} runs outside its guarded branch")
    if any("OpAtomicS" in line and "CompareExchange" not in line for line in lines):
        errors.append("a signed atomic reaches the heap directly")
    if re.search(r"OpSelect %uint \S+ \S+ %x2w_invalid", lowered):
        errors.append("a signed access uses the substitute index")
    return errors


def assemble(source, module):
    subprocess.run([assembler, "--target-env", "vulkan1.2", source, "-o", module], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)


def edit(assembly, old, new):
    if assembly.count(old) != 1:
        raise ValueError(f"expected exactly one {old!r} in the lowered module")
    return assembly.replace(old, new)


def build(directory, name, assembly):
    source, module, hlsl = (directory / f"{name}{suffix}" for suffix in (".spvasm", ".spv", ".hlsl"))
    source.write_text(assembly, encoding="utf-8")
    assemble(source, module)
    subprocess.run([cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl], check=True)
    return hlsl


# The source's entry block, branching into a loop whose header loads through a signed pointer before OpLoopMerge.
LOOP_HEADER = """OpBranch %header
%header = OpLabel
%in_header = OpLoad %int %load_target Aligned 4
%in_loop = OpSLessThan %bool %in_header %operand
OpLoopMerge %exit %back None
OpBranch %body
%body = OpLabel
OpBranchConditional %in_loop %exit %back
%back = OpLabel
OpBranch %header
%exit = OpLabel
OpReturn
OpFunctionEnd"""


def expect_rejection(old, new, message):
    try:
        lower(edit(SOURCE, old, new))
    except ValueError as error:
        if message not in str(error):
            raise SystemExit(f"rejection of {new!r} gave {error}")
        return
    raise SystemExit(f"{new!r} was accepted")


with tempfile.TemporaryDirectory() as folder:
    directory = Path(folder)
    source = directory / "source.spvasm"
    source.write_text(SOURCE, encoding="utf-8")
    assemble(source, directory / "source.spv")
    lowered = lower(SOURCE)
    problems = shape_errors(lowered)
    if problems:
        sys.exit("lowered module: " + "; ".join(problems))
    hlsl = build(directory, "lowered", lowered).read_text(encoding="utf-8")
    # Both atomics become one signed compare-exchange loop each; no unsigned Interlocked min or max remains.
    if hlsl.count("InterlockedCompareExchange") != 4 or "InterlockedMin" in hlsl or "InterlockedMax" in hlsl:
        sys.exit("expected two compare-exchange loops, two compare-exchanges and no InterlockedMin or InterlockedMax")
    for name in ("Add", "Exchange", "And", "Or", "Xor"):
        if hlsl.count(f"Interlocked{name}(") != 1:
            sys.exit(f"expected one Interlocked{name}")
    print("lowered module: validated, guarded, signed compare-exchange loops")
    for name, (old, new) in (("a signed subtract", ("OpAtomicSMin %int %min_target", "OpAtomicISub %int %min_target")),
                             ("a volatile load", ("OpLoad %int %load_target Aligned 4", "OpLoad %int %load_target Volatile")),
                             ("an unaligned store", ("OpStore %output_first %old_min Aligned 4",
                                                     "OpStore %output_first %old_min Aligned 2")),
                             ("a signed load in a loop header", ("OpReturn\nOpFunctionEnd", LOOP_HEADER)),
                             ("a missing signed array", ("OpDecorate %rt_int ArrayStride 4\n", ""))):
        expect_rejection(old, new, {"a missing signed array": "unsupported physical pointer or scalar layout",
                                    "a signed load in a loop header": "share a block with OpLoopMerge"}.get(
                                        name, "unhandled physical word pointer use"))
        print(f"rejected {name}")
    # Release semantics are legal on the atomic but not on the compare-exchange failure path; a source result named
    # like a generated store stem must not collide with it.
    for name, (old, new) in (("acquire-release semantics", ("%c4 = OpConstant %uint 4\n", "%c4 = OpConstant %uint 4\n%c72 = OpConstant %uint 72\n")),
                             ("a result named like a store", ("%loaded", "%store_1"))):
        variant = SOURCE.replace(old, new)
        if name == "acquire-release semantics":
            variant = variant.replace("%max_target %c1 %c0 %operand", "%max_target %c1 %c72 %operand")
        path = directory / "variant.spvasm"
        source.write_text(variant, encoding="utf-8")
        assemble(source, directory / "variant.spv")
        path.write_text(lower(variant), encoding="utf-8")
        assemble(path, directory / "variant.spv")
        print(f"validated {name}")
    # A workgroup signed atomic is not a physical access, so it passes through with its own semantics constant.
    variant = SOURCE.replace(
        "OpEntryPoint GLCompute %main \"main\" %user_data", "OpEntryPoint GLCompute %main \"main\" %user_data %shared").replace(
        "%words = OpTypeArray", "%_ptr_WG_int = OpTypePointer Workgroup %int\n%shared = OpVariable %_ptr_WG_int Workgroup\n"
        "%relaxed = OpConstant %int 0\n%words = OpTypeArray").replace(
        "OpStore %output_first", "%local_old = OpAtomicSMin %int %shared %c2 %relaxed %operand\nOpStore %output_first")
    source.write_text(variant, encoding="utf-8")
    assemble(source, directory / "variant.spv")
    path = directory / "variant.spvasm"
    path.write_text(lower(variant), encoding="utf-8")
    assemble(path, directory / "variant.spv")
    if "%local_old = OpAtomicSMin %int %shared %c2 %relaxed %operand" not in path.read_text(encoding="utf-8"):
        sys.exit("the workgroup atomic was not preserved")
    print("validated a workgroup signed atomic")
    source.write_text(SOURCE, encoding="utf-8")
    for name, (old, new), _, _ in CONTROLS:
        if not shape_errors(edit(lowered, old, new)):
            sys.exit(f"shape check missed '{name}'")
    print(f"shape check caught all {len(CONTROLS)} mutations")
    if gpu:
        dxc, renderer, adapter = gpu

        def run(assembly, name):
            hlsl = build(directory, name, assembly)
            dxil = directory / f"{name}.dxil"
            subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil, hlsl], check=True)
            return subprocess.run([renderer, adapter, FLAG, dxil], capture_output=True, text=True)

        result = run(lowered, "window")
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode:
            sys.exit(result.returncode)
        for index, (name, (old, new), case, word) in enumerate(CONTROLS):
            result = run(edit(lowered, old, new), f"control{index}")
            text = result.stdout + result.stderr
            if result.returncode == 77:
                sys.exit(77)
            if not result.returncode or not re.search(rf"^{re.escape(case)} word {word} expected ", text, re.M):
                sys.stderr.write(text)
                sys.exit(f"negative control '{name}' did not first diverge at '{case}' word {word}")
            print(f"negative control '{name}': caught at '{case}' word {word}")
