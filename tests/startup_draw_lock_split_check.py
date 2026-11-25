"""LOCAL stall: two threads issue translated draws with a buffer srv at the same time (X2W_DRAW_LOCK_SPLIT).

The consumer runs a startup_frame_copy frame on X2W_TEST_THREADS=2 threads, X2W_TEST_REPEAT times each: per
thread its own constant buffer, target, list, queue and fence; one shared device and pipeline. Two frames:
- "changed": red, rewritten to blue between the two draws of one list, red again next frame: every frame copy
  misses the cache (compare fails or nothing cached) and is copied fresh.
- "same": red twice: after each thread's first draw the copies are cache hits.
X2W_DRAW_LOCK_TIMES=1 in both modes; the consumer writes the lock-times report ("fixture") at the end.

- split (X2W_DRAW_LOCK_SPLIT=1): exact pixels per thread; site ids read from d3d12_draw.inl: the tail lock_draw
  and frame_copy_unlocked's lookup are taken once per draw; its commit (lookup id + 1) once per miss: every draw
  for "changed", 2 to 49 of 100 for "same" (at least each thread's first; a hit whose ring chunk was recycled
  meanwhile is a miss too).
- locked (control, switch off): the same pixels; no split-only site appears in the report.

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
    """Site ids of draw_translated read from the source lines: entry, tail, frame_copy_unlocked lookup and commit
    (lookup + 1), and every site that exists only under the switch."""
    lines = SOURCE.read_text(encoding="utf-8", errors="replace").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("bool draw_translated("))
    entry = tail = lookup = None
    split_only = set()
    for i in range(start, len(lines)):
        line, number = lines[i], 100000 + i + 1
        if line.startswith("}"):
            break
        if entry is None and "lock_draw(100000 + __LINE__)" in line:
            entry = number
        if "SplitRelock relock(split, 100000 + __LINE__)" in line:
            split_only.add(number)
        if "have = frame_copy_unlocked(" in line:
            lookup = number
            split_only.update((number, number + 1))
        if "if (split) lock_draw(100000 + __LINE__)" in line:
            tail = number
            split_only.add(number)
    if None in (entry, tail, lookup):
        raise RuntimeError(f"draw_translated site ids not found in {SOURCE}: entry {entry} tail {tail} lookup {lookup}")
    return entry, tail, lookup, lookup + 1, split_only


def report_holds(log):
    lines = [line for line in log.splitlines() if "x2w-draw lock times (fixture" in line]
    if len(lines) != 1:
        raise AssertionError(f"{len(lines)} fixture lock-times lines")
    return {int(m.group(1)): int(m.group(2)) for m in re.finditer(r" (\d+) \S+ (\d+)/[\d.]+/[\d.]+, \d+/", lines[0])}, lines[0]


def run(directory, consumer, backend, mode, frame):
    """frame "changed": every draw misses the frame-copy cache (red, then blue, then red again on the next frame);
    "same": red twice, so after each thread's first draw the copies are cache hits."""
    entry, tail, lookup, commit, split_only = sites()
    folder = directory / frame
    folder.mkdir()
    env, args, output = prepare_draw_cache(folder, consumer, HLSL, {"X2W_TEST_FRAME_COPY": frame}, "srv 0 buf table 0 0")
    for name in ("X2W_DRAW_LOCK_SPLIT", "X2W_DRAW_ASYNC", "X2W_DRAW_SRV_IN_PLACE"):
        env.pop(name, None)
    env.update(X2W_TEST_THREADS=str(THREADS), X2W_TEST_REPEAT=str(REPEAT), X2W_DRAW_LOCK_TIMES="1")
    if mode == "split":
        env["X2W_DRAW_LOCK_SPLIT"] = "1"
    result = subprocess.run([*args, backend, output], env=env, capture_output=True, text=True, timeout=120)
    print(result.stdout, end="")
    if backend == "hardware" and result.returncode == 77:
        print("No D3D12 feature-level 12 hardware adapter")
        return None
    if result.returncode:
        raise RuntimeError(f"consumer failed on {backend}: {result.returncode}\n{result.stderr[-5000:]}")
    holds, line = report_holds(result.stderr)
    print(line)
    draws = THREADS * REPEAT * 2
    wanted = (RED * 4 + BLUE * 4) * 4 if frame == "changed" else RED * 32
    failures = []
    for t in range(THREADS):
        pixels = Path(f"{output}.t{t}").read_bytes()
        if pixels != wanted:
            failures.append(f"{frame} thread {t}: {pixels.hex()} want {wanted.hex()}")
    if holds.get(entry) != draws:
        failures.append(f"{frame} entry site {entry}: {holds.get(entry)} holds, want {draws}")
    seen = sorted(site for site in split_only if site in holds)
    if mode == "split":
        if holds.get(tail) != draws:
            failures.append(f"{frame} tail site {tail}: {holds.get(tail)} holds, want {draws}")
        if holds.get(lookup) != draws:
            failures.append(f"{frame} frame_copy lookup site {lookup}: {holds.get(lookup)} holds, want {draws}")
        misses = holds.get(commit, 0)
        if frame == "changed" and misses != draws:
            failures.append(f"changed: {misses} frame_copy commits (misses), want {draws}")
        if frame == "same" and not THREADS <= misses < draws // 2:
            failures.append(f"same: {misses} frame_copy commits (misses), want {THREADS} to {draws // 2 - 1} (mostly hits)")
    elif seen:
        failures.append(f"{frame}: split-only sites {seen} without the switch")
    if any("x2w-d3d12-debug " in l and (" sev=0 " in l or " sev=1 " in l) for l in result.stderr.splitlines()):
        failures.append(f"{frame}: debug-layer error or corruption")
    print(f"{backend} {mode} {frame}: {THREADS} threads x {REPEAT} frames, entry {entry}={holds.get(entry)}, tail {tail}={holds.get(tail)}, "
          f"frame_copy lookup {lookup}={holds.get(lookup)} commit {commit}={holds.get(commit)}, split-only sites in the report {seen}")
    return failures


def check(directory, consumer, backend, mode):
    if mode not in ("split", "locked"):
        return 2
    failures = []
    for frame in ("changed", "same"):
        result = run(directory, consumer, backend, mode, frame)
        if result is None:
            return 77
        failures += result
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
