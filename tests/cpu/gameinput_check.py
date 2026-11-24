"""Compare the source-built GameInput caller under Windows and pe_map.

The caller imports GameInput.dll ordinal 1 only (no name), as the statically read console caller
in #1056 does, plus one named query export of the test provider. It drives the v0 objects with
that caller's call shapes (see gameinput_caller.asm); the runtime behaviour it checks is the
fixture policy of gameinput_provider.cpp.

Controls:
- create fails: the caller stops at Step 1;
- blocking enumeration calls back on another thread and waits: the caller's bounded lock spin
  reports it at Step 2 instead of deadlocking;
- the gamepad state is written 4 bytes late: the caller stops at Step 5;
- withholding ordinal 1, or a provider that exports the function at ordinal 2, stops pe_map
  before entry; the Windows loader refuses the wrong ordinal;
- with no provider at all, pe_map stops before entry. The Windows loader either finds no DLL or
  falls back to the desktop System32 GameInput.dll, which lacks the test's named export.
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

PASS = 0x6A1
FAIL_CREATE = 0xF01
FAIL_THREAD = 0xF02
FAIL_LAYOUT = 0xF05
STATUS_DLL_NOT_FOUND = 0xC0000135
STATUS_ORDINAL_NOT_FOUND = 0xC0000138
STATUS_ENTRYPOINT_NOT_FOUND = 0xC0000139
DLL = "gameinput.dll"
QUERY_IMPORT = "get_gameinput_outstanding_refs"
QUERIES = [
    "get_gameinput_create_calls",
    "get_gameinput_create_refusals",
    "get_gameinput_register_calls",
    "get_gameinput_register_refusals",
    "get_gameinput_callbacks",
    "get_gameinput_callbacks_other_thread",
    "get_gameinput_reading_calls",
    "get_gameinput_reading_refusals",
    "get_gameinput_gamepad_state_calls",
    "get_gameinput_key_state_calls",
    "get_gameinput_find_calls",
    "get_gameinput_find_refusals",
    "get_gameinput_rumble_calls",
    "get_gameinput_rumble_trigger_nonzero",
    "get_gameinput_rumble_low_bits",
    "get_gameinput_unregister_calls",
    "get_gameinput_unregister_refusals",
    "get_gameinput_unregister_timeout_sum",
    "get_gameinput_unexpected_calls",
    "get_gameinput_outstanding_refs",
]
# Calls made by one successful run of the caller (see gameinput_caller.asm).
EXPECTED_RECORDS = {
    "get_gameinput_create_calls": 3,          # NULL output, the object, the same object again
    "get_gameinput_create_refusals": 1,
    "get_gameinput_register_calls": 3,        # keyboard, gamepad, mouse
    "get_gameinput_register_refusals": 1,
    "get_gameinput_callbacks": 2,
    "get_gameinput_callbacks_other_thread": 0,
    "get_gameinput_reading_calls": 4,         # gamepad current and previous, keyboard current and next
    "get_gameinput_reading_refusals": 1,
    "get_gameinput_gamepad_state_calls": 2,
    "get_gameinput_key_state_calls": 1,
    "get_gameinput_find_calls": 2,
    "get_gameinput_find_refusals": 1,
    "get_gameinput_rumble_calls": 1,
    "get_gameinput_rumble_trigger_nonzero": 0,
    "get_gameinput_rumble_low_bits": 0x3F000000,  # 0.5f
    "get_gameinput_unregister_calls": 3,
    "get_gameinput_unregister_refusals": 1,
    "get_gameinput_unregister_timeout_sum": 5000 + 1 + 5000,  # an infinite wait is recorded as 1
    "get_gameinput_unexpected_calls": 0,
    "get_gameinput_outstanding_refs": 0,
}


def imports_by_dll(image):
    with pefile.PE(str(image), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        result = {}
        for module in pe.DIRECTORY_ENTRY_IMPORT:
            result[module.dll.decode().casefold()] = sorted(
                (imp.name.decode() if imp.name else f"#{imp.ordinal}") for imp in module.imports)
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
            shutil.copyfile(provider_dll, temp_dir / "GameInput.dll")
        shutil.copyfile(io_dll, temp_dir / io_dll.name)
        env = dict(os.environ)
        env.pop("CPU_CRC_TEST_LIVE_WORKER", None)
        if live_worker:
            env["CPU_CRC_TEST_LIVE_WORKER"] = "1"
        return subprocess.run([str(temp_exe)], cwd=str(temp_dir), env=env, timeout=30).returncode & 0xFFFFFFFF


def run_native_quiet(*args, **kwargs):
    # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, inherited by the child, so the loader
    # reports a missing DLL or export as an exit status instead of a dialog.
    previous = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    try:
        return run_native(*args, **kwargs)
    finally:
        ctypes.windll.kernel32.SetErrorMode(previous)


def load_provider(path, queries=QUERIES):
    lib = ctypes.WinDLL(str(path))
    for name in queries:
        getattr(lib, name).restype = ctypes.c_uint32
    lib.reset_gameinput_records.restype = None
    return lib


def records(lib):
    return {name: getattr(lib, name)() for name in QUERIES}


def mapped(mapper_path, exe, lib, io_name, io_lib):
    lib.reset_gameinput_records()
    return run_mapped(mapper_path, exe, {DLL: lib, io_name: io_lib})


def compare(mapper_path, exe_path, provider_path, create_fail_path, other_thread_path, shifted_path, wrong_path,
            io_path):
    mapper_path = Path(mapper_path)
    exe = Path(exe_path)
    provider = Path(provider_path)
    io_dll = Path(io_path)
    io_name = io_dll.name.casefold()

    imports = imports_by_dll(exe)
    check("caller imports", imports, {io_name: ["finish_exe"], DLL: ["#1", QUERY_IMPORT]})
    print(f"caller imports: {imports}")

    # Windows loader, with and without a live worker at exit.
    native_code = run_native(exe, provider, io_dll)
    check("native exit", native_code, PASS)
    check("native exit with live worker", run_native(exe, provider, io_dll, live_worker=True), PASS)

    # pe_map, with the provider's records read back in this process.
    prov = load_provider(provider)
    io_lib = ctypes.WinDLL(str(io_dll))
    mapped_code = mapped(mapper_path, exe, prov, io_name, io_lib)
    check("mapped exit", mapped_code, PASS)
    check("mapped matches native", mapped_code, native_code)
    check("mapped provider records", records(prov), EXPECTED_RECORDS)
    print(f"native and mapped exit {PASS:#x}; provider records {records(prov)}")

    # Controls: three faulty providers, each caught at its step under both loaders.
    for path, code, label in ((create_fail_path, FAIL_CREATE, "create fails"),
                              (other_thread_path, FAIL_THREAD, "callback on another thread"),
                              (shifted_path, FAIL_LAYOUT, "gamepad state 4 bytes late")):
        path = Path(path)
        check(f"native {label}", run_native(exe, path, io_dll), code)
        lib = load_provider(path)
        check(f"mapped {label}", mapped(mapper_path, exe, lib, io_name, io_lib), code)
        print(f"{label}: native and mapped exit {code:#x}")
    other = load_provider(Path(other_thread_path))
    mapped(mapper_path, exe, other, io_name, io_lib)
    check("other-thread callbacks recorded", other.get_gameinput_callbacks_other_thread(), 1)

    # Control: withholding ordinal 1 stops pe_map before entry, with no calls.
    mapper = Mapper(mapper_path)
    query = ctypes.cast(getattr(prov, QUERY_IMPORT), ctypes.c_void_p).value
    prov.reset_gameinput_records()
    base, status, detail, _ = mapper(exe, {io_name: io_lib, DLL: {QUERY_IMPORT.encode(): query}})
    if base:
        mapper.release(base)
        raise AssertionError("caller mapped without ordinal 1")
    check("withheld ordinal status", status, STATUS["unresolved"])
    check("withheld ordinal diagnostic", detail, "unresolved import GameInput.dll!#1")
    check("withheld ordinal calls", sum(records(prov).values()), 0)
    print(f"before entry: {detail}")

    # Control: a provider exporting the function at ordinal 2 instead.
    wrong = Path(wrong_path)
    wrong_lib = ctypes.WinDLL(str(wrong))
    base, status, detail, _ = mapper(exe, {io_name: io_lib, DLL: wrong_lib})
    if base:
        mapper.release(base)
        raise AssertionError("caller mapped with the function at ordinal 2")
    check("wrong ordinal status", status, STATUS["unresolved"])
    check("wrong ordinal diagnostic", detail, "unresolved import GameInput.dll!#1")
    check("native wrong ordinal exit", run_native_quiet(exe, wrong, io_dll), STATUS_ORDINAL_NOT_FOUND)
    print(f"ordinal 2 provider: pe_map {detail}; Windows loader exit {STATUS_ORDINAL_NOT_FOUND:#x}")

    # Control: no provider at all.
    prov.reset_gameinput_records()
    base, status, detail, requests = mapper(exe, {io_name: io_lib})
    if base:
        mapper.release(base)
        raise AssertionError("caller mapped without GameInput.dll")
    check("missing provider status", status, STATUS["unresolved"])
    dll, symbol = requests[-1]
    check("missing provider module", dll.casefold(), DLL)
    shown = f"#{symbol}" if isinstance(symbol, int) else symbol
    check("missing provider diagnostic", detail, f"unresolved import {dll}!{shown}")
    check("missing provider calls", sum(records(prov).values()), 0)
    print(f"missing provider, before entry: {detail}")
    missing_code = run_native_quiet(exe, None, io_dll)
    if missing_code not in (STATUS_DLL_NOT_FOUND, STATUS_ENTRYPOINT_NOT_FOUND):
        raise AssertionError(f"native missing provider exit: got {missing_code:#x}")
    where = "no GameInput.dll found" if missing_code == STATUS_DLL_NOT_FOUND else \
        "loader fell back to the system GameInput.dll, which lacks the test export"
    print(f"missing provider, Windows loader: exit {missing_code:#x} ({where})")


if __name__ == "__main__":
    compare(*sys.argv[1:])
