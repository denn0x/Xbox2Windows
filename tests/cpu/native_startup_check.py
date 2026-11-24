"""Compare two-DLL EXE startup and TLS callback observations under Windows and pe_map."""

import argparse
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile

from native_map_check import Mapper, Resolver, STATUS

A, B, EXE = "cpu_startup_a.dll", "cpu_startup_b.dll", "cpu_startup_exe.exe"
TIMEOUT = 30  # per process; longer than the 10 s wait inside the EXE, so its failure code is observed first
# startup_*.cpp: a_main logs 'a' and sets 7; b_main logs 'b' and publishes a_value() * 2; the EXE's TLS
# callback logs 't', its entry point 'e', each of two workers 'w' + its TlsGetValue (2, then 3), and the
# main thread 'm' + 1. Exit code: b_value | first worker << 8 | second worker << 16 | main << 24.
LOG = "abtew2w3m1"
EXIT = 14 | (2 << 8) | (3 << 16) | (1 << 24)
LOG_WITHOUT_B, EXIT_WITHOUT_B = "atew2w3m1", (2 << 8) | (3 << 16) | (1 << 24)
CREATE_FAILED, WAIT_TIMED_OUT = 0xE0000002, 0xE0000003  # startup_exe.cpp failure codes
EXIT_ALTERED_A = 18 | (2 << 8) | (3 << 16) | (1 << 24)  # a_value 9 instead of 7
LINGER_DEADLINE = 10  # s; the EXE itself ends in milliseconds
RETURN_DEADLINE = 3  # s; the returning control never ends by itself while its thread sleeps


def observed(done):
    log = next((line[4:] for line in done.stdout.decode().splitlines() if line.startswith("LOG:")), None)
    return {"log": log, "exit": done.returncode & 0xffffffff,
            "stdout": done.stdout.decode(), "stderr": done.stderr.decode()}


def check_tls(loader, result):
    def read_fields(line, prefix):
        if not line.startswith(prefix):
            raise AssertionError(f"{loader}: expected {prefix} in {line!r}")
        fields = tuple(int(value, 16) for value in line[len(prefix):].split())
        if len(fields) != 3:
            raise AssertionError(f"{loader}: expected three fields in {line!r}")
        return fields

    lines = result["stdout"].splitlines()
    if len(lines) < 2 or lines[0] != f"LOG:{result['log']}":
        raise AssertionError(f"{loader}: missing startup log or thread IDs: {result['stdout']!r}")
    main, first, second = read_fields(lines[1], "THREADS:")
    if not all((main, first, second)) or len({main, first, second}) != 3:
        raise AssertionError(f"{loader}: expected distinct main and worker thread IDs: {lines[1]}")
    # Host threads can trigger a DLL notification too. Compare only the threads this fixture started.
    thread_ids = {main, first, second}
    rows = [read_fields(line, "TLS:") for line in lines[2:]]
    actual = [row for row in rows if row[0] in thread_ids]
    # PROCESS_ATTACH = 1, THREAD_ATTACH = 2, THREAD_DETACH = 3.
    expected = [(main, 1, 1), (first, 2, 0), (first, 3, 2), (second, 2, 0), (second, 3, 3)]
    if actual != expected:
        raise AssertionError(f"{loader} TLS callback (thread, reason, value): {actual} != {expected}")
    print(f"{loader} TLS callbacks (thread, reason, value): {actual}")


def native(exe, a, b):
    """Launch the EXE in a directory holding the two DLLs under the names it imports."""
    with TemporaryDirectory() as temp:
        for name, path in ((A, a), (B, b), (EXE, exe)):
            shutil.copy(path, Path(temp) / name)
        return observed(subprocess.run([str(Path(temp) / EXE)], capture_output=True, timeout=TIMEOUT))


