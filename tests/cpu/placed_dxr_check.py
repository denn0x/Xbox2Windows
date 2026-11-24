"""The same linked EXE writes two guest build records under Windows, pe_map and AOT."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from exe_entry_check import check
from mapped_exe_chain_check import mapped_child
from shim_compute_check import check_lifted_image


def check_ray_control(actual, expected):
    """Every word but the hit distance must match exactly. As in check_dxr_rays, the distance (word 3)
    must be within 1e-5 of 1, because hardware intersection may round it (the RX 6600 returns 0x3f7fffff)."""
    if len(actual) != len(expected):
        check("direct Windows ray control", actual, expected)
    distance = struct.unpack_from("<f", actual, 12)[0]
    check("direct Windows ray control", actual[:12] + actual[16:], expected[:12] + expected[16:])
    if not abs(distance - 1.0) < 1e-5:
        raise AssertionError(f"direct Windows ray control: hit distance {distance!r} "
                             f"(0x{struct.unpack_from('<I', actual, 12)[0]:08x}) is not within 1e-5 of 1")


def compare(root, adapter, mapper, exe, lifted, provider, io):
    check_lifted_image(exe, lifted)
    mapped = [sys.executable, "-B", __file__, "--map", mapper, exe, provider, io]
    expected = struct.pack("<12I", 1, 7, 0, 0x3f800000, 0, 0, 0, 0, 0, 0, 0, 0)
    actual_device = None
    for case, scratch in (("valid", "3+768"), ("short", "4+768"), ("unmapped", "unmapped")):
        valid = case == "valid"
        env = dict(os.environ, X2W_DXR_ADAPTER=adapter, X2W_DXR_CASE=case)
        for runner, cmd in (("native", [exe]), ("mapped", mapped), ("AOT", [lifted])):
            run = root / f"{case}-{runner}"
            run.mkdir()
            child = subprocess.run(cmd, cwd=run, env=env, capture_output=True, timeout=40)
            if child.returncode == 77 and valid and runner == "native":
                print(f"{adapter} skips DXR pipeline: {child.stdout.decode(errors='replace')}")
                return 77
            status = 42 if valid else 22
            if child.returncode != status:
                raise AssertionError(f"{case} {runner}: expected {status}, got {child.returncode}\n"
                                     f"{child.stderr.decode(errors='replace')}\n{child.stdout.decode(errors='replace')}")
            device, *events = (run / "calls.txt").read_text(encoding="utf-8").splitlines()
            if actual_device is None:
                actual_device = device
                print(device)
            check("same DXR adapter", device, actual_device)
            check("software selection", device.endswith("software=1"), adapter == "warp")
            check("first rejected index or submitted batch and lifetime", events, [
                "request 0 dest_offset=512 scratch=2+768",
                f"request 1 dest_offset=512 scratch={scratch}",
                ("accepted=2 placed_records=2 placed_submissions=1 direct_records=2 direct_submissions=1"
                 if valid else "first_rejected=1 error=E_INVALIDARG records=0 submissions=0"),
                "released=5",
            ])
            placed, direct = run / "placed.bin", run / "direct.bin"
            if valid:
                check_ray_control(direct.read_bytes(), expected)
                check("guest-selected addresses", placed.read_bytes(), direct.read_bytes())
            else:
                check("unpublished on reject", (placed.exists(), direct.exists()), (False, False))
            print(f"{case} {runner}: exit={status}, {events[2]}, {events[3]}")
    return 0


if __name__ == "__main__":
    if sys.argv[1] == "--map":
        sys.exit(mapped_child(*map(Path, sys.argv[2:])))
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(compare(Path(temp), sys.argv[1], *map(Path, sys.argv[2:])))
