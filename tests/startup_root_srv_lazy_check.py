"""LOCAL stall: a root SRV (list slot 44) read by a translated pixel shader, with and without X2W_ROOT_SRV_LAZY.

The translated shader reads root parameter 0 through its root pointer (root_address), not through the desktop
root SRV that slot 44 uploads for real desktop pipelines. Both runs draw the colour the title bound (yellow over
the left half). Without the switch (control) slot 44 copies the range at record time ("x2w-render root SRV 0 at
title"); with X2W_ROOT_SRV_LAZY=1 it copies nothing ("X2W_ROOT_SRV_LAZY=1" line, no copy line).
Not covered here: the copy at a real-pipeline draw (bind_lazy_root_srvs); no fixture draws a real pipeline
with a root SRV.
"""
from pathlib import Path
import sys
import tempfile

from startup_address_srv_check import draw_texture
from startup_root_cbv_check import CLEAR, HLSL


def check(directory, consumer, backend):
    yellow = bytes((255, 255, 0, 255))
    expected = (yellow * 4 + CLEAR * 4) * 4
    for lazy in (False, True):
        folder = directory / ("lazy" if lazy else "copy")
        folder.mkdir()
        log_path = folder / "trap.log"
        env = {"X2W_TEST_ROOT_CBV": "root_srv", "X2W_TRAP_LOG": str(log_path)}
        if lazy:
            env["X2W_ROOT_SRV_LAZY"] = "1"
        pixels = draw_texture(folder, consumer, backend, HLSL, env, binding="srv 0 buf root 0")
        if pixels is None:
            return 77
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        copied = "x2w-render root SRV 0 at title" in log
        deferred = "X2W_ROOT_SRV_LAZY=1: root SRVs are copied at a real-pipeline draw only" in log
        print(f"{backend} lazy={int(lazy)}: pixels {'exact' if pixels == expected else 'DIFFER'}, record-time copy line {int(copied)}, "
              f"deferred line {int(deferred)}")
        if pixels != expected:
            raise AssertionError(f"lazy={lazy}: actual {pixels.hex()} expected {expected.hex()}")
        if copied == lazy or deferred != lazy:
            raise AssertionError(f"lazy={lazy}: copy line {copied}, deferred line {deferred}")
    print("PASS")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(check(Path(temporary), *sys.argv[1:]))