def mapped(mapper, exe, a, b, reverse=False):
    """pe_map in a child process; its stdout and stderr belong to the guest, the result goes to a file."""
    with TemporaryDirectory() as temp:
        result_file = Path(temp) / "result.json"
        command = [sys.executable, "-B", __file__, "--run", str(mapper), str(result_file),
                   f"{A}={a}", f"{B}={b}", f"{EXE}={exe}"]
        done = subprocess.run(command + (["--reverse"] if reverse else []), capture_output=True, timeout=TIMEOUT)
        if done.returncode:
            raise AssertionError(f"mapped child exited {done.returncode:#x}: {done.stderr.decode()}")
        return {**observed(done), **json.loads(result_file.read_text())}


def dependency_order(images):
    """Dependencies first, from the import tables alone; the EXE ends up last because nothing imports it."""
    order = []

    def visit(name):
        if name in order:
            return
        with pefile.PE(str(images[name]), fast_load=True) as pe:
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
            wanted = [e.dll.decode().casefold() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])]
        for dependency in wanted:
            if dependency in images:
                visit(dependency)
        order.append(name)

    for name in sorted(images):
        visit(name)
    return order


def check_pe_dependencies(exe, a, b):
    """The ordering the harness derives rests on real import-table entries, not on file names it was given."""
    def imported(path):
        with pefile.PE(str(path), fast_load=True) as pe:
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
            return {e.dll.decode(): {i.name.decode() for i in e.imports if i.name}
                    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])}

    a_imports, b_imports, exe_imports = imported(a), imported(b), imported(exe)
    if A in a_imports or B in a_imports:
        raise AssertionError(f"A must not depend on the fixture DLLs: {a_imports}")
    if b_imports.get(A) != {"a_log", "a_value"} or B in b_imports:
        raise AssertionError(f"B must import exactly a_log and a_value from A: {b_imports}")
    a_exports = {"a_log", "a_snapshot", "a_tls_slot", "a_tls_snapshot"}
    if exe_imports.get(B) != {"b_value"} or not a_exports <= exe_imports.get(A, set()):
        raise AssertionError(f"the EXE must import from both DLLs: {exe_imports}")
    print(f"PE imports: A none of the fixtures, B -> {sorted(b_imports[A])} of A, "
          f"EXE -> {sorted(exe_imports[A])} of A and {sorted(exe_imports[B])} of B")


def run(mapper_path, result_file, specs, reverse):
    """Child process. The harness orders the images and serves the DLL imports with pe_map_export; pe_map maps,
    relocates, runs each DLL's attach and starts the EXE."""
    mapper = Mapper(mapper_path)
    start = ctypes.CDLL(str(mapper_path)).pe_map_run
    start.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    start.restype = ctypes.c_int
    images = dict(spec.split("=", 1) for spec in specs)
    order = dependency_order(images)
    if reverse:
        order.reverse()
    live, requests, system = {}, [], {}

    def resolve(dll, name, ordinal):
        dll = dll.decode()
        requests.append([dll, name.decode() if name else ordinal])
        if dll in images:  # served by the image mapped earlier, never by the file on disk
            return mapper.export(live[dll], name) if dll in live and name else None
        module = system.setdefault(dll, ctypes.WinDLL(dll))
        return ctypes.cast(getattr(module, name.decode()), ctypes.c_void_p).value if name else None

    callback = Resolver(resolve)
    mapped_order, failure, exit_code, started = [], None, ctypes.c_uint32(0xdeadbeef), None
    for name in order:
        status, detail = ctypes.c_uint32(0xffffffff), ctypes.create_string_buffer(256)
        base = mapper.map(images[name], callback, ctypes.byref(status), detail, len(detail))
        if not base:
            failure = [name, status.value, detail.value.decode()]
            break
        live[name] = base
        mapped_order.append(name)
    if not failure:
        started = start(live[EXE], ctypes.byref(exit_code))
    for name in reversed(mapped_order):
        mapper.release(live[name])
    Path(result_file).write_text(json.dumps({"order": mapped_order, "failure": failure, "started": started,
                                             "exit": exit_code.value, "requests": requests}))


