"""Check the clock-driven sampling profiler of tools/startup/debug_launch.py (X2W_PROFILE).

tests/startup_profile_probe.cpp is the only input: a thread named "spin" burns CPU in spin_work, one
named "waiter" blocks in wait_here, one named "faults" takes a caught access violation every
millisecond (a stream of debug events), for about 3 s. The profile must show the spinning thread busy
in spin_work, the waiter idle on its wait call at wait_here, and the process frozen only briefly per
sample. Controls: no profile without the switch, a malformed value is reported and ignored, and a
repeat interval writes a second batch.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
DEBUG = REPO / "tools/startup/debug_launch.py"
ROW = re.compile(r"^\s*(\d+)\s+([\d.]+)\s+\d+\s+\d+\s+(\d+)/\s*(\d+)/\s*(\d+)\s+(\S+) /", re.M)


def run(probe, work, name, variables):
    logdir = work / name
    env = {k: v for k, v in os.environ.items() if not k.startswith("X2W_")}
    env.update(variables)
    command = [sys.executable, "-B", str(DEBUG), str(probe), str(work), str(logdir), "--timeout", "30", "--console", "0"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=100, env=env)
    return result.stdout + result.stderr, logdir


def main(probe):
    probe = Path(probe).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        out, logdir = run(probe, work, "profile", {"X2W_PROFILE": "0.5,40,25"})
        assert "process exited with 0x00000000" in out, out
        report = (logdir / "profile_0.5.txt").read_text(encoding="utf-8")
        rows = {m.group(6): m for m in ROW.finditer(report)}
        assert "spin" in rows and "waiter" in rows and "faults" in rows, report
        spin, waiter = rows["spin"], rows["waiter"]
        assert float(spin.group(2)) >= 50.0, report      # a core, less the time the debugger froze it
        assert float(waiter.group(2)) < 5.0, report
        assert int(waiter.group(5)) >= 35, report         # waiting in nearly every sample
        block = report.split(f"thread {spin.group(1)} (spin")[1].split("\nthread ")[0]
        assert "spin_work" in block.split("\n  inclusive:")[0], block    # the leaf is the spin loop
        waits = [line for line in report.splitlines() if line.startswith(f"thread {waiter.group(1)}: ")]
        # wait_here's call is a tail jump in an optimised build, so the site is wait_here or its caller.
        assert waits and "NtWaitForSingleObject @ " in waits[0] and re.search(r"wait_(here|thread)", waits[0]), waits
        frozen = float(re.search(r"frozen per sample: median ([\d.]+) ms", report).group(1))
        assert frozen < 50.0, report
        assert "profile: 40 samples" in (logdir / "events.txt").read_text(encoding="utf-8")
        print(f"spin {spin.group(2)}% cpu, waiter {waiter.group(2)}% ({waiter.group(5)} waiting samples), "
              f"frozen median {frozen} ms per sample")

        # Repeat: a second batch one second later.
        out, logdir = run(probe, work, "repeat", {"X2W_PROFILE": "0.3,5,20,1"})
        assert "process exited with 0x00000000" in out, out
        assert (logdir / "profile_0.3.txt").exists() and (logdir / "profile_1.3.txt").exists(), os.listdir(logdir)
        # Controls: no switch, no profile; a malformed value is reported and ignored.
        out, logdir = run(probe, work, "off", {})
        assert "process exited with 0x00000000" in out and not list(logdir.glob("profile_*")), out
        out, logdir = run(probe, work, "malformed", {"X2W_PROFILE": "1,x"})
        assert "process exited with 0x00000000" in out and "ignored" in out and not list(logdir.glob("profile_*")), out
    print("startup_profile: PASS (busy thread and its loop, waiter and its wait site, short freeze; 3 controls)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
