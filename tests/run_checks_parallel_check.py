"""The parallel check runner reports every script's output and fails when any script fails."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "tools", "run_checks_parallel.py")


def run(*scripts):
    return subprocess.run([sys.executable, "-B", RUNNER, "-j", "2", *scripts],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


with tempfile.TemporaryDirectory() as tmp:
    good = os.path.join(tmp, "good.py")
    bad = os.path.join(tmp, "bad.py")
    with open(good, "w") as f:
        f.write("print('good ran')\n")
    with open(bad, "w") as f:
        f.write("import sys\nprint('bad ran')\nsys.exit(3)\n")
    ok = run(good, good)
    assert ok.returncode == 0 and ok.stdout.count("good ran") == 2, ok.stdout
    mixed = run(bad, good)
    assert mixed.returncode == 1, mixed.stdout
    assert "bad ran" in mixed.stdout and "good ran" in mixed.stdout and "FAILED: " + bad in mixed.stdout, mixed.stdout
    assert mixed.stdout.index("bad ran") < mixed.stdout.index("good ran"), "output must keep the given order"
print("run_checks_parallel_check: ok")