def check_failure_control(mapper, exe_control, a, b, expected, what):
    """A worker that cannot be created or outlives the wait must end the process with a fixed code, under both
    loaders, before any result is consumed or the log is printed."""
    with TemporaryDirectory() as temp:
        for name, path in ((A, a), (B, b), (EXE, exe_control)):
            shutil.copy(path, Path(temp) / name)
        native_done = subprocess.run([str(Path(temp) / EXE)], capture_output=True, timeout=TIMEOUT)
        command = [sys.executable, "-B", __file__, "--run", str(mapper), str(Path(temp) / "unused.json"),
                   f"{A}={a}", f"{B}={b}", f"{EXE}={exe_control}"]
        mapped_done = subprocess.run(command, capture_output=True, timeout=TIMEOUT)
    codes = (native_done.returncode & 0xffffffff, mapped_done.returncode & 0xffffffff)
    if codes != (expected, expected) or native_done.stdout or mapped_done.stdout:
        raise AssertionError(f"{what}: exit codes {codes[0]:#x} and {codes[1]:#x}, "
                             f"stdout {native_done.stdout!r} and {mapped_done.stdout!r}")
    print(f"control, {what}: both processes end with {expected:#x} before consuming a result or printing the log")


def check_lingering(exe_linger, exe_linger_return, a, b):
    """#1375: a thread that outlives the entry point must not hold the native process open. The _linger build
    leaves one running and must still end with its result; the _linger_return build returns from its entry
    point instead and must stay alive until it is killed, which shows the control sees a lingering process.
    Native launches only: in a host mapping the thread would run unmapped code after the release."""
    with TemporaryDirectory(ignore_cleanup_errors=True) as temp:  # a killed control may hold its EXE briefly
        for name, path in ((A, a), (B, b), (EXE, exe_linger), ("returning.exe", exe_linger_return)):
            shutil.copy(path, Path(temp) / name)
        try:
            done = observed(subprocess.run([str(Path(temp) / EXE)], capture_output=True, timeout=LINGER_DEADLINE))
        except subprocess.TimeoutExpired as error:
            raise AssertionError(f"lingering thread: native EXE still alive after {LINGER_DEADLINE} s") from error
        if (done["log"], done["exit"]) != (LOG, EXIT) or done["stderr"]:
            raise AssertionError(f"lingering thread: native launch {done}")
        try:
            control = subprocess.run([str(Path(temp) / "returning.exe")], capture_output=True, timeout=RETURN_DEADLINE)
        except subprocess.TimeoutExpired:
            pass  # expected: the entry returned, and the lingering thread keeps the process alive
        else:
            raise AssertionError(f"returning control ended with {control.returncode & 0xffffffff:#x} within "
                                 f"{RETURN_DEADLINE} s; the lingering-thread control does not hold the process open")
    print(f"lingering thread: native EXE exited {EXIT:#x} with the full log while its thread was alive; "
          f"the returning control was still alive after {RETURN_DEADLINE} s")


def check_package_runner(mapper, exe, a, b, native_result):
    tool = Path(__file__).resolve().parents[2] / "tools" / "pe_run_package.py"
    with TemporaryDirectory() as temp:
        package = Path(temp)
        for name, path in ((A, a), (B, b), (EXE, exe)):
            shutil.copy(path, package / name)
        command = [sys.executable, "-B", str(tool), str(mapper), str(package), EXE]
        failed = subprocess.run(command, capture_output=True, timeout=TIMEOUT)
        if failed.returncode != 1 or failed.stdout or b"unresolved external symbol" not in failed.stderr:
            raise AssertionError(f"package runner accepted missing host provider: {failed}")
        command += ["--system", "kernel32.dll"]
        done = subprocess.run(command, capture_output=True, timeout=TIMEOUT)
        if done.returncode or done.stderr:
            raise AssertionError(f"package runner failed: {done}")
        lines = done.stdout.decode().splitlines()
        log = next(line[4:] for line in lines if line.startswith("LOG:"))
        start_of_json = lines.index("{")
        result = json.loads("\n".join(lines[start_of_json:]))
        if (log, result) != (native_result["log"], {
            "image_order": [A, B, EXE], "entry_executed": True, "exit_code": native_result["exit"],
        }):
            raise AssertionError(f"package runner differs from Windows: {log!r} {result!r}")
        print("package runner found both dependencies, matched the native entry trace, and refused a missing provider")


