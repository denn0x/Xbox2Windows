"""LOCAL stall: two threads issue translated draws with a buffer srv at the same time (X2W_DRAW_LOCK_SPLIT).

The consumer runs startup_frame_copy's "changed" frame on X2W_TEST_THREADS=2 threads, X2W_TEST_REPEAT times
each: per thread its own constant buffer (red, then rewritten to blue between the two draws of one list), its
own target, list, queue and fence, one shared device and pipeline. X2W_DRAW_LOCK_TIMES=1 in both modes; the
consumer writes the lock-times report ("fixture") at the end.

- split (X2W_DRAW_LOCK_SPLIT=1): every thread's last frame is left red / right blue; the report shows the
  relock sites of draw_translated (ids read from d3d12_draw.inl: the SplitRelock lines and the tail lock_draw),
  the tail taken once per draw and the frame_copy relock once per draw (one buffer srv each).
- locked (control, switch off): the same pixels; no relock site appears in the report.

Both: the entry site of draw_translated is taken once per draw (2 per frame).
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from startup_address_srv_check import prepare_draw_cache

HLSL = (
    "ByteAddressBuffer constants : register(t0);\n"
    "float4 main(float4 p : SV_Position) : SV_Target {\n"
    "  return asfloat(constants.Load4(0));\n"
    "}\n"
)
RED = bytes((255, 0, 0, 255))
BLUE = bytes((0, 0, 255, 255))
THREADS, REPEAT = 2, 25
SOURCE = Path(__file__).resolve().parent.parent / "src" / "startup_stubs" / "d3d12_draw.inl"


def sites():
    """(entry, tail, frame_copy relock, every relock) site ids of draw_translated, from the source lines."""
    lines = SOURCE.read_text(encoding="utf-8", errors="replace").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("bool draw_translated("))
    entry = tail = copy = None
    relocks = set()
    for i in range(start, len(lines)):
        line, number = lines[i], 100000 + i + 1
        if line.startswith("}"):
            break
        if entry is None and "lock_draw(100000 + __LINE__)" in line:
            entry = number
        if "SplitRelock relock(split, 100000 + __LINE__)" in line:
            relocks.add(number)
            if copy is None and any("have = frame_copy(" in l for l in lines[i + 1:i + 3]):
                copy = number
        if "if (split) lock_draw(100000 + __LINE__)" in line:
            tail = number
            relocks.add(number)
    if None in (entry, tail, copy):
        raise RuntimeError(f"draw_translated site ids not found in {SOURCE}: entry {entry} tail {tail} copy {copy}")
    return entry, tail, copy, relocks


def report_holds(log):
    lines = [line for line in log.splitlines() if "x2w-draw lock times (fixture" in line]
    if len(lines) != 1:
        raise AssertionError(f"{len(lines)} fixture lock-times lines")
    return {int(m.group(1)): int(m.group(2)) for m in re.finditer(r" (\d+) \S+ (\d+)/[\d.]+/[\d.]+, \d+/", lines[0])}, lines[0]


def check(directory, consumer, backend, mode):
    if mode not in ("split", "locked"):
        return 2
    entry, tail, copy, relocks = sites()
    env, args, output = prepare_draw_cache(directory, consumer, HLSL, {"X2W_TEST_FRAME_COPY": "changed"},
                                           "srv 0 buf table 0 0")
    for name in ("X2W_DRAW_LOCK_SPLIT", "X2W_DRAW_ASYNC", "X2W_DRAW_SRV_IN_PLACE"):
        env.pop(name, None)
    env.update(X2W_TEST_THREADS=str(THREADS), X2W_TEST_REPEAT=str(REPEAT), X2W_DRAW_LOCK_TIMES="1")
    if mode == "split":
        env["X2W_DRAW_LOCK_SPLIT"] = "1"
    result = subprocess.run([*args, backend, output], env=env, capture_output=True, text=True, timeout=120)
    print(result.stdout, end="")
    if backend == "hardware" and result.returncode == 77:
        print("No D3D12 feature-level 12 hardware adapter")
        return 77
    if result.returncode:
        raise RuntimeError(f"consumer failed on {backend}: {result.returncode}\n{result.stderr[-5000:]}")
    holds, line = report_holds(result.stderr)
    print(line)
    draws = THREADS * REPEAT * 2
    wanted = (RED * 4 + BLUE * 4) * 4
    failures = []
    for t in range(THREADS):
        pixels = Path(f"{output}.t{t}").read_bytes()
        if pixels != wanted:
            failures.append(f"thread {t}: {pixels.hex()} want {wanted.hex()}")
    if holds.get(entry) != draws:
        failures.append(f"entry site {entry}: {holds.get(entry)} holds, want {draws}")
    seen = sorted(site for site in relocks if site in holds)
    if mode == "split":
        if holds.get(tail) != draws:
            failures.append(f"tail site {tail}: {holds.get(tail)} holds, want {draws}")
        if holds.get(copy) != draws:
            failures.append(f"frame_copy relock site {copy}: {holds.get(copy)} holds, want {draws}")
    elif seen:
        failures.append(f"relock sites {seen} without the switch")
    if any("x2w-d3d12-debug " in l and (" sev=0 " in l or " sev=1 " in l) for l in result.stderr.splitlines()):
        failures.append("debug-layer error or corruption")
    print(f"{backend} {mode}: {THREADS} threads x {REPEAT} frames, entry {entry}={holds.get(entry)}, tail {tail}={holds.get(tail)}, "
          f"frame_copy {copy}={holds.get(copy)}, relock sites in the report {seen}")
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
