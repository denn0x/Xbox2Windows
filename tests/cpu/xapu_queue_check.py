"""Compare the source-built XApu command-queue caller under Windows and pe_map.

The caller imports the five xapu.dll queue functions by name. No public source gives
their signatures, so the caller and the test-only recording provider share a fixture
convention (see xapu_queue_provider.cpp). It is proposed desktop policy, not console
evidence:

- observable result: connect, enqueue three commands, read the result count, dequeue
  the three results in submission order with matching IDs and values, disconnect;
- refusals: an empty-queue dequeue, an unknown handle, a zero handle and a disconnected
  handle each fail and leave the caller's outputs untouched.

Controls: a provider that accepts unknown handles stops the caller at the handle step,
a provider that returns the newest result first stops it at the order step, a missing
xapu.dll stops the Windows loader before entry, and pe_map stops before entry when the
provider or any one of the five imports is withheld.
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

PASS = 0x8B0
FAIL_ORDER = 0xF03
FAIL_HANDLE = 0xF05
STATUS_DLL_NOT_FOUND = 0xC0000135
QUEUE_IMPORTS = [
    "XApuConnect",
    "XApuEnqueueCommand",
    "XApuGetResultCount",
    "XApuDequeueResult",
    "XApuDisconnect",
]
QUERIES = [
    "get_xapu_connect_count",
    "get_xapu_disconnect_count",
    "get_xapu_live_client_count",
    "get_xapu_enqueued_count",
    "get_xapu_dequeued_count",
    "get_xapu_refused_count",
]
EXPECTED_IDS = [0x101, 0x102, 0x103]
SENTINEL = 0xA9A9A9A9A9A9A9A9
# Refusals with an output: empty dequeue, unknown-handle dequeue, zero-handle count.
EXPECTED_REFUSED_OUTPUTS = [SENTINEL, SENTINEL, 0xA9A9A9A9]
# Calls made by the caller: 1 connect, 3 enqueues, 1 count, 3 x (dequeue + count),
# 1 empty dequeue, 4 handle-step calls, 3 disconnect-step calls.
EXPECTED_CALLS = 1 + 3 + 1 + 6 + 1 + 4 + 3


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
            shutil.copyfile(provider_dll, temp_dir / "xapu.dll")
        shutil.copyfile(io_dll, temp_dir / io_dll.name)
        env = dict(os.environ)
        env.pop("CPU_CRC_TEST_LIVE_WORKER", None)
        if live_worker:
            env["CPU_CRC_TEST_LIVE_WORKER"] = "1"
        return subprocess.run([str(temp_exe)], cwd=str(temp_dir), env=env, timeout=20).returncode & 0xFFFFFFFF


def load_provider(path):
    lib = ctypes.WinDLL(str(path))
    for name in (*QUERIES, "get_xapu_call_count"):
        getattr(lib, name).restype = ctypes.c_uint32
    lib.get_xapu_refused_output_count.restype = ctypes.c_uint32
    for name in ("get_xapu_enqueued_id", "get_xapu_dequeued_id", "get_xapu_refused_output"):
        getattr(lib, name).restype = ctypes.c_uint64
        getattr(lib, name).argtypes = [ctypes.c_uint32]
    lib.reset_xapu_records.restype = None
    return lib


def run_mapped_provider(mapper_path, exe, lib, io_name, io_lib):
    lib.reset_xapu_records()
    return run_mapped(mapper_path, exe, {"xapu.dll": lib, io_name: io_lib})


def compare(mapper_path, exe_path, provider_path, lenient_path, lifo_path, io_path):
    # A missing DLL must fail the child process, not raise a loader dialog.
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    mapper_path = Path(mapper_path)
    exe = Path(exe_path)
    provider = Path(provider_path)
    lenient = Path(lenient_path)
    lifo = Path(lifo_path)
    io_dll = Path(io_path)
    io_name = io_dll.name.casefold()

    imports = imports_by_dll(exe)
    check("caller imports", imports, {
        io_name: ["finish_exe"],
        "xapu.dll": sorted([*QUEUE_IMPORTS, *QUERIES]),
    })
    print(f"caller imports: {imports}")

    # Windows loader, with and without a live worker at exit.
    native_code = run_native(exe, provider, io_dll)
    check("native exit", native_code, PASS)
    check("native exit with live worker", run_native(exe, provider, io_dll, live_worker=True), PASS)

    # pe_map, with the provider's records read back in this process.
    prov = load_provider(provider)
    io_lib = ctypes.WinDLL(str(io_dll))
    mapped_code = run_mapped_provider(mapper_path, exe, prov, io_name, io_lib)
    check("mapped exit", mapped_code, PASS)
    check("mapped matches native", mapped_code, native_code)
    check("mapped counts", [getattr(prov, name)() for name in QUERIES], [1, 1, 0, 3, 3, 6])
    check("mapped calls", prov.get_xapu_call_count(), EXPECTED_CALLS)
    check("enqueued IDs", [prov.get_xapu_enqueued_id(i) for i in range(4)], [*EXPECTED_IDS, 0])
    check("dequeued IDs", [prov.get_xapu_dequeued_id(i) for i in range(4)], [*EXPECTED_IDS, 0])
    check("refused outputs", prov.get_xapu_refused_output_count(), len(EXPECTED_REFUSED_OUTPUTS))
    check("sentinels seen at refusal",
          [prov.get_xapu_refused_output(i) for i in range(len(EXPECTED_REFUSED_OUTPUTS))],
          EXPECTED_REFUSED_OUTPUTS)
    print(f"native and mapped exit {PASS:#x}; FIFO IDs {[hex(i) for i in EXPECTED_IDS]}; 6 refusals; "
          f"sentinels seen at refusal {[hex(v) for v in EXPECTED_REFUSED_OUTPUTS]}")

    # Control: a provider that accepts unknown handles is caught at the handle step.
    check("native lenient exit", run_native(exe, lenient, io_dll), FAIL_HANDLE)
    lenient_lib = load_provider(lenient)
    check("mapped lenient exit", run_mapped_provider(mapper_path, exe, lenient_lib, io_name, io_lib), FAIL_HANDLE)
    check("lenient accepted foreign enqueue", lenient_lib.get_xapu_enqueued_id(3), 0x1EE)
    print(f"lenient provider: native and mapped exit {FAIL_HANDLE:#x} at the handle step")

    # Control: a provider that returns the newest result first is caught at the order step.
    check("native LIFO exit", run_native(exe, lifo, io_dll), FAIL_ORDER)
    lifo_lib = load_provider(lifo)
    check("mapped LIFO exit", run_mapped_provider(mapper_path, exe, lifo_lib, io_name, io_lib), FAIL_ORDER)
    check("LIFO first dequeued ID", lifo_lib.get_xapu_dequeued_id(0), EXPECTED_IDS[-1])
    print(f"LIFO provider: native and mapped exit {FAIL_ORDER:#x} at the order step")

    # Control: without xapu.dll the Windows loader refuses the process before entry.
    check("native missing provider", run_native(exe, None, io_dll), STATUS_DLL_NOT_FOUND)
    print(f"Windows loader without xapu.dll: {STATUS_DLL_NOT_FOUND:#x}")

    # Control: pe_map stops before entry without the provider or any one queue import.
    mapper = Mapper(mapper_path)
    exports = {name: ctypes.cast(getattr(prov, name), ctypes.c_void_p).value
               for name in (*QUEUE_IMPORTS, *QUERIES)}
    cases = [("provider", {io_name: io_lib})]
    for withheld in QUEUE_IMPORTS:
        table = {name.encode(): address for name, address in exports.items() if name != withheld}
        cases.append((withheld, {io_name: io_lib, "xapu.dll": table}))
    for withheld, providers in cases:
        prov.reset_xapu_records()
        base, status, detail, requests = mapper(exe, providers)
        if base:
            mapper.release(base)
            raise AssertionError(f"caller mapped without {withheld}")
        check(f"withheld {withheld} status", status, STATUS["unresolved"])
        if withheld == "provider":
            dll, symbol = requests[-1]
            check("missing provider module", dll.casefold(), "xapu.dll")
            check("missing provider diagnostic", detail, f"unresolved import {dll}!{symbol}")
        else:
            check(f"withheld {withheld} diagnostic", detail, f"unresolved import xapu.dll!{withheld}")
        check(f"withheld {withheld} calls", prov.get_xapu_call_count(), 0)
        print(f"before entry: {detail}")


if __name__ == "__main__":
    compare(*sys.argv[1:])
