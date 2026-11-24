"""Syntax-check every Python file under tools/startup (Refs #1149).

Compiles each source without importing or running it, so a file that cannot be parsed is reported with its path
and line number. Nothing in this check executes the tools; a syntax error is the only thing it can see.

The self-test is what keeps a "0 problems" line meaningful: a deliberately broken snippet must be rejected and
a valid one accepted, so the harness is shown to observe parse errors at all rather than reporting that it
looked at nothing.
"""

import pathlib
import sys
import tempfile

DEFAULT_DIRECTORY = "tools/startup"


def compile_sources(directory):
    """Returns (files, problems) for every .py file under directory."""
    files = sorted(pathlib.Path(directory).rglob("*.py"))
    problems = []
    for path in files:
        try:
            # Compiling bytes lets Python honor its own BOM and encoding-cookie rules.
            source = path.read_bytes()
        except OSError as error:
            problems.append("{}: unreadable: {}".format(path, error))
            continue
        try:
            compile(source, str(path), "exec", dont_inherit=True)
        except SyntaxError as error:
            problems.append("{}:{}: {}".format(path, error.lineno, error.msg))
    return files, problems


def self_test():
    """Check the file-scanning path with valid encodings and invalid syntax."""
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        broken = root / "broken.py"
        broken.write_bytes(b"\ndef broken(:\n")
        (root / "utf8_bom.py").write_bytes(b"\xef\xbb\xbfanswer = 1\n")
        (root / "latin1.py").write_bytes(b"# coding: latin-1\nword = '\xe9'\n")
        files, problems = compile_sources(root)
        if len(files) != 3 or len(problems) != 1 or not problems[0].startswith(f"{broken}:2: "):
            return [f"control: expected only the syntax error at {broken}:2, got {problems}"]
    return []


def main(argv):
    directory = argv[1] if len(argv) > 1 else DEFAULT_DIRECTORY
    files, problems = compile_sources(directory)
    controls = self_test()
    if not files:
        print("FAIL: no Python files found under {}".format(directory))
        return 1
    for problem in problems:
        print("syntax error: {}".format(problem))
    for failure in controls:
        print(failure)
    if problems or controls:
        print("FAIL: {} of {} files under {} cannot be parsed".format(len(problems), len(files), directory))
        return 1
    print("PASS: {} files under {} parse; control snippet rejected and valid snippet accepted".format(
        len(files), directory))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
