"""Check pe_run_package.py --trap-callable against source-built EXEs.

trap_missing_exe imports trap_fixture.dll!trap_fixture_alpha and !trap_fixture_beta, which no
package provides, and reads its mode from TRAP_FIXTURE_MODE (see trap_missing_exe.cpp).
trap_missing_data_exe also imports trap_fixture_data as data.

- Without --trap-callable the runner refuses each package before mapping, as before.
- With alpha and beta listed, the call to alpha ends the process with trap 1 and names that
  import, and the call to beta ends it with trap 2. Neither trap returns to the caller (0xBAD).
- When the EXE calls neither, its own exit code is reported and both traps are unhit.
- An unresolved external that is not listed stays a refusal: a partial list, and the data
  import, which an import table entry does not mark as data or code.
- A listed name that is not an unresolved external, a malformed name and --preflight-only
  are refused.
- A trap terminates even when its report fails: a child process closes fd 2, replaces
  sys.stderr and calls a trap thunk directly.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "pe_run_package.py"
sys.path.insert(0, str(ROOT / "tools"))
from pe_run_package import TRAP_EXIT_BASE  # noqa: E402

EXE = "trap_missing_exe.exe"
DATA_EXE = "trap_missing_data_exe.exe"
ALPHA = "trap_fixture.dll!trap_fixture_alpha"
BETA = "trap_fixture.dll!trap_fixture_beta"
DATA = "trap_fixture.dll!trap_fixture_data"
LISTED = ["--trap-callable", ALPHA, "--trap-callable", BETA]
HEADER = ("trap-callable: 2 listed unresolved external(s) bound to terminating traps; "
          "diagnostic only, not providers")
LISTING = [HEADER, f"  trap 1 (exit {TRAP_EXIT_BASE | 1:#x}): {ALPHA}",
           f"  trap 2 (exit {TRAP_EXIT_BASE | 2:#x}): {BETA}"]
TIMEOUT = 60

# Child for the failed-report control: bind one trap, break every diagnostic stream, call it.
FAILED_REPORT_CHILD = r"""
import ctypes, io, os, sys
sys.path.insert(0, sys.argv[1])
from pe_run_package import bind_traps
key = ("trap_fixture.dll", "trap_fixture_alpha")
addresses, callbacks, labels = bind_traps(ctypes.WinDLL("kernel32"), {key: {"fixture.exe"}})
os.close(2)
sys.stderr = io.StringIO()
ctypes.WINFUNCTYPE(None)(addresses[key])()
print("trap returned", flush=True)
sys.exit(0)
"""


def run(mapper, package, entry, *flags, mode=None):
    env = dict(os.environ)
    env.pop("TRAP_FIXTURE_MODE", None)
    if mode is not None:
        env["TRAP_FIXTURE_MODE"] = mode
    command = [sys.executable, "-B", str(TOOL), str(mapper), str(package), entry,
               "--system", "kernel32.dll", *flags]
    done = subprocess.run(command, capture_output=True, text=True, env=env, timeout=TIMEOUT)
    return done.returncode & 0xFFFFFFFF, done.stdout, done.stderr.splitlines()


def expect(label, actual, expected):
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")


def expect_refusal(label, result, message):
    code, out, err = result
    expect(f"{label} exit", code, 1)
    expect(f"{label} stdout", out, "")
    expect(f"{label} stderr", err, [message])
    print(f"{label}: refused before mapping: {message}")


def check_failed_report():
    done = subprocess.run([sys.executable, "-B", "-c", FAILED_REPORT_CHILD, str(ROOT / "tools")],
                          capture_output=True, text=True, timeout=TIMEOUT)
    expect("failed-report exit", done.returncode & 0xFFFFFFFF, TRAP_EXIT_BASE | 1)
    expect("failed-report stdout", done.stdout, "")
    print(f"failed report: with fd 2 closed and sys.stderr replaced, the trap still ended the child "
          f"with {TRAP_EXIT_BASE | 1:#x}")


def check(mapper, exe, data_exe):
    with TemporaryDirectory() as temp:
        package = Path(temp) / "calls"
        data_package = Path(temp) / "data"
        package.mkdir()
        data_package.mkdir()
        shutil.copyfile(exe, package / EXE)
        shutil.copyfile(data_exe, data_package / DATA_EXE)

        expect_refusal("default", run(mapper, package, EXE),
                       f"2 unresolved external symbol(s), first {EXE}: {ALPHA}")

        for mode, number, label in ((None, 1, ALPHA), ("alpha", 1, ALPHA), ("beta", 2, BETA)):
            code, out, err = run(mapper, package, EXE, *LISTED, mode=mode)
            expect(f"{mode} exit", code, TRAP_EXIT_BASE | number)
            expect(f"{mode} stdout", out, "")
            expect(f"{mode} stderr", err, [*LISTING, f"trap {number}: {EXE}: {label}"])
            print(f"mode {mode or 'unset'}: trap {number} named {label} and ended the process with {code:#x}")

        code, out, err = run(mapper, package, EXE, *LISTED, mode="none")
        expect("none exit", code, 0)
        expect("none stderr", err, LISTING)
        expect("none result", json.loads(out), {
            "image_order": [EXE], "entry_executed": True, "exit_code": 0x7A0,
            "unhit_traps": [ALPHA, BETA],
        })
        print("mode none: the EXE returned 0x7a0 and both traps were reported unhit")

        expect_refusal("partial list", run(mapper, package, EXE, "--trap-callable", ALPHA),
                       f"1 unresolved external symbol(s) not listed with --trap-callable, first {EXE}: {BETA}")
        expect_refusal("stale name",
                       run(mapper, package, EXE, *LISTED, "--trap-callable", "trap_fixture.dll!absent"),
                       "--trap-callable trap_fixture.dll!absent is not an unresolved external")
        expect_refusal("malformed name", run(mapper, package, EXE, "--trap-callable", "trap_fixture_alpha"),
                       "--trap-callable expects DLL!SYMBOL or DLL!#ORDINAL, got 'trap_fixture_alpha'")

        expect_refusal("data import, default", run(mapper, data_package, DATA_EXE),
                       f"3 unresolved external symbol(s), first {DATA_EXE}: {ALPHA}")
        for mode in (None, "data"):
            expect_refusal(f"data import, callables listed, mode {mode or 'unset'}",
                           run(mapper, data_package, DATA_EXE, *LISTED, mode=mode),
                           f"1 unresolved external symbol(s) not listed with --trap-callable, "
                           f"first {DATA_EXE}: {DATA}")

        code, out, err = run(mapper, package, EXE, *LISTED, "--preflight-only")
        expect("combined exit", code, 2)
        if not any("not allowed with argument" in line for line in err):
            raise AssertionError(f"combined flags: {err!r}")
        print("--trap-callable with --preflight-only: refused by the argument parser")

        expect("package contents", sorted(p.name for p in package.iterdir()), [EXE])
        expect("data package contents", sorted(p.name for p in data_package.iterdir()), [DATA_EXE])
    check_failed_report()
    print("trap-callable checks passed")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: trap_missing_check.py PE_MAP_DLL TRAP_MISSING_EXE TRAP_MISSING_DATA_EXE")
    check(*(Path(arg).resolve(strict=True) for arg in sys.argv[1:]))
