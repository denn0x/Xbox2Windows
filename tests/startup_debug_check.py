"""Check three diagnostics of tools/startup/debug_launch.py on a source-built probe.

X2W_STOP_AT stops at a chosen main-image breakpoint while --continue-asserts steps over the
others, X2W_DUMP_AT writes thread stacks at a time during the run, and X2W_VALLOC_MIN logs large
virtual-memory allocations with their callers. The launcher also keeps the debug heap off unless
the caller sets _NO_DEBUG_HEAP=0. tests/startup_debug_probe.cpp is the only input: it hits two
breakpoints, reserves 256 MiB, prints its debug-heap flags, waits 3 s and exits 0.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
DEBUG = REPO / "tools/startup/debug_launch.py"


def run(probe, work, name, variables, options=("--continue-asserts", "10")):
    logdir = work / name
    env = {k: v for k, v in os.environ.items() if not k.startswith("X2W_") and k.upper() != "_NO_DEBUG_HEAP"}
    env.update(variables)
    command = [sys.executable, "-B", str(DEBUG), str(probe), str(work), str(logdir), "--timeout", "30", "--console", "0", *options]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, env=env)
    return result.stdout + result.stderr, logdir


def text(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def main(probe):
    sys.path.insert(0, str(DEBUG.parent))
    from debug_launch import read_qword
    word = bytes.fromhex("ab cd ef 01 23 45 67 89")
    for readable in range(8):
        assert read_qword(lambda address, size: word[:readable], 0) is None
    assert read_qword(lambda address, size: word[:size], 0) == 0x8967452301efcdab
    assert read_qword(lambda address, size: bytes(size), 0) == 0
    probe = Path(probe).resolve()
    image = probe.name
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)

        # Baseline: both breakpoints are stepped over and the probe exits 0.
        out, logdir = run(probe, work, "baseline", {})
        assert "process exited with 0x00000000" in out, out
        sites = [int(m, 16) for m in re.findall(r"breakpoint at %s\+0x([0-9a-f]+) first-chance" % re.escape(image),
                                                text(logdir / "events.txt"))]
        assert len(sites) == 2 and sites[0] != sites[1], text(logdir / "events.txt")
        assert not (logdir / "threads_stop.txt").exists() and "VALLOC" not in text(logdir / "files.txt")
        # The launcher keeps the debug heap off by default ...
        assert "debug-probe heap flags 0x0\n" in text(logdir / "stdout.txt"), text(logdir / "stdout.txt")
        # ... and a caller who sets _NO_DEBUG_HEAP=0 gets it back (control: the loader's three heap bits).
        out, logdir = run(probe, work, "debug-heap", {"_NO_DEBUG_HEAP": "0"})
        assert "process exited with 0x00000000" in out, out
        assert "debug-probe heap flags 0x70\n" in text(logdir / "stdout.txt"), text(logdir / "stdout.txt")

        # X2W_STOP_AT on the second site: the first is still stepped over, the run stops at the second.
        out, logdir = run(probe, work, "stop", {"X2W_STOP_AT": f"{sites[1]:x}"})
        assert f"stopped at breakpoint (0x80000003) at {image}+{sites[1]:#x}" in out and "first chance" in out, out
        events = text(logdir / "events.txt")
        assert f"breakpoint at {image}+{sites[0]:#x}" in events, events
        stacks = text(logdir / "threads_stop.txt")
        assert image in stacks and "rip=" in stacks, stacks
        # Control: an address that is not a breakpoint site stops nothing.
        out, logdir = run(probe, work, "stop-control", {"X2W_STOP_AT": f"{sites[1] + 0x1000:x}"})
        assert "process exited with 0x00000000" in out and not (logdir / "threads_stop.txt").exists(), out
        # Control: without --continue-asserts the first site stops the run as before, with or without the switch.
        out, _ = run(probe, work, "stop-first", {"X2W_STOP_AT": f"{sites[1]:x}"}, options=())
        assert f"stopped at breakpoint (0x80000003) at {image}+{sites[0]:#x}" in out, out

        # X2W_DUMP_AT: a dump one second in, while the probe is still alive.
        out, logdir = run(probe, work, "dump", {"X2W_DUMP_AT": "1"})
        assert "process exited with 0x00000000" in out, out
        assert "rip=" in text(logdir / "threads_1.txt"), text(logdir / "threads_1.txt")
        assert not (work / "baseline" / "threads_1.txt").exists()

        # X2W_VALLOC_MIN: the 256 MiB reservation is logged with a caller in the probe; a higher
        # threshold logs nothing.
        out, logdir = run(probe, work, "valloc", {"X2W_VALLOC_MIN": "10000000"})
        lines = [line for line in text(logdir / "files.txt").splitlines() if " VALLOC " in line]
        ours = [line for line in lines if "size=0x10000000" in line and "type=0x2000" in line]
        assert len(ours) == 1 and "requested=0x0" in ours[0] and image in ours[0].split("callers:")[1], lines
        out, logdir = run(probe, work, "valloc-control", {"X2W_VALLOC_MIN": "20000000"})
        assert "process exited with 0x00000000" in out and " VALLOC " not in text(logdir / "files.txt"), out
        # An empty value is the same as unset.
        out, logdir = run(probe, work, "valloc-empty", {"X2W_VALLOC_MIN": ""})
        assert "process exited with 0x00000000" in out and " VALLOC " not in text(logdir / "files.txt"), out

    print("startup_debug: PASS (stop at a chosen assert, timed thread dump, large-allocation log, debug heap off; 6 controls)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
