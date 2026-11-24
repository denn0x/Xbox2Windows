"""Check the DirectStorage heartbeat's commit and physical-page section counters."""
import ctypes as C
from pathlib import Path
import os
import re
import sys
import tempfile
import time

PAGE_BYTES = 65536
PHYSICAL_64K = 0x20400000
WIN_RESERVE = 0x2000
RESERVE_PLACEHOLDER = 0x40000
WIN_RELEASE = 0x8000
NOACCESS = 1
HEARTBEAT = re.compile(
    r"x2w-dstorage heartbeat (\d+) s: .*?commit peak=(\d+) MiB current=(\d+) MiB, "
    r"live physical-page sections=(\d+|unknown)")


class MemoryCounters(C.Structure):
    _fields_ = [
        ("cb", C.c_ulong),
        ("PageFaultCount", C.c_ulong),
        ("PeakWorkingSetSize", C.c_size_t),
        ("WorkingSetSize", C.c_size_t),
        ("QuotaPeakPagedPoolUsage", C.c_size_t),
        ("QuotaPagedPoolUsage", C.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", C.c_size_t),
        ("QuotaNonPagedPoolUsage", C.c_size_t),
        ("PagefileUsage", C.c_size_t),
        ("PeakPagefileUsage", C.c_size_t),
        ("PrivateUsage", C.c_size_t),
    ]


def bind(library, name, args, result):
    function = getattr(library, name)
    function.argtypes, function.restype = args, result
    return function


def process_commit():
    psapi = C.WinDLL("psapi.dll", use_last_error=True)
    query = bind(psapi, "GetProcessMemoryInfo", [C.c_void_p, C.POINTER(MemoryCounters), C.c_ulong], C.c_int)
    counters = MemoryCounters()
    counters.cb = C.sizeof(counters)
    assert query(C.c_void_p(-1), C.byref(counters), counters.cb), C.get_last_error()
    return counters.PagefileUsage, counters.PeakPagefileUsage


def wait_for_sample(log, predicate):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        for match in HEARTBEAT.finditer(log.read_text(errors="replace")):
            seconds, peak, current, sections = match.groups()
            sample = int(seconds), int(peak), int(current), sections
            if predicate(sample):
                return sample
        time.sleep(0.2)
    raise AssertionError("no matching heartbeat sample: " + log.read_text(errors="replace")[-1200:])


def main(xmem_path, xcrdapi_path):
    log = Path(tempfile.gettempdir()) / f"x2w-heartbeat-{os.getpid()}.txt"
    log.write_bytes(b"")
    os.environ["X2W_TRAP_LOG"] = str(log)
    xcrdapi = C.WinDLL(str(Path(xcrdapi_path).resolve()), use_last_error=True, winmode=0x900)
    kernel = C.WinDLL("kernelbase.dll", use_last_error=True)
    pointer, number = C.c_void_p, C.c_size_t
    get_factory = bind(xcrdapi, "DStorageGetFactory", [pointer, C.POINTER(pointer)], C.c_long)
    factory = pointer()
    assert get_factory(None, C.byref(factory)) == 0 and factory.value, "DStorageGetFactory failed"
    first_sample = wait_for_sample(log, lambda sample: sample[3] == "unknown")
    library = C.WinDLL(str(Path(xmem_path).resolve()), use_last_error=True, winmode=0x900)
    allocate = bind(library, "XMemAllocatePhysicalPages", [number, C.POINTER(number), C.POINTER(number)], C.c_int)
    release = bind(library, "XMemFreePhysicalPages", [number, C.POINTER(number)], C.c_int)
    contiguous = bind(library, "XMemMapPhysicalPages", [pointer, number, C.POINTER(number)], pointer)
    commit = bind(kernel, "VirtualAlloc", [pointer, number, C.c_ulong, C.c_ulong], pointer)
    reserve = bind(kernel, "VirtualAlloc2", [pointer, pointer, number, C.c_ulong, C.c_ulong, pointer, C.c_ulong],
                   pointer)
    free = bind(kernel, "VirtualFree", [pointer, number, C.c_ulong], C.c_int)

    first = reserve(pointer(-1), None, PAGE_BYTES, WIN_RESERVE | RESERVE_PLACEHOLDER, NOACCESS, None, 0)
    second = reserve(pointer(-1), None, PAGE_BYTES, WIN_RESERVE | RESERVE_PLACEHOLDER, NOACCESS, None, 0)
    assert first and second, C.get_last_error()
    count, pages = number(2), (number * 2)()
    one, other = number(0), number(0)
    extra = None
    try:
        extra = commit(None, 16 << 20, 0x3000, 4)  # MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE
        assert extra, C.get_last_error()
        # Two live sections are visible while the heartbeat samples.
        assert allocate(PHYSICAL_64K, C.byref(count), pages) and count.value == 2
        one.value, other.value = pages[0], pages[1]
        assert contiguous(first, 1, C.byref(one)) == first
        assert contiguous(second, 1, C.byref(other)) == second
        _, earlier_peak = process_commit()

        second_mark, peak_mib, current_mib, sections = wait_for_sample(
            log, lambda sample: sample[0] > first_sample[0] and sample[3] == "2")
        _, later_peak = process_commit()
        assert peak_mib >= current_mib > 0, f"implausible commit: peak={peak_mib} MiB current={current_mib} MiB"
        assert earlier_peak // (1 << 20) <= peak_mib <= later_peak // (1 << 20)

        assert free(extra, 0, WIN_RELEASE), C.get_last_error()
        extra = None
        released = wait_for_sample(log, lambda sample: sample[0] > second_mark and sample[3] == "2")
        assert released[1] == peak_mib and released[2] <= current_mib - 8, (peak_mib, current_mib, released)
        print(f"sections unknown at {first_sample[0]} s, then {sections} at {second_mark} s; "
              f"commit fell from {current_mib} to {released[2]} MiB at unchanged peak {peak_mib} MiB")
    finally:
        if extra:
            free(extra, 0, WIN_RELEASE)
        if count.value:
            contiguous(first, 1, None)
            contiguous(second, 1, None)
            release(2, pages)
        free(first, 0, WIN_RELEASE)
        free(second, 0, WIN_RELEASE)
        try:
            log.unlink()
        except OSError:
            pass


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
