"""LOCAL stall: a translated draw issued while its desktop pipeline compile is held back (X2W_DRAW_ASYNC).

The red pixel shader of startup_stream_draw_check is translated into the cache; the consumer then draws twice
(X2W_TEST_HOLD_RELEASE) while the stand-in holds every translated pipeline compile until a named event is set
(X2W_CX_TEST_HOLD_EVENT).

- async (X2W_DRAW_ASYNC=1, the event set after the first draw): the first draw is skipped (pixels <output>.first
  all clear, one "draw skipped and the build queued" line for the key); the second draw, 1.5 s after the release,
  is red; the ready line counts 1 skipped draw.
- sync (control, switch off, the event set by a thread 300 ms after the first draw starts): the recording thread
  waits, so both draws are red; neither line.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from startup_stream_check import gfx2hlsl
from startup_stream_draw_check import pixel, vertex


def check(directory, consumer, backend, mode):
    if mode not in ("async", "sync"):
        return 2
    cache = directory / "cache"
    cache.mkdir()
    vs, ps, root, output = (directory / name for name in ("vs", "ps", "root", "pixels"))
    vs.write_bytes(vertex())
    ps.write_bytes(pixel(False))
    args = [consumer, vs, ps, root]
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_LIST_NOOP="1", X2W_D3D12_RENDER="1",
               X2W_SHADER_CACHE=str(cache))
    env.setdefault("X2W_D3D12_DEBUG", "0")
    for name in ("X2W_DRAW_ASYNC", "X2W_CX_ASYNC", "X2W_DRAW_PSO_LOCKED"):
        env.pop(name, None)
    subprocess.run(args, env=env, check=True)
    subprocess.run([sys.executable, "-B", gfx2hlsl.__file__, "--pending", cache], env=env, check=True)
    key = gfx2hlsl.pipeline_key(vs.read_bytes(), ps.read_bytes(), root.read_bytes())

    run_env = dict(env, X2W_CX_TEST_HOLD_EVENT=f"Local\\x2w_draw_hold_{os.getpid()}",
                   X2W_TEST_HOLD_RELEASE="first" if mode == "async" else "300")
    if mode == "async":
        run_env["X2W_DRAW_ASYNC"] = "1"
    result = subprocess.run(args + [backend, output], env=run_env, capture_output=True, text=True, timeout=60)
    print(result.stdout, end="")
    log = result.stderr
    for line in log.splitlines():
        if "X2W_DRAW_ASYNC" in line or "X2W_CX_TEST_HOLD_EVENT" in line or "x2w-d3d12-debug " in line or "-> drawn;" in line:
            print(line)
    if backend == "hardware" and result.returncode == 77:
        print("No D3D12 feature-level 12 hardware adapter")
        return 77
    if result.returncode:
        raise RuntimeError(f"consumer failed on {backend}: {result.returncode}\n{log}")
    red, clear = bytes((255, 0, 0, 255)), bytes((0, 0, 255, 255))
    drawn = (red * 4 + clear * 4) * 4
    first, second = Path(str(output) + ".first").read_bytes(), output.read_bytes()
    skip_line = f"x2w-draw {key:016x}: desktop pipeline not built yet, draw skipped and the build queued (X2W_DRAW_ASYNC)"
    ready_line = f"x2w-draw {key:016x} ready (X2W_DRAW_ASYNC); 1 draws skipped while compiling"
    held = f"x2w-draw {key:016x}: load held by X2W_CX_TEST_HOLD_EVENT (released)"
    draws = log.count("-> drawn;")
    want_first = clear * 32 if mode == "async" else drawn
    failures = []
    if first != want_first:
        failures.append(f"first draw: {first.hex()} want {want_first.hex()}")
    if second != drawn:
        failures.append(f"second draw: {second.hex()} want {drawn.hex()}")
    if draws != (1 if mode == "async" else 2):
        failures.append(f"{draws} '-> drawn;' lines")
    if held not in log:
        failures.append("the compile was not held and released")
    if mode == "async" and (log.count(skip_line) != 1 or ready_line not in log):
        failures.append("skip / ready lines missing")
    if mode == "sync" and ("X2W_DRAW_ASYNC" in log):
        failures.append("X2W_DRAW_ASYNC lines without the switch")
    if any("x2w-d3d12-debug " in line and (" sev=0 " in line or " sev=1 " in line) for line in log.splitlines()):
        failures.append("debug-layer error or corruption")
    print(f"{backend} {mode}: first draw {'clear' if first == clear * 32 else 'red' if first == drawn else 'other'}, "
          f"second {'red' if second == drawn else 'other'}, {draws} drawn lines")
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
