"""Start the reserve+commit pool EXE under the Windows loader and pe_map; compare the exit codes.

tests/cpu/xmem_pool_commit.asm imports XMemVirtualAlloc by name from xmem.dll, VirtualQuery,
VirtualFree, SetLastError and GetLastError from kernel32.dll, and finish_exe (exe_startup.asm) from
cpu_exe_io.dll. Its body returns 0x870 when every step passes, or 0xF00 + the first failing step. The
reserve_only, misaligned, dirty, accepted_refusal, host_fit and skip_release variants are negative controls with
known failing steps. A provider without XMemVirtualAlloc must stop the mapping before the entry point.
"""
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from exe_process import launch
from native_map_check import Mapper, STATUS, kernel

PASS = 0x870
EXPECTED = {"original": PASS, "reserve_only": 0xF02, "misaligned": 0xF02, "dirty": 0xF03, "accepted_refusal": 0xF13,
            "host_fit": 0xF1C, "skip_release": 0xF1E}
EXPORT = "XMemVirtualAlloc"
XMEM, IO = "xmem.dll", "cpu_exe_io.dll"
REQUESTS = sorted([[XMEM, EXPORT], ["kernel32.dll", "VirtualQuery"], ["kernel32.dll", "VirtualFree"],
                   ["kernel32.dll", "SetLastError"], ["kernel32.dll", "GetLastError"], [IO, "finish_exe"]])


def stage(folder, image, xmem, io):
    folder = Path(folder)
    for source, name in ((image, Path(image).name), (xmem, XMEM), (io, IO)):
        shutil.copyfile(source, folder / name)
    return folder / Path(image).name


def native(image):
    row = launch([image], b"", timeout=30)
    if not row["process_signaled"]:
        raise AssertionError(f"{image.name}: process did not end: {row}")
    if row["stdout"]["bytes"] or row["stderr"]["bytes"]:
        raise AssertionError(f"{image.name}: unexpected output {row['stdout']['bytes']!r} {row['stderr']['bytes']!r}")
    return row["code"]


def child(mapper_path, image, xmem, io, omit=""):
    """Map and start the EXE in this (child) process; print the observation as JSON."""
    mapper = Mapper(mapper_path)
    run_entry = ctypes.CDLL(str(mapper_path)).pe_map_run
    run_entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    run_entry.restype = ctypes.c_int
    module = ctypes.CDLL(str(xmem))
    # A provider without the export resolves nothing from xmem.dll.
    bindings = {XMEM: {} if omit else module, IO.casefold(): ctypes.CDLL(str(io)), "kernel32.dll": kernel}
    base, status, detail, requests = mapper(Path(image), bindings)
    if not base:
        print(json.dumps({"status": status, "detail": detail, "requests": requests}))
        return
    code = ctypes.c_uint32(0xdeadbeef)
    started = run_entry(base, ctypes.byref(code))
    mapper.release(base)
    print(json.dumps({"status": status, "started": started, "exit": code.value, "requests": requests}))


def mapped(mapper_path, image, xmem, io, omit=""):
    result = subprocess.run([sys.executable, "-B", __file__, "--child", str(mapper_path), str(image),
                             str(xmem), str(io), omit], capture_output=True, text=True, timeout=60)
    if result.returncode or result.stderr:
        raise AssertionError(f"mapped {Path(image).name}: child exited {result.returncode:#x}: {result.stderr}")
    return json.loads(result.stdout)


def main(mapper_path, original, reserve_only, misaligned, dirty, accepted_refusal, host_fit, skip_release,
         xmem, io):
    images = {"original": original, "reserve_only": reserve_only, "misaligned": misaligned, "dirty": dirty,
              "accepted_refusal": accepted_refusal, "host_fit": host_fit, "skip_release": skip_release}
    with TemporaryDirectory() as temp:
        for name, image in images.items():
            staged = stage(temp, image, xmem, io)
            expected = EXPECTED[name]
            launched = native(staged)
            result = mapped(mapper_path, staged, Path(temp) / XMEM, Path(temp) / IO)
            if result.get("status") != STATUS["ok"] or result.get("started") != 1:
                raise AssertionError(f"mapped {name}: {result}")
            requests = sorted([dll.casefold(), symbol] for dll, symbol in result["requests"])
            if requests != REQUESTS:
                raise AssertionError(f"mapped {name}: import requests {result['requests']}")
            if launched != expected or result["exit"] != expected:
                raise AssertionError(f"{name}: Windows launch exited {launched:#x}, pe_map {result['exit']:#x}, "
                                     f"expected {expected:#x} for both")
            print(f"{name}: Windows launch {launched:#x}, pe_map {result['exit']:#x}")

        staged = Path(temp) / Path(original).name
        result = mapped(mapper_path, staged, Path(temp) / XMEM, Path(temp) / IO, EXPORT)
        if result.get("status") != STATUS["unresolved"] or "started" in result or \
                result.get("detail") != f"unresolved import {XMEM}!{EXPORT}":
            raise AssertionError(f"provider without {EXPORT}: {result}")
        print(f"provider without {EXPORT}: {result['detail']}")
    print("XMem reserve+commit pool: PASS")


if __name__ == "__main__":
    if sys.argv[1] == "--child":
        child(*sys.argv[2:6], *(sys.argv[6:7] or [""]))
    else:
        main(*map(Path, sys.argv[1:]))
