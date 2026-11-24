"""Verify ordinal-only DLL imports in native and mapped package startup on Windows.

Only source-built CRT-free fixtures are read or executed. All cases run as child
processes so a failing mapped image cannot terminate the CTest host.

Lingering-thread control (#1375): with a provider whose mix_args leaves a thread running, the native EXE
must still end at once with its result, because its entry ends the process. A build whose entry returns
instead must stay alive until the harness kills it, which shows the control can see a lingering process.
"""
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "pe_run_package.py"
MODULE = "cpu_package_ordinal_provider.dll"
EXE = "cpu_package_ordinal_exe.exe"
RETURN_EXE = "cpu_package_ordinal_exe_return.exe"
LINGER_DEADLINE = 10  # s; the EXE itself ends in milliseconds
RETURN_DEADLINE = 3  # s; the returning control never ends by itself while the provider's thread sleeps


def import_fixture(exe, provider, altered, wrong, live, returning):
    for image in (exe, returning):  # ending the process adds no import
        with pefile.PE(str(image), fast_load=True) as pe:
            pe.parse_data_directories(directories=[1])
            rows = [(d.dll.decode().casefold(), imp.ordinal, imp.name) for d in pe.DIRECTORY_ENTRY_IMPORT
                    for imp in d.imports]
        assert rows == [(MODULE, 77, None)], (image, rows)
    for path, expected in ((provider, 77), (altered, 77), (wrong, 78), (live, 77)):
        with pefile.PE(str(path), fast_load=True) as pe:
            pe.parse_data_directories(directories=[0])
            entries = [(exp.ordinal, exp.name, exp.forwarder) for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols]
        assert entries == [(expected, None, None)], (path, entries)


def launch(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def check_case(mapper, exe, provider, expected):
    with TemporaryDirectory() as temp:
        root = Path(temp)
        shutil.copyfile(exe, root / EXE)
        shutil.copyfile(provider, root / MODULE)
        native = launch([str(root / EXE)])
        assert (native.returncode, native.stdout, native.stderr) == (expected, "", ""), native
        preflight = launch([sys.executable, "-B", str(TOOL), str(mapper), str(root), EXE,
                            "--preflight-only"])
        assert preflight.returncode == 0 and not preflight.stderr, preflight
        pre = json.loads(preflight.stdout)
        assert pre["image_order"] == [MODULE, EXE], pre
        assert pre["external_bindings"] == 0 and pre["missing"] == [] and not pre["entry_executed"], pre
        mapped = launch([sys.executable, "-B", str(TOOL), str(mapper), str(root), EXE])
        assert mapped.returncode == 0 and not mapped.stderr, mapped
        report = json.loads(mapped.stdout)
        assert report == {"image_order": [MODULE, EXE], "entry_executed": True, "exit_code": expected}, report
        print(f"ordinal @77: native and pe_map package startup exited {expected}, correct dependency order")


def check_wrong_provider(mapper, exe, wrong):
    with TemporaryDirectory() as temp:
        root = Path(temp)
        shutil.copyfile(exe, root / EXE)
        shutil.copyfile(wrong, root / MODULE)
        # Avoid starting the intentionally broken EXE through the native loader:
        # missing-ordinal loader dialogs can block an unattended CI session.
        mapped = launch([sys.executable, "-B", str(TOOL), str(mapper), str(root), EXE])
        assert mapped.returncode == 1 and not mapped.stdout, mapped
        assert "bundled_export_not_found" in mapped.stderr and "#77" in mapped.stderr, mapped.stderr
        print("wrong ordinal @78: package runner rejects before guest entry")
    with TemporaryDirectory() as temp:
        root = Path(temp)
        shutil.copyfile(exe, root / EXE)
        absent = launch([sys.executable, "-B", str(TOOL), str(mapper), str(root), EXE])
        assert absent.returncode == 1 and not absent.stdout, absent
        assert "unresolved external symbol" in absent.stderr, absent.stderr
        print("missing DLL: package runner rejects before guest entry")


def check_lingering(exe, returning, live):
    with TemporaryDirectory(ignore_cleanup_errors=True) as temp:  # a killed control may hold its EXE briefly
        root = Path(temp)
        shutil.copyfile(exe, root / EXE)
        shutil.copyfile(returning, root / RETURN_EXE)
        shutil.copyfile(live, root / MODULE)
        try:
            native = subprocess.run([str(root / EXE)], capture_output=True, text=True, timeout=LINGER_DEADLINE)
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f"lingering thread: native EXE still alive after {LINGER_DEADLINE} s") from error
        assert (native.returncode, native.stdout, native.stderr) == (13, "", ""), native
        try:
            control = subprocess.run([str(root / RETURN_EXE)], capture_output=True, timeout=RETURN_DEADLINE)
        except subprocess.TimeoutExpired:
            pass  # expected: the entry returned, and the provider's thread keeps the process alive
        else:
            raise AssertionError(f"returning control ended with {control.returncode:#x} within {RETURN_DEADLINE} s; "
                                 "the lingering-thread control does not hold the process open")
    print(f"lingering thread: native EXE exited 13 while the provider's thread was alive; "
          f"the returning control was still alive after {RETURN_DEADLINE} s")


def check_mapper_ordinal(mapper, provider):
    # Exercise the new PE export lookup directly, including holes and invalid ordinals.
    # The mapper executes the source-built DLL's trivial process-attach only.
    lib = ctypes.CDLL(str(mapper))
    mapped_fn = lib.pe_map_image
    mapped_fn.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
                         ctypes.c_char_p, ctypes.c_size_t]
    mapped_fn.restype = ctypes.c_void_p
    export = lib.pe_map_export_ordinal
    export.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    export.restype = ctypes.c_void_p
    release = lib.pe_map_release
    release.argtypes = [ctypes.c_void_p]
    release.restype = ctypes.c_int
    status, detail = ctypes.c_uint32(0xffff), ctypes.create_string_buffer(256)
    base = mapped_fn(str(provider), None, ctypes.byref(status), detail, len(detail))
    assert base and status.value == 0, (status.value, detail.value)
    try:
        fn = export(base, 77)
        assert fn, "NONAME ordinal @77 should be callable"
        assert all(not export(base, x) for x in (0, 1, 76, 78, 65535, 65536)), "invalid ordinals returned code"
        assert not export(None, 77), "null mapping returned code"
        output = (ctypes.c_uint64 * 3)()
        result = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
                                 ctypes.POINTER(ctypes.c_uint64))(fn)(5, 3, output)
        assert result == 13 and tuple(output) == (5, 3, 13), (result, tuple(output))
        print("pe_map_export_ordinal: NONAME @77 callable; six invalid ordinal controls rejected")
    finally:
        assert release(base) == 1


def main():
    if len(sys.argv) != 8:
        raise SystemExit("usage: package_ordinal_check.py pe_map.exe exe provider altered wrong live exe_return")
    mapper, exe, provider, altered, wrong, live, returning = map(Path, sys.argv[1:])
    import_fixture(exe, provider, altered, wrong, live, returning)
    check_case(mapper, exe, provider, 13)
    check_case(mapper, exe, altered, 14)
    check_wrong_provider(mapper, exe, wrong)
    check_lingering(exe, returning, live)
    check_mapper_ordinal(mapper, provider)


if __name__ == "__main__":
    main()
