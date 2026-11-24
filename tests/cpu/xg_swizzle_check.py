"""Compare the source-built XG swizzle selection and texture computer caller under Windows and pe_map (#870, #1032).

Exercises the swizzle selection mode, output pointer aliasing, and texture computer integration:
- Public sample call shape: XGSuggestSwizzleMode(desc, 0, &out) -> XGCreateTextureComputer(desc, &comp)
  with selector flag 0 and 3-mip RGBA8 texture descriptor (28x4, 14x2, 7x1 = 147 texels).
- Output pointer aliasing check:
  - Separate output variable receives suggested layout (0x105 per fixture policy), leaving descriptor Layout untouched.
  - Aliased output pointer (&desc.Layout) writes suggested layout in-place, matching separate output.
- Texture computer upload and readback:
  - CopyIntoSubresource uploads 147 texels across 3 mips into tiled resource memory.
  - CopyFromSubresource detiles all 147 texels back to linear destination with 100% round-trip match.
  - The 147-texel Morton round-trip serves as an additional synthetic caller/provider check (disagreeing
    on 139 of 147 texels under Morton bit-interleaving). It is fixture policy only, not an independent
    match to #979 and not evidence of console layout encoding (see #1032 for the full AddrLib reference).
- Separated fault controls:
  - Sentinel clobber fault on refusal is caught specifically at step 1 (exit 0xE01).
  - Dummy linear fallback fault is caught specifically by tiling oracle at step 4 (exit 0xE04).
- Multi-loader verification:
  - Runs natively under Windows loader (exit 0x870).
  - Runs with live worker thread under CPU_CRC_TEST_LIVE_WORKER.
  - Runs mapped under pe_map (exit 0x870).
  - Withholding xg_xs.dll stops pe_map at import resolution with STATUS["unresolved"].
"""

import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mapped_exe_chain_check import run_mapped
from native_map_check import Mapper, STATUS


def imports_by_dll(image):
    with pefile.PE(str(image), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        return {module.dll.decode().casefold(): sorted(imp.name.decode() for imp in module.imports if imp.name)
                for module in pe.DIRECTORY_ENTRY_IMPORT}


def check(label, actual, expected):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def run_native(exe, provider_dll, io_dll, *, live_worker=False, extra_env=None):
    """Run native caller EXE in temporary directory with xg_xs.dll and cpu_exe_io.dll."""
    with TemporaryDirectory() as temp:
        temp_dir = Path(temp)
        temp_exe = temp_dir / exe.name
        temp_xg = temp_dir / "xg_xs.dll"
        temp_io = temp_dir / "cpu_exe_io.dll"
        shutil.copyfile(exe, temp_exe)
        shutil.copyfile(provider_dll, temp_xg)
        shutil.copyfile(io_dll, temp_io)
        env = dict(os.environ)
        if live_worker:
            env["CPU_CRC_TEST_LIVE_WORKER"] = "1"
        if extra_env:
            env.update(extra_env)
        proc = subprocess.run([str(temp_exe)], cwd=str(temp_dir), env=env, timeout=20)
        return proc.returncode


def compare(mapper_path, exe_path, provider_path, clobber_provider_path, linear_provider_path, io_path):
    mapper_path = Path(mapper_path)
    exe = Path(exe_path)
    provider = Path(provider_path)
    clobber_provider = Path(clobber_provider_path)
    linear_provider = Path(linear_provider_path)
    io_dll = Path(io_path)
    io_name = io_dll.name.casefold()

    # 1. Verify import descriptors in caller PE image
    imports = imports_by_dll(exe)
    check("caller imports", imports, {
        io_name: ["finish_exe"],
        "xg_xs.dll": ["XGCreateTextureComputer", "XGSuggestSwizzleMode"],
    })
    print(f"xg swizzle caller imports: {imports}")

    # Set up provider functions for verification
    prov_lib = ctypes.WinDLL(str(provider))
    prov_lib.get_xg_suggest_calls.restype = ctypes.c_uint64
    prov_lib.get_xg_create_computer_calls.restype = ctypes.c_uint64
    prov_lib.get_xg_copy_into_calls.restype = ctypes.c_uint64
    prov_lib.get_xg_copy_from_calls.restype = ctypes.c_uint64
    prov_lib.get_xg_invalid_refusals.restype = ctypes.c_uint64
    prov_lib.get_xg_last_suggested_mode.restype = ctypes.c_uint32

    # Case 1: Native Windows execution with valid recording provider (exit 0x870)
    code = run_native(exe, provider, io_dll)
    check("native execution exit", code, 0x870)
    print("native execution: exit 0x870 PASS")

    # Case 2: Clean exit with live worker thread
    code_worker = run_native(exe, provider, io_dll, live_worker=True)
    check("live worker execution exit", code_worker, 0x870)
    print("live worker execution: exit 0x870 PASS")

    # Case 3A: Fault control 1 - sentinel clobber mutant caught specifically with exit 0xE01
    clobber_code = run_native(exe, clobber_provider, io_dll)
    check("clobber mutant exit", clobber_code, 0xE01)
    print("clobber mutant caught: exit 0xE01 PASS")

    # Case 3B: Fault control 2 - dummy linear fallback mutant caught specifically with exit 0xE04
    linear_code = run_native(exe, linear_provider, io_dll)
    check("linear mutant exit", linear_code, 0xE04)
    print("linear fallback mutant caught: exit 0xE04 PASS")

    # Case 4: Mapped execution under pe_map
    io_loaded = ctypes.WinDLL(str(io_dll))
    providers_dict = {
        "xg_xs.dll": prov_lib,
        io_name: io_loaded,
    }
    mapped_code = run_mapped(mapper_path, exe, providers_dict)
    check("mapped execution exit", mapped_code, 0x870)
    print("mapped execution under pe_map: exit 0x870 PASS")

    # Case 5: Withholding xg_xs.dll stops pe_map at import resolution
    withheld_providers = {io_name: io_loaded}
    mapper = Mapper(mapper_path)
    base, status, detail, _ = mapper(exe, withheld_providers)
    if base:
        mapper.release(base)
        raise AssertionError("image mapped successfully despite withheld xg_xs.dll")
    check("withheld provider status", status, STATUS["unresolved"])
    if not detail.startswith("unresolved import xg_xs.dll!"):
        raise AssertionError(f"unexpected diagnostic on withholding xg_xs.dll: {detail}")
    print("withheld provider control: STATUS['unresolved'] PASS")

    print("cpu_native_xg_swizzle: all checks PASS")


if __name__ == "__main__":
    if len(sys.argv) != 7:
        print(f"usage: {sys.argv[0]} <pe_map> <caller_exe> <provider_dll> <clobber_dll> <linear_dll> <io_dll>")
        sys.exit(2)
    compare(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
