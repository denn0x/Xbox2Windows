"""Compare the source-built ACP HAL, shape-context and heap caller under Windows and pe_map.

The caller imports acphal.dll exports by name:
  - ApuCreateHeap
  - AcpHalCreate
  - AcpHalAllocateShapeContexts
  - AcpHalReleaseShapeContexts
  - ApuAlloc and ApuFree (public DirectXTK12 call shape), to observe the heap

The four HAL/heap argument lists are a fixture contract (see acphal_hal_provider.cpp);
none has a public signature. The checker verifies:
- the heap created by ApuCreateHeap bounds ApuAlloc, and ApuAlloc before it is refused;
- one HAL instance; a second create is refused with its output untouched;
- shape contexts come back cleared, aligned and disjoint, are reusable after release;
- invalid counts, null outputs, wrong handles and a double release are refused.

Controls:
- A faulty provider that skips clearing the contexts stops the caller at Step 7.
- Withholding any of the four exports stops pe_map before entry.
- With no acphal.dll binding at all, pe_map stops before entry, and the Windows
  loader refuses the process with STATUS_DLL_NOT_FOUND.
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

PASS = 0x8AC
FAIL_FAULTY = 0xF07
STATUS_DLL_NOT_FOUND = 0xC0000135
CONTRACT = [
    "AcpHalAllocateShapeContexts",
    "AcpHalCreate",
    "AcpHalReleaseShapeContexts",
    "ApuCreateHeap",
]
IMPORTED = sorted(CONTRACT + ["ApuAlloc", "ApuFree", "get_acphal_double_release_refusals"])
QUERIES = [
    "get_acphal_heap_create_calls",
    "get_acphal_heap_limit",
    "get_acphal_alloc_calls",
    "get_acphal_free_calls",
    "get_acphal_hal_create_calls",
    "get_acphal_shape_alloc_calls",
    "get_acphal_shape_alloc_successes",
    "get_acphal_shape_release_calls",
    "get_acphal_double_release_refusals",
    "get_acphal_handle_refusals",
    "get_acphal_live_shape_allocations",
]
# Calls made by one successful run of the caller (see acphal_hal_caller.asm).
EXPECTED_RECORDS = {
    "get_acphal_heap_create_calls": 3,
    "get_acphal_heap_limit": 0x2000,
    "get_acphal_alloc_calls": 3,
    "get_acphal_free_calls": 1,
    "get_acphal_hal_create_calls": 3,
    "get_acphal_shape_alloc_calls": 7,
    "get_acphal_shape_alloc_successes": 3,
    "get_acphal_shape_release_calls": 5,
    "get_acphal_double_release_refusals": 1,
    "get_acphal_handle_refusals": 2,
    "get_acphal_live_shape_allocations": 0,
}


def imports_by_dll(image):
    with pefile.PE(str(image), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        result = {}
        for module in pe.DIRECTORY_ENTRY_IMPORT:
            if any(not imp.name for imp in module.imports):
                raise AssertionError(f"{module.dll.decode()} has an ordinal import")
            result[module.dll.decode().casefold()] = sorted(imp.name.decode() for imp in module.imports)
        return result


def check(label, actual, expected):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def run_native(exe, provider_dll, io_dll, *, live_worker=False):
    with TemporaryDirectory() as temp:
        temp_dir = Path(temp)
        temp_exe = temp_dir / exe.name
        shutil.copyfile(exe, temp_exe)
        if provider_dll is not None:
            shutil.copyfile(provider_dll, temp_dir / "acphal.dll")
        shutil.copyfile(io_dll, temp_dir / io_dll.name)
        env = dict(os.environ)
        env.pop("CPU_CRC_TEST_LIVE_WORKER", None)
        if live_worker:
            env["CPU_CRC_TEST_LIVE_WORKER"] = "1"
        return subprocess.run([str(temp_exe)], cwd=str(temp_dir), env=env, timeout=20).returncode & 0xFFFFFFFF


def load_provider(path):
    lib = ctypes.WinDLL(str(path))
    for name in QUERIES:
        getattr(lib, name).restype = ctypes.c_uint32
    lib.reset_acphal_hal_records.restype = None
    return lib


def records(lib):
    return {name: getattr(lib, name)() for name in QUERIES}


def compare(mapper_path, exe_path, provider_path, faulty_path, io_path):
    mapper_path = Path(mapper_path)
    exe = Path(exe_path)
    provider = Path(provider_path)
    faulty = Path(faulty_path)
    io_dll = Path(io_path)
    io_name = io_dll.name.casefold()

    imports = imports_by_dll(exe)
    check("caller imports", imports, {io_name: ["finish_exe"], "acphal.dll": IMPORTED})
    print(f"caller imports: {imports}")

    # Windows loader, with and without a live worker at exit.
    native_code = run_native(exe, provider, io_dll)
    check("native exit", native_code, PASS)
    check("native exit with live worker", run_native(exe, provider, io_dll, live_worker=True), PASS)

    # pe_map, with the provider's records read back in this process.
    prov = load_provider(provider)
    io_lib = ctypes.WinDLL(str(io_dll))
    prov.reset_acphal_hal_records()
    mapped_code = run_mapped(mapper_path, exe, {"acphal.dll": prov, io_name: io_lib})
    check("mapped exit", mapped_code, PASS)
    check("mapped matches native", mapped_code, native_code)
    check("mapped provider records", records(prov), EXPECTED_RECORDS)
    print(f"native and mapped exit {PASS:#x}; provider records {records(prov)}")

    # Control: the caller's Step 4 passes a null handle, which a handle check alone would
    # refuse. Here the real handle address is replayed before AcpHalCreate, so only the
    # provider's created-state check can refuse it; the same call succeeds after create.
    hresult = ctypes.c_int32
    prov.AcpHalCreate.restype = hresult
    prov.AcpHalCreate.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    prov.AcpHalAllocateShapeContexts.restype = hresult
    prov.AcpHalAllocateShapeContexts.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
    prov.AcpHalReleaseShapeContexts.restype = hresult
    prov.AcpHalReleaseShapeContexts.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    prov.reset_acphal_hal_records()
    hal = ctypes.c_void_p()
    check("direct create", prov.AcpHalCreate(ctypes.byref(hal)), 0)
    prov.reset_acphal_hal_records()
    contexts = ctypes.c_void_p(0xA5A5)
    check("replayed handle before create", prov.AcpHalAllocateShapeContexts(hal, 1, ctypes.byref(contexts)) & 0xFFFFFFFF,
          0x80070006)
    check("replayed handle output", contexts.value, 0xA5A5)
    check("replayed handle refusals", prov.get_acphal_handle_refusals(), 1)
    again = ctypes.c_void_p()
    check("direct recreate", prov.AcpHalCreate(ctypes.byref(again)), 0)
    check("handle address", again.value, hal.value)
    check("same call after create", prov.AcpHalAllocateShapeContexts(hal, 1, ctypes.byref(contexts)), 0)
    check("direct release", prov.AcpHalReleaseShapeContexts(hal, contexts), 0)
    prov.reset_acphal_hal_records()
    print("before create: replayed handle refused with E_HANDLE; same call succeeds after create")

    # Control: a provider that skips clearing the contexts is caught at Step 7.
    check("native faulty exit", run_native(exe, faulty, io_dll), FAIL_FAULTY)
    faulty_lib = load_provider(faulty)
    faulty_lib.reset_acphal_hal_records()
    faulty_code = run_mapped(mapper_path, exe, {"acphal.dll": faulty_lib, io_name: io_lib})
    check("mapped faulty exit", faulty_code, FAIL_FAULTY)
    check("faulty provider still reported success", faulty_lib.get_acphal_shape_alloc_successes(), 1)
    print(f"faulty provider: native and mapped exit {FAIL_FAULTY:#x} at Step 7")

    # Control: withholding any contract export stops pe_map before entry, with no calls.
    mapper = Mapper(mapper_path)
    exports = {name: ctypes.cast(getattr(prov, name), ctypes.c_void_p).value for name in IMPORTED}
    for withheld in CONTRACT:
        prov.reset_acphal_hal_records()
        table = {name.encode(): address for name, address in exports.items() if name != withheld}
        base, status, detail, _ = mapper(exe, {io_name: io_lib, "acphal.dll": table})
        if base:
            mapper.release(base)
            raise AssertionError(f"caller mapped without {withheld}")
        check(f"withheld {withheld} status", status, STATUS["unresolved"])
        check(f"withheld {withheld} diagnostic", detail, f"unresolved import acphal.dll!{withheld}")
        check(f"withheld {withheld} calls", sum(records(prov).values()), 0)
        print(f"before entry: {detail}")

    # Control: no provider at all.
    prov.reset_acphal_hal_records()
    base, status, detail, requests = mapper(exe, {io_name: io_lib})
    if base:
        mapper.release(base)
        raise AssertionError("caller mapped without acphal.dll")
    check("missing provider status", status, STATUS["unresolved"])
    dll, symbol = requests[-1]
    check("missing provider module", dll.casefold(), "acphal.dll")
    check("missing provider diagnostic", detail, f"unresolved import {dll}!{symbol}")
    check("missing provider calls", sum(records(prov).values()), 0)
    print(f"missing provider, before entry: {detail}")

    # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, inherited by the child, so the
    # loader reports the missing DLL as an exit status instead of a dialog.
    previous = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    try:
        missing_code = run_native(exe, None, io_dll)
    finally:
        ctypes.windll.kernel32.SetErrorMode(previous)
    check("native missing provider exit", missing_code, STATUS_DLL_NOT_FOUND)
    print(f"missing provider, Windows loader: exit {missing_code:#x} (STATUS_DLL_NOT_FOUND)")


if __name__ == "__main__":
    compare(*sys.argv[1:])
