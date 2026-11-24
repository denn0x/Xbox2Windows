"""A malformed PE makes the AOT command-line tools refuse with a message, never a Python traceback."""

from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from synthetic_pe import build_leaf_dll

TOOLS = Path(__file__).resolve().parents[2] / "tools"


def run(*command):
    return subprocess.run([sys.executable, "-B", *map(str, command)], capture_output=True, text=True, timeout=60)


def expect_refusal(label, result):
    if result.returncode != 1 or "Traceback" in result.stderr or not result.stderr.strip():
        raise AssertionError((label, result.returncode, result.stderr[-300:]))


def main():
    with TemporaryDirectory() as folder:
        folder = Path(folder)
        whole, short = folder / "whole.dll", folder / "short.dll"
        build_leaf_dll(whole)
        build_leaf_dll(short, directories=3)  # NumberOfRvaAndSizes stops before the runtime-function directory

        # Control: the same image with a complete directory table is translated.
        result = run(TOOLS / "cpu_leaf_aot.py", whole, "leaf", folder / "leaf.cpp")
        if result.returncode != 0 or "lifted" not in (folder / "leaf.cpp").read_text(encoding="utf-8"):
            raise AssertionError(("complete image", result.returncode, result.stderr[-300:]))

        for tool, extra in (("cpu_leaf_aot", ()), ("cpu_call_aot", ()), ("cpu_chained_aot", ())):
            result = run(TOOLS / f"{tool}.py", short, "leaf", folder / f"{tool}.cpp", *extra)
            expect_refusal(f"{tool} with a short directory table", result)
            if "runtime-function" not in result.stderr:
                raise AssertionError((tool, result.stderr[-300:]))

        # The EXE translator reports a missing file or an unsupported image the same way.
        for label, image in (("missing image", folder / "absent.exe"), ("DLL image", whole)):
            result = run(TOOLS / "cpu_exe_aot.py", image, folder / "io.dll", folder / "mix.dll", folder / "exe.cpp")
            expect_refusal(f"cpu_exe_aot with a {label}", result)
    print("ok")


if __name__ == "__main__":
    main()