def check(mapper, exe, a, b, b_noinit, a_alt, exe_timeout, exe_create, exe_linger, exe_linger_return):
    check_pe_dependencies(exe, a, b)
    check_failure_control(mapper, exe_timeout, a, b, WAIT_TIMED_OUT, "a worker that outlives the wait")
    check_failure_control(mapper, exe_create, a, b, CREATE_FAILED, "a worker thread that cannot be created")
    nat, map_ = native(exe, a, b), mapped(mapper, exe, a, b)
    print(f"native: LOG:{nat['log']} exit {nat['exit']:#x}")
    print(f"mapped: LOG:{map_['log']} exit {map_['exit']:#x}; mapped in order {map_['order']}")
    if (nat["log"], nat["exit"]) != (LOG, EXIT):
        raise AssertionError(f"native launch: {nat}")
    if nat["stderr"] or map_["stderr"]:
        raise AssertionError(f"stderr: native {nat['stderr']!r}, mapped {map_['stderr']!r}")
    if (map_["log"], map_["exit"], map_["started"], map_["failure"]) != (LOG, EXIT, 1, None):
        raise AssertionError(f"mapped launch differs from native: {map_}")
    check_tls("native", nat)
    check_tls("mapped", map_)
    if map_["order"] != [A, B, EXE]:
        raise AssertionError(f"dependency order {map_['order']}")
    check_package_runner(mapper, exe, a, b, nat)
    for event in "abte":  # initializers, TLS callback and entry point ran exactly once
        if map_["log"].count(event) != 1:
            raise AssertionError(f"event {event!r} not logged exactly once in {map_['log']}")
    served = {tuple(r) for r in map_["requests"]}
    expected_imports = {(A, "a_value"), (B, "b_value"), (A, "a_log"), (A, "a_tls_slot"), (A, "a_tls_snapshot")}
    if not expected_imports <= served:
        raise AssertionError(f"import requests {map_['requests']}")

    for name, dlls, expected in (("without B's initializer", (a, b_noinit), (LOG_WITHOUT_B, EXIT_WITHOUT_B)),
                                 ("with another A under the imported name", (a_alt, b), (LOG, EXIT_ALTERED_A))):
        controls = native(exe, *dlls), mapped(mapper, exe, *dlls)
        for loader, result in zip(("native", "mapped"), controls):
            if (result["log"], result["exit"]) != expected:
                raise AssertionError(f"{loader} {name}: {result['log']} {result['exit']:#x}")
            check_tls(f"{loader} {name}", result)
        print(f"control {name}: LOG:{controls[1]['log']} exit {controls[1]['exit']:#x} under both loaders; "
              f"differs from the original in {'the log and ' if expected[0] != LOG else ''}the exit code")

    wrong = mapped(mapper, exe, a, b, reverse=True)
    if not wrong["failure"] or wrong["failure"][1] != STATUS["unresolved"] or wrong["started"] is not None:
        raise AssertionError(f"mapping out of dependency order: {wrong}")
    print(f"mapping the EXE before its DLLs: {wrong['failure'][2]}")
    check_lingering(exe_linger, exe_linger_return, a, b)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--run":
        flagged = sys.argv[-1] == "--reverse"
        run(sys.argv[2], sys.argv[3], sys.argv[4:-1] if flagged else sys.argv[4:], flagged)
    else:
        parser = argparse.ArgumentParser()
        for argument in ("mapper", "exe", "a", "b", "b_noinit", "a_alt", "exe_timeout", "exe_create", "exe_linger",
                         "exe_linger_return"):
            parser.add_argument(argument, type=Path)
        args = parser.parse_args()
        check(args.mapper, args.exe, args.a, args.b, args.b_noinit, args.a_alt, args.exe_timeout, args.exe_create,
              args.exe_linger, args.exe_linger_return)
