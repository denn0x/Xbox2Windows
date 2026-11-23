"""Start the load-config callback EXEs (#903) under the Windows loader and pe_map; compare the results.

tests/cpu/load_config_callback.cpp, compiled with /guard:cf, calls the two entries of a read-only callback
table through the load config's dispatch slot (__guard_dispatch_icall_fptr). The second entry is null and
is skipped. The callback hands emit_trace (cpu_exe_io.dll) three words: a | b << 32, c | calls << 32 and
d - __ImageBase, which the stderr and stdout bytes carry. The exit code is the callback's result, ORed with
0x10000 if the dispatch slot no longer holds the image's own `jmp rax`, and 0x20000 for the check slot.

The record, other and invalid images are linked without /guard:cf, and cfg_record and cfg_invalid are
the same objects linked with it. other binds a different callback (the negative control); the two invalid
images bind a target inside a function, which the /guard:cf function table leaves out.

Checked from each image's own PE: the guard slots and the callback table are in a section without write
access and carry DIR64 relocations, the null entry has none, the load-config fields, and the two
`call [dispatch slot]` instructions of the caller. In pe_map's mapping, before the entry runs: the slots
hold the relocated in-image targets on read-only pages.

Windows replaces both slots of a /guard:cf image and fails a call to the invalid target fast
(0xC0000409). pe_map does not implement CFG: it keeps the image's own slots, so the valid callback runs
without the slot bits and the invalid target runs. Both are asserted as the current pe_map behavior, so a
change to the loader policy has to update this test.
"""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

import pefile

from exe_process import launch
from native_map_check import Mapper, STATUS

IO = "cpu_exe_io.dll"
VARIANTS = ("record", "other", "invalid", "cfg_record", "cfg_invalid")
GUARD_CF = 0x4000  # IMAGE_DLLCHARACTERISTICS_GUARD_CF
CF_INSTRUMENTED, CF_TABLE = 0x100, 0x400  # IMAGE_GUARD_CF_INSTRUMENTED, IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT
SLOT_BITS = 0x30000
FAST_FAIL = 0xC0000409  # STATUS_STACK_BUFFER_OVERRUN, raised by __fastfail
PAGE_READONLY = 0x02


def words(a, b, c, calls, d):
    return a | b << 32, c | calls << 32, d


