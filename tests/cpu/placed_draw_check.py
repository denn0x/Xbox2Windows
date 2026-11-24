"""Run one PE's placed-buffer draw through the Windows loader, pe_map and the AOT program."""
from pathlib import Path
import subprocess
import sys
import tempfile
from mapped_exe_chain_check import mapped_child
from shim_compute_check import check_lifted_image
from exe_entry_check import check
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from startup_address_srv_check import prepare_draw_cache

SHADER = """ByteAddressBuffer vertices : register(t0);
float4 main(float4 p : SV_Position) : SV_Target {
    return float4(float(vertices.Load(0)) / 255.0, 0, 0, 1);
}
"""

def compare(directory, adapter, consumer, mapper, exe, lifted, provider, io):
    check_lifted_image(exe, lifted)
    env, _, _ = prepare_draw_cache(directory, consumer, SHADER, {}, "srv 0 buf vb 0")
    env["X2W_PLACED_ADAPTER"] = adapter
    mapped = [sys.executable, "-B", __file__, "--map", mapper, exe, provider, io]
    trace, pixels = directory / "calls.txt", directory / "pixels.bin"
    device = None
    for mode, high, width, red in (("low", False, 4, 41), ("high", True, 4, 135), ("cross-end", True, 8, 0),
                                   ("reject-submit", True, 4, None), ("partial-open", True, 4, None)):
        env["X2W_PLACED_CASE"] = mode
        calls = ["open bit=40", "resource index=0 bytes=260", "resource index=1 bytes=260",
                 f"bind address_delta={(1<<40 if high else 0) + 256} bytes={width} stride=4",
                 "draw vertices=3 instances=1", "submit lists=1 fence=1", "close placed_resources=2"]
        if mode == "reject-submit":
            calls[-2] = "submit lists=0 fence=1"
        if mode == "partial-open":
            calls = [*calls[:2], "resource index=1 bytes=0", "close placed_resources=1"]
        status = 42 if red is not None else 20
        for name, command in (("native", [exe]), ("pe_map", mapped), ("AOT", [lifted])):
            pixels.unlink(missing_ok=True)
            trace.unlink(missing_ok=True)
            child = subprocess.run(command, env=env, cwd=directory, capture_output=True, timeout=30)
            if child.returncode == 77 and name == "native" and mode == "low":
                print(f"{adapter} missing placed-buffer capability")
                return 77
            if child.returncode != status:
                raise AssertionError(f"{mode} {name} exit={child.returncode}\n"
                                     f"{child.stdout.decode(errors='replace')}\n{child.stderr.decode(errors='replace')[-4000:]}")
            actual_device, *observed = trace.read_text().splitlines()
            if device is None:
                device = actual_device
                print(f"{adapter} draw and observation {device}")
            check(f"{mode} {name} device", actual_device, device)
            check(f"{mode} {name} software adapter", actual_device.endswith("software=1"), adapter == "warp")
            check(f"{mode} {name} imported calls", observed, calls)
            if red is None:
                check(f"{mode} {name} unpublished pixels", pixels.exists(), False)
            else:
                check(f"{mode} {name} readback", pixels.read_bytes(), bytes([red, 0, 0, 255])*32)
            print(f"{mode} {name}: {len(calls)} ordered events, red={red}, {calls[-1]}, exit={status}")
    return 0

if __name__ == "__main__":
    if sys.argv[1] == "--map":
        sys.exit(mapped_child(*map(Path, sys.argv[2:])))
    with tempfile.TemporaryDirectory() as temporary:
        sys.exit(compare(Path(temporary), sys.argv[1], *map(Path, sys.argv[2:])))
