#!/usr/bin/env python3
"""Check that tools/startup/run.ps1 exits with the startup debugger's exit code.

On Windows the script runs under every PowerShell found (powershell is Windows
PowerShell 5.1, pwsh is PowerShell 7) in a scratch tree whose debug_launch.py
is a stub, so no process is ever started under a debugger. On other hosts,
without a PowerShell, or with --text-only, only the text after the debugger
call is checked. Mutated copies of the script must fail the same checks.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_PS1 = Path(__file__).resolve().parent.parent / "tools" / "startup" / "run.ps1"
DLLS = ["xmem", "d3d12_xs", "AcpHal", "XCRDAPI", "chad", "xapu", "xg_xs", "GameInput"]
STUB_LINES = [f"stub debugger line {n}" for n in range(1, 8)]
HEAD_LINES = 5  # run.ps1 prints this many lines of "<LogDir>.out"
# The stub stands in for tools/startup/debug_launch.py: it records its arguments,
# prints seven lines and exits with the code under test.
STUB = """\
import json
import sys
from pathlib import Path

Path(__file__).with_name("argv.json").write_text(json.dumps(sys.argv[1:]), encoding="utf-8")
for n in range(1, 8):
    print("stub debugger line", n)
sys.exit({code})
"""
CAPTURE = re.compile(r"\$(\w+)\s*=\s*\$LASTEXITCODE", re.I)
EXIT = re.compile(r"exit\s+\$(\w+)", re.I)
HEAD = 'Get-Content "$LogDir.out" -TotalCount 5'


def decode(raw):
    # Windows PowerShell 5.1 writes a redirected file as UTF-16 with a byte order mark.
    wide = raw[:2] in (b"\xff\xfe", b"\xfe\xff")
    return raw.decode("utf-16" if wide else "utf-8-sig", errors="replace")


def run_case(shell, script, code, work):
    """Run one copy of the script on a stub that exits with `code`; return what is wrong."""
    root = Path(tempfile.mkdtemp(dir=work))
    startup = root / "tools" / "startup"
    startup.mkdir(parents=True)
    (startup / "run.ps1").write_bytes(script.encode("utf-8"))
    (startup / "debug_launch.py").write_text(STUB.format(code=code), encoding="utf-8")
    (root / "bld").mkdir()
    for dll in DLLS:  # the stand-ins the script copies next to the executable
        (root / "bld" / f"{dll}.dll").write_bytes(b"")
    exe, data, log = root / "run" / "t.exe", root / "data", root / "log"
    exe.parent.mkdir()
    exe.write_bytes(b"")
    data.mkdir()
    env = dict(os.environ)
    # In this tree run.ps1 finds `python` through PATH. This interpreter's directory
    # goes first so the stub runs under the Python that runs this check.
    env["PATH"] = os.path.dirname(sys.executable) + os.pathsep + env.get("PATH", "")
    command = [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(startup / "run.ps1"),
               "-Exe", str(exe), "-DataDir", str(data), "-LogDir", str(log), "-BuildDir", "bld",
               "-Seconds", "9", "-MemoryGiB", "3", "-ContinueAsserts", "4"]
    try:
        out = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True,
                             errors="replace", timeout=120)
    except subprocess.TimeoutExpired:
        return [f"stub exit {code}: run.ps1 did not finish in 120 s"]
    found = []
    if out.returncode != code:
        found.append(f"stub debugger exited {code}, run.ps1 exited {out.returncode}")
    printed = [line.strip() for line in out.stdout.splitlines() if line.strip()]
    if printed != STUB_LINES[:HEAD_LINES]:
        found.append(f"stub exit {code}: printed {printed} instead of the first {HEAD_LINES} "
                     f"debugger lines (stderr: {out.stderr.strip()!r})")
    summary = Path(str(log) + ".out")
    if not summary.is_file():
        found.append(f"stub exit {code}: {summary.name} was not written")
    elif [line.strip() for line in decode(summary.read_bytes()).splitlines() if line.strip()] != STUB_LINES:
        found.append(f"stub exit {code}: {summary.name} does not hold the debugger's output")
    want = [str(exe), str(data), str(log), "--timeout", "9", "--memory-gib", "3",
            "--trace-files", "1", "--continue-asserts", "4",
            # LOCAL run.ps1 switches -FastExit (off) and -LogRepeats (default 200) are always forwarded
            "--fast-exit", "0", "--log-repeats", "200"]
    record = startup / "argv.json"
    got = json.loads(record.read_text(encoding="utf-8")) if record.is_file() else None
    if got != want:
        found.append(f"stub exit {code}: the debugger received {got}, expected {want}")
    return found


def run_problems(shell, script, work, first_only):
    found = []
    for code in (2, 0, 7):  # 2 is the debugger's "CreateProcess failed"; 7 rules out a fixed code
        found += run_case(shell, script, code, work)
        if found and first_only:
            break
    return found


def text_problems(script):
    code = re.sub(r"<#.*?#>", "", script, flags=re.S)  # the help block names debug_launch.py too
    statements = []
    for line in (line.strip() for line in code.splitlines()):
        if not line or line.startswith("#"):
            continue
        if statements and statements[-1].endswith("`"):  # continuation line
            statements[-1] = statements[-1][:-1] + line
        else:
            statements.append(line)
    # The debugger call is the one statement that names debug_launch.py, however
    # the interpreter is spelled (python, & $python, ...).
    calls = [i for i, statement in enumerate(statements) if "debug_launch.py" in statement]
    if len(calls) != 1:
        return [f"expected one statement that names debug_launch.py, found {len(calls)}"]
    after = statements[calls[0] + 1:]
    capture = CAPTURE.fullmatch(after[0]) if after else None
    if not capture:
        return ["the statement after the debugger call does not store $LASTEXITCODE"]
    found = []
    leave = EXIT.fullmatch(after[-1])
    if not leave or leave.group(1).lower() != capture.group(1).lower():
        found.append(f"the script does not end with 'exit ${capture.group(1)}'")
    if HEAD not in after[1:-1]:
        found.append("the log head is not printed between the capture and the exit")
    return found


def mutants_of(script):
    lines = script.splitlines()
    is_exit = lambda line: bool(EXIT.fullmatch(line.strip()))
    is_capture = lambda line: bool(CAPTURE.fullmatch(line.strip()))
    early = []
    for line in lines:
        if line.strip() == HEAD:
            early += [other for other in lines if is_exit(other)]
        if not is_exit(line):
            early.append(line)
    mutants = {
        "no capture and no exit": [line for line in lines if not is_exit(line) and not is_capture(line)],
        "fixed exit 2": ["exit 2" if is_exit(line) else line for line in lines],
        "exit before the log head": early,
    }
    return {name: None if mutant == lines else "\n".join(mutant) + "\n" for name, mutant in mutants.items()}


def main():
    if sys.argv[1:] not in ([], ["--text-only"]):
        sys.exit("usage: startup_run_ps1_check.py [--text-only]")
    script = RUN_PS1.read_bytes().decode("utf-8")
    shells = []
    if sys.argv[1:]:
        mode = "text only: requested"
    elif os.name != "nt":  # the launcher and its backslash build paths are Windows-only
        mode = "text only: not a Windows host"
    else:
        shells = [found for found in map(shutil.which, ("powershell", "pwsh")) if found]
        mode = "executed with " + ", ".join(Path(shell).stem.lower() for shell in shells)
        if not shells:
            mode = "text only: no PowerShell found"
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        checks = {Path(shell).stem.lower(): (lambda text, first_only, shell=shell:
                                             run_problems(shell, text, tmp, first_only))
                  for shell in shells}
        if not shells:
            checks = {"text": lambda text, first_only: text_problems(text)}
        for name, check in checks.items():
            bad = check(script, False)
            failures += [f"{name}: {item}" for item in bad]
            if bad:
                continue
            for label, mutant in mutants_of(script).items():
                if mutant is None or not check(mutant, True):
                    failures.append(f"{name}: mutant '{label}' was not detected")
    for item in failures:
        print("FAIL", item)
    print(f"startup run.ps1 exit code: {len(failures)} failed ({mode})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
