"""Compile the source-built address-window atomic and run its D3D12 readback."""
from pathlib import Path
import subprocess
import sys
import tempfile

assembler, validator, cross, dxc, renderer, adapter = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools"))
from spirv_address_window import lower


def assemble(source, module):
    subprocess.run([assembler, "--target-env", "vulkan1.2", source, "-o", module], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)


with tempfile.TemporaryDirectory() as folder:
    directory = Path(folder)
    module = directory / "atomic.spv"
    hlsl = directory / "atomic.hlsl"
    dxil = directory / "atomic.dxil"
    source = Path(__file__).with_suffix(".spvasm")
    assemble(source, module)
    adapted = lower(source.read_text(encoding="utf-8"))
    # Put the substitute index inside this fixture's UAV so a stray atomic changes word 15.
    sentinel = "%x2w_invalid = OpConstant %uint 1073741823\n"
    if adapted.count(sentinel) != 1:
        raise ValueError("expected exactly one address-window substitute index")
    logical = directory / "logical.spvasm"
    logical.write_text(adapted.replace(sentinel, "%x2w_invalid = OpConstant %uint 15\n"), encoding="utf-8")
    assemble(logical, module)
    subprocess.run([cross, module, "--hlsl", "--shader-model", "60", "--output", hlsl], check=True)
    subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", dxil, hlsl], check=True)
    sys.exit(subprocess.run([renderer, adapter, "--spirv-address-window-atomic", dxil]).returncode)
