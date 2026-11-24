"""Compile the source-built address-window QWORD fixture and run its D3D12 readback and negative controls.

Usage: qword.py <spirv-as> <spirv-val> <spirv-cross> <dxc> <offscreen_check> <hardware|warp>

The shader loads one QWORD through an indexed physical address, adds one and stores it through a direct physical
address. The readback model in readback.cpp is separate C++ and does not share code with the lowering. Two builds run:
the production window, and a narrow one that ends inside the 64-byte view so an access past the end is visible.
Each negative control mutates one part of the lowered module and must first diverge at a predicted case and word.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

assembler, validator, cross, dxc, renderer, adapter = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools"))
from spirv_address_window import lower

SKIP = 77
PRODUCTION = "--spirv-address-window-qword"
NARROW = "--spirv-address-window-qword-narrow"
END = "%x2w_end_wide = OpConstant %ulong 4294967288"
NARROW_END = "%x2w_end_wide = OpConstant %ulong 40"
LOAD = "%x2w_load_value"

# name, flag, (old, new) edit of the lowered module, case, first diverging word
CONTROLS = (
    ("halves swapped on load", PRODUCTION, (f"{LOAD}_low = OpLoad %uint {LOAD}_low_ptr",
                                            f"{LOAD}_low = OpLoad %uint {LOAD}_high_ptr"),
     "qword baseline", 8),
    ("DWORD alignment guard", PRODUCTION, ("%x2w_mask_wide = OpConstant %ulong 7",
                                           "%x2w_mask_wide = OpConstant %ulong 3"),
     "qword load aligned to a DWORD only", 8),
    ("load wrap guard dropped", PRODUCTION, ("OpBranchConditional %x2w_address_source_checked",
                                             "OpBranchConditional %x2w_address_source_valid"),
     "qword index addition wraps", 8),
    ("store alignment guard dropped", PRODUCTION, ("OpBranchConditional %x2w_address_target_valid",
                                                   "OpBranchConditional %x2w_address_target_range"),
     "qword store aligned to a DWORD only", 9),
    ("window end beyond the view", NARROW, (NARROW_END, END), "narrow load at the end", 4),
    ("store range guard dropped", NARROW, ("OpBranchConditional %x2w_address_target_valid",
                                           "OpBranchConditional %x2w_address_target_aligned"),
     "narrow store at the end", 10),
)


def assemble(source, module):
    subprocess.run([assembler, "--target-env", "vulkan1.2", source, "-o", module], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)


def build(directory, name, assembly):
    source = directory / f"{name}.spvasm"
    module = directory / f"{name}.spv"
    hlsl = directory / f"{name}.hlsl"
    dxil = directory / f"{name}.dxil"
    source.write_text(assembly, encoding="utf-8")
    assemble(source, module)
    subprocess.run([cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl], check=True)
    subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil, hlsl], check=True)
    return dxil


def edit(assembly, old, new):
    if assembly.count(old) != 1:
        raise ValueError(f"expected exactly one {old!r} in the lowered module")
    return assembly.replace(old, new)


def run(flag, dxil):
    return subprocess.run([renderer, adapter, flag, dxil], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as folder:
    directory = Path(folder)
    lowered = lower(Path(__file__).with_suffix(".spvasm").read_text(encoding="utf-8"))
    narrow = edit(lowered, END, NARROW_END)
    for flag, assembly in ((PRODUCTION, lowered), (NARROW, narrow)):
        result = run(flag, build(directory, flag.removeprefix("--spirv-address-window-"), assembly))
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode:
            sys.exit(result.returncode)
    for index, (name, flag, (old, new), case, word) in enumerate(CONTROLS):
        assembly = edit(narrow if flag == NARROW else lowered, old, new)
        result = run(flag, build(directory, f"control{index}", assembly))
        text = result.stdout + result.stderr
        if result.returncode == SKIP:
            sys.exit(SKIP)
        if not result.returncode or not re.search(rf"^{re.escape(case)} word {word} expected ", text, re.M):
            sys.stderr.write(text)
            sys.exit(f"negative control '{name}' did not first diverge at '{case}' word {word}")
        print(f"negative control '{name}': caught at '{case}' word {word}")
