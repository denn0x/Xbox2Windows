"""Compare the linked shim EXE, the mapped image and the AOT process against one oracle."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys

import pefile

from shim_startup_check import EXPECTED, SHIM_IMPORTS, check


EVENT = struct.Struct("<4Q")
ATTRIBUTES = 0xA8004000


def verify(case, size, result):
    check("companion startup", result.stdout[:len(EXPECTED.output)], EXPECTED.output)
    trace = result.stdout[len(EXPECTED.output):]
    if case == "fail":
        check("failed allocation and no subsequent calls", trace, EVENT.pack(1, size, ATTRIBUTES, 0))
        check("failed allocation exit", result.returncode, 17)
        return b""

    rejected = case == "adapter_error"
    check("EXE exit", result.returncode, 19 if rejected else 42)
    payload_bytes = 0 if rejected else 16
    check("allocation, dispatch, payload and release length", len(trace), 3 * EVENT.size + payload_bytes)
    kind, requested, attributes, address = EVENT.unpack_from(trace)
    check("allocation import", (kind, requested, attributes), (1, size, ATTRIBUTES))
    check("aligned non-null allocation", bool(address) and address % 256 == 0, True)
    seed = size ^ 0x12345678
    check("dispatch import", EVENT.unpack_from(trace, EVENT.size), (2, address, seed, 4))
    payload = trace[2 * EVENT.size:2 * EVENT.size + payload_bytes]
    expected = struct.pack("<4I", *(((i + 1) * 33) ^ (seed + i * 7) for i in range(4)))
    check("published GPU bytes", payload, b"" if rejected else expected)
    check("matching address freed once", EVENT.unpack_from(trace, 2 * EVENT.size + payload_bytes),
          (3, address, ATTRIBUTES, 1))
    return payload


def check_lifted_image(exe, lifted):
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    with pefile.PE(str(lifted), fast_load=True) as pe:
        pe.parse_data_directories(directories=[0])
        stamp, = [s.address for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name == b"aot_source_sha256"]
        check("AOT input identity", pe.get_string_at_rva(stamp).decode("ascii"), digest)
    print(f"AOT input {exe.name} sha256={digest}")


def compare(mapper, exe, lifted, companion, io, xmem, d3d12):
    check_lifted_image(exe, lifted)
    map_command = [sys.executable, "-B", Path(__file__).with_name("shim_startup_check.py"), "--map",
                   mapper, exe, companion, io, xmem, d3d12, ""]
    markers = sorted(f"bound {dll}!{symbol}" for dll, symbols in SHIM_IMPORTS.items() for symbol in symbols)
    for case, size in (("small", 4096), ("large", 8192), ("fail", (1 << 64) - 1), ("adapter_error", 4096)):
        env = dict(os.environ, X2W_SHIM_COMPUTE_CASE=case)
        if case == "adapter_error":
            env.update(X2W_SHIM_COMPUTE_CASE="small", X2W_SHIM_COMPUTE_ADAPTER="invalid")
        for runner, command in (("native", [exe]), ("pe_map", map_command), ("AOT", [lifted])):
            child = subprocess.run(command, env=env, capture_output=True, timeout=30)
            lines = child.stderr.decode(errors="replace").splitlines()
            if (runner == "native" and case == "small" and env.get("X2W_SHIM_COMPUTE_ADAPTER") == "hardware"
                    and child.returncode == 77 and lines == ["no D3D12 physical adapter"]):
                print(lines[0])
                sys.exit(77)
            if case == "adapter_error":
                diagnostic = "Select compute adapter failed (HRESULT 0x80070057)"
                check("one provider rejection", lines.count(diagnostic), 1)
                lines.remove(diagnostic)
            if runner == "pe_map":
                check("mapper returned", lines[-1:], ["mapped host returned"])
                check("explicit shim bindings", sorted(lines[:-1]), markers)
            else:
                check("runner diagnostics", lines, [])
            payload = verify(case, size, child)
            print(f"{case} {runner}: size={size} attr={ATTRIBUTES:#x} "
                  f"output={payload.hex() or '(empty)'} compute releases={int(case != 'fail')} exit={child.returncode}")


if __name__ == "__main__":
    compare(*map(Path, sys.argv[1:]))