def layout(image):
    """Static facts from the image's own PE."""
    with pefile.PE(str(image)) as pe:
        base = pe.OPTIONAL_HEADER.ImageBase
        config = pe.DIRECTORY_ENTRY_LOAD_CONFIG.struct
        dir64 = {e.rva for block in pe.DIRECTORY_ENTRY_BASERELOC for e in block.entries
                 if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_DIR64"]}
        check, dispatch = (config.GuardCFCheckFunctionPointer - base, config.GuardCFDispatchFunctionPointer - base)
        table = check - 16  # callback_table precedes the two slots in .const
        slots = {"table[0]": table, "table[1]": table + 8, "check": check, "dispatch": dispatch}
        facts = {"base": base, "slots": slots, "size": config.Size, "flags": config.GuardFlags,
                 "guard_cf": bool(pe.OPTIONAL_HEADER.DllCharacteristics & GUARD_CF),
                 "values": {name: pe.get_qword_at_rva(rva) for name, rva in slots.items()},
                 "relocated": {name: rva in dir64 for name, rva in slots.items()}}
        sections = {}
        for name, rva in slots.items():
            section = pe.get_section_by_rva(rva)
            sections[name] = (section.Name.rstrip(b"\0").decode(), section.Characteristics)
        facts["sections"] = sections
        text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
        code, start = text.get_data(), text.VirtualAddress
        facts["dispatch_calls"] = sum(
            1 for i in range(len(code) - 5) if code[i:i + 2] == b"\xff\x15"
            and start + i + 6 + struct.unpack_from("<i", code, i + 2)[0] == dispatch)
        fids = []
        if config.GuardCFFunctionCount:
            stride = 4 + (config.GuardFlags >> 28)
            data = pe.get_data(config.GuardCFFunctionTable - base, config.GuardCFFunctionCount * stride)
            fids = [struct.unpack_from("<I", data, i * stride)[0] for i in range(config.GuardCFFunctionCount)]
        facts["fids"] = fids
        facts["data"] = {s.Name.rstrip(b"\0").decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in pe.sections}
        return facts


def check_layout(name, facts):
    if facts["size"] != 0x138:
        raise AssertionError(f"{name}: load config size {facts['size']:#x}")
    for slot, (section, characteristics) in facts["sections"].items():
        if section != ".rdata" or characteristics & pefile.SECTION_CHARACTERISTICS["IMAGE_SCN_MEM_WRITE"]:
            raise AssertionError(f"{name}: {slot} is in {section} ({characteristics:#x}), expected read-only .rdata")
    expected = {"table[0]": True, "table[1]": False, "check": True, "dispatch": True}
    if facts["relocated"] != expected:
        raise AssertionError(f"{name}: DIR64 relocations {facts['relocated']}, expected {expected}")
    if facts["values"]["table[1]"] != 0:
        raise AssertionError(f"{name}: the absent callback entry is not null")
    text_rva, text_size = facts["data"][".text"]
    for slot in ("table[0]", "check", "dispatch"):
        rva = facts["values"][slot] - facts["base"]
        if not text_rva <= rva < text_rva + text_size:
            raise AssertionError(f"{name}: {slot} does not point into .text")
    if facts["dispatch_calls"] != 2:
        raise AssertionError(f"{name}: {facts['dispatch_calls']} calls through the dispatch slot, expected 2")
    cfg = name.startswith("cfg_")
    if facts["guard_cf"] != cfg:
        raise AssertionError(f"{name}: GUARD_CF characteristic {facts['guard_cf']}")
    target = facts["values"]["table[0]"] - facts["base"]
    if cfg:
        if facts["flags"] & (CF_INSTRUMENTED | CF_TABLE) != CF_INSTRUMENTED | CF_TABLE:
            raise AssertionError(f"{name}: guard flags {facts['flags']:#x}")
        if (target in facts["fids"]) != (name == "cfg_record"):
            raise AssertionError(f"{name}: callback target {target:#x} in function table: {target in facts['fids']}")
    elif facts["flags"] != CF_INSTRUMENTED or facts["fids"]:
        raise AssertionError(f"{name}: guard flags {facts['flags']:#x} with {len(facts['fids'])} table entries")
    print(f"{name}: slots in read-only .rdata with DIR64 relocations (null entry has none), "
          f"guard flags {facts['flags']:#x}, GUARD_CF {facts['guard_cf']}, {len(facts['fids'])} function table "
          f"entries, 2 calls through the dispatch slot")


def child(mapper_path, image, io, report):
    """Map the EXE in this process, record the slots, start it, and write the observation to report."""
    mapper = Mapper(mapper_path)
    run_entry = ctypes.CDLL(str(mapper_path)).pe_map_run
    run_entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    run_entry.restype = ctypes.c_int
    base, status, detail, requests = mapper(Path(image), {IO: ctypes.CDLL(str(io))})
    result = {"status": status, "detail": detail, "requests": requests}
    if base:
        facts = layout(image)
        query = ctypes.WinDLL("kernel32").VirtualQuery
        query.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

        class Info(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("allocation", ctypes.c_void_p), ("allocation_protect",
                        wintypes.DWORD), ("partition", wintypes.WORD), ("size", ctypes.c_size_t),
                        ("state", wintypes.DWORD), ("protect", wintypes.DWORD), ("type", wintypes.DWORD)]
        slots = {}
        for name, rva in facts["slots"].items():
            info = Info()
            query(base + rva, ctypes.byref(info), ctypes.sizeof(info))
            value = ctypes.c_uint64.from_address(base + rva).value
            stored = facts["values"][name]
            slots[name] = {"protect": info.protect,
                           "relocated": value == (stored - facts["base"] + base if stored else 0)}
        result["slots"] = slots
        code = ctypes.c_uint32(0xdeadbeef)
        result["started"] = run_entry(base, ctypes.byref(code))
        result["exit"] = code.value
        mapper.release(base)
    Path(report).write_text(json.dumps(result))


def mapped(mapper_path, image, io, folder):
    report = Path(folder) / (Path(image).stem + ".json")
    run = subprocess.run([sys.executable, "-B", __file__, "--child", str(mapper_path), str(image), str(io),
                          str(report)], capture_output=True, timeout=60)
    if run.returncode:
        raise AssertionError(f"mapped {Path(image).name}: child exited {run.returncode & 0xffffffff:#x}: "
                             f"{run.stderr[-400:]!r}")
    return json.loads(report.read_text()), run.stdout, run.stderr


def trace(stderr, stdout):
    if len(stderr) != 16 or len(stdout) != 8:
        return None
    return struct.unpack("<QQ", stderr) + struct.unpack("<Q", stdout)


def show(row):
    code, values = row
    return f"exit {code:#x} trace " + ("none" if values is None else " ".join(f"{v:#x}" for v in values))


def main(mapper_path, io, *images):
    images = dict(zip(VARIANTS, images))
    with TemporaryDirectory() as temp:
        shutil.copyfile(io, Path(temp) / IO)
        staged_io = Path(temp) / IO
        observed = {}
        for name, image in images.items():
            staged = Path(temp) / Path(image).name
            shutil.copyfile(image, staged)
            facts = layout(staged)
            check_layout(name, facts)
            marker = facts["data"][".data"][0] + 8  # marker follows __security_cookie
            expected = {
                "record": (0x55, words(0x11, 0x22, 0x33, 1, marker)),
                "other": (0x99, words(0x22, 0x11, 0x33, 1, marker)),
                "invalid": (0x77, words(0, 0, 0, 0, 0)),
            }
            row = launch([staged], b"", timeout=30)
            if not row["process_signaled"]:
                raise AssertionError(f"{name}: Windows launch did not end: {row}")
            native = (row["code"], trace(row["stderr"]["bytes"], row["stdout"]["bytes"]))
            result, out, err = mapped(mapper_path, staged, staged_io, temp)
            if result["status"] != STATUS["ok"] or result.get("started") != 1:
                raise AssertionError(f"mapped {name}: {result}")
            for slot, state in result["slots"].items():
                if state["protect"] != PAGE_READONLY or not state["relocated"]:
                    raise AssertionError(f"mapped {name}: {slot} {state}")
            mapped_row = (result["exit"], trace(err, out))
            observed[name] = native, mapped_row

            base = name.removeprefix("cfg_")
            if name in expected:
                if native != expected[name] or mapped_row != expected[name]:
                    raise AssertionError(f"{name}: Windows {native}, pe_map {mapped_row}, expected {expected[name]}")
            elif name == "cfg_record":
                code, words_ = expected[base]
                if native != (code | SLOT_BITS, words_):
                    raise AssertionError(f"{name}: Windows {native}, expected both slots replaced and {code:#x}")
                if mapped_row != (code, words_):
                    raise AssertionError(f"{name}: pe_map {mapped_row}, expected its own slots kept and {code:#x}")
            else:  # cfg_invalid
                if native != (FAST_FAIL, None) or row["stdout"]["bytes"] or row["stderr"]["bytes"]:
                    raise AssertionError(f"{name}: Windows {native}, expected a fast fail before any output")
                if mapped_row != expected[base]:
                    raise AssertionError(f"{name}: pe_map {mapped_row}, expected the unchecked call {expected[base]}")
            print(f"{name}: Windows {show(native)}; pe_map {show(mapped_row)}; mapped slots read-only and relocated")

        if observed["other"][0] == observed["record"][0]:
            raise AssertionError("the control binding was not distinguished")
    print("load-config dispatch callback: PASS (pe_map keeps the image's own slots for /guard:cf images)")


if __name__ == "__main__":
    if sys.argv[1] == "--child":
        child(*sys.argv[2:6])
    else:
        main(*map(Path, sys.argv[1:]))
