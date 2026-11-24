#!/usr/bin/env python3
"""Check the opt-in sccache block of tools/dev.ps1.

With pwsh on PATH the block is executed (and the whole script is parsed) for
each environment; without it only the shape of the block is checked. Mutated
copies of the block must fail the same checks.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEV = Path(__file__).resolve().parent.parent / "tools" / "dev.ps1"
EXPECTED = [
    "-DCMAKE_C_COMPILER_LAUNCHER=sccache",
    "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
    "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded",
    "-DCMAKE_POLICY_DEFAULT_CMP0141=NEW",
]
CLEARED = ["-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER="]
RUNNER = "\nforeach ($a in $global:X2wSccacheCmakeArgs) { Write-Output ('ARG=' + $a) }\n"


def block_of(text):
    m = re.search(r"# x2w-sccache-begin\n(.*?)# x2w-sccache-end", text, re.S)
    if not m:
        sys.exit("dev.ps1 has no x2w-sccache block")
    return m.group(1)


def run_block(pwsh, block, env_value, with_sccache, work):
    script = work / "block.ps1"
    script.write_text(block + RUNNER, encoding="utf-8")
    bindir = work / ("with" if with_sccache else "without")
    bindir.mkdir(exist_ok=True)
    if with_sccache:
        if os.name == "nt":  # found through PATHEXT
            (bindir / "sccache.cmd").write_text("@exit /b 0\r\n")
        else:
            fake = bindir / "sccache"
            fake.write_text("#!/bin/sh\nexit 0\n")
            fake.chmod(0o755)
    env = {k: v for k, v in os.environ.items() if k != "X2W_USE_SCCACHE"}
    # Only the stub directory (plus System32 on Windows): pwsh is started by
    # absolute path, and its own directory may also hold a real sccache.
    env["PATH"] = os.pathsep.join([str(bindir)] +
                                  ([os.path.join(os.environ["SystemRoot"], "System32")] if os.name == "nt" else []))
    if env_value is not None:
        env["X2W_USE_SCCACHE"] = env_value
    out = subprocess.run([pwsh, "-NoProfile", "-File", str(script)], env=env,
                         capture_output=True, text=True)
    if out.returncode:
        return None, True
    text = out.stdout + out.stderr
    args = re.findall(r"^ARG=(.*)$", text, re.M)
    return args, "WARNING" in text


def problems(pwsh, block, work):
    found = []
    cases = [  # env value, sccache on PATH, expected args, warning expected
        (None, True, [], False), (None, False, [], False),
        ("0", True, [], False), ("true", True, [], False), ("", True, [], False),
        ("1", True, EXPECTED, False), ("1", False, CLEARED, True),
    ]
    for value, present, want, warn in cases:
        args, warned = run_block(pwsh, block, value, present, work)
        if args != want or warned != warn:
            found.append(f"X2W_USE_SCCACHE={value!r} sccache={present}: args={args} warned={warned}")
    return found


def static_problems(block):
    found = []
    if "$env:X2W_USE_SCCACHE -eq '1'" not in block:
        found.append("block is not gated on X2W_USE_SCCACHE -eq '1'")
    if "Get-Command sccache" not in block or "Write-Warning" not in block:
        found.append("missing sccache lookup or warning")
    defs = re.findall(r"'(-D[^']+)'", block)
    if defs != EXPECTED + CLEARED:
        found.append(f"definitions are {defs}")
    return found


def main():
    text = DEV.read_text(encoding="utf-8")
    block = block_of(text)
    pwsh = shutil.which(sys.argv[1] if len(sys.argv) > 1 else "pwsh")
    mutants = {
        "Z7 instead of Embedded": block.replace("Embedded", "Z7"),
        "always on": block.replace("-eq '1'", "-ne '0'"),
        "no warning": block.replace("Write-Warning", "Write-Verbose"),
        "fallback keeps launchers": block.replace("'-DCMAKE_C_COMPILER_LAUNCHER=',", "").replace("'-DCMAKE_CXX_COMPILER_LAUNCHER='", "$null"),
        "missing policy": block.replace("'-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'", ""),
    }
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        if pwsh:
            check = lambda b: problems(pwsh, b, work)
            parse = subprocess.run(
                [pwsh, "-NoProfile", "-Command",
                 "$e=$null;[void][System.Management.Automation.Language.Parser]::ParseFile("
                 f"'{DEV}',[ref]$null,[ref]$e);$e|ForEach-Object{{$_.Message}};exit [int]($e.Count -gt 0)"],
                capture_output=True, text=True)
            if parse.returncode:
                sys.exit("dev.ps1 does not parse: " + parse.stdout)
        else:
            print("pwsh not found: checking the block text only")
            check = static_problems
        bad = check(block)
        if bad:
            sys.exit("dev.ps1 sccache block: " + "; ".join(bad))
        for name, mutant in mutants.items():
            if mutant == block or not check(mutant):
                sys.exit(f"mutant '{name}' was not detected")
    print("dev.ps1 sccache opt-in: ok" + (" (executed with pwsh)" if pwsh else " (text only)"))


if __name__ == "__main__":
    main()
