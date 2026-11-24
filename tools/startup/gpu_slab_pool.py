"""gpu_slab_pool.py: holds a fixed set of named pagefile sections that the memory stand-in (xmem) uses for slabs it
moves to the pagefile for the GPU. LOCAL ONLY.

Why (out/gamerun/memleak_work/NOTES.md): a pagefile-backed section that has been opened as a D3D12 heap
(ID3D12Device3::OpenExistingHeapFromFileMapping) keeps its whole commit charge after the process that opened it is
gone, until the next reboot, however that process ended. When the sections come from this long-lived holder
instead, the same section objects are used run after run, so the charge stays fixed at the pool size instead of
growing by every moved slab of every run.

  python -B gpu_slab_pool.py ensure [--count N] [--mib M]   start the holder unless it is running (detached)
  python -B gpu_slab_pool.py status
  python -B gpu_slab_pool.py quit                            end the holder (its sections stay charged if leaked)
  python -B gpu_slab_pool.py hold --count N --mib M          (the holder itself)

Names: Local\\x2w-gpu-slab-<i> (i = 0..N-1), events Local\\x2w-gpu-slab-pool-ready / -quit. Each section is
SEC_COMMIT, M MiB (xmem's slab size is 256 MiB). xmem opens them in order (X2W_XMEM_GPU_POOL, default on); a run
that needs more falls back to fresh sections (logged). Only one title runs at a time (machine lock), so no two
processes use the pool at once.
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

PREFIX = "Local\\x2w-gpu-slab-"
READY = "Local\\x2w-gpu-slab-pool-ready"
QUIT = "Local\\x2w-gpu-slab-pool-quit"
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileMappingW.restype = wt.HANDLE
k32.CreateFileMappingW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPCWSTR]
k32.OpenEventW.restype = wt.HANDLE
k32.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.CreateEventW.restype = wt.HANDLE
k32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
PAGE_READWRITE, SEC_COMMIT, SYNCHRONIZE, EVENT_MODIFY_STATE, FILE_MAP_READ = 0x04, 0x8000000, 0x100000, 0x2, 0x4


def opt(name, default):
    return int(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def running():
    h = k32.OpenEventW(SYNCHRONIZE, False, READY)
    if not h:
        return False
    k32.CloseHandle(h)
    return True


def hold(count, mib):
    quit_event = k32.CreateEventW(None, True, False, QUIT)
    handles = []
    size = mib << 20
    for i in range(count):
        h = k32.CreateFileMappingW(wt.HANDLE(-1), None, PAGE_READWRITE | SEC_COMMIT, size >> 32, size & 0xFFFFFFFF, PREFIX + str(i))
        if not h:
            print(f"section {i} failed: {ctypes.get_last_error()}", flush=True)
            break
        handles.append(h)
    ready = k32.CreateEventW(None, True, True, READY)
    print(f"holding {len(handles)} x {mib} MiB sections ({PREFIX}0..{len(handles) - 1})", flush=True)
    k32.WaitForSingleObject(wt.HANDLE(quit_event), 0xFFFFFFFF)
    for h in handles:
        k32.CloseHandle(wt.HANDLE(h))
    k32.CloseHandle(wt.HANDLE(ready))
    return 0


def reachable():
    """How many pool sections can be opened by name now."""
    n = 0
    while True:
        h = k32.OpenFileMappingW(FILE_MAP_READ, False, PREFIX + str(n))
        if not h:
            return n
        k32.CloseHandle(wt.HANDLE(h))
        n += 1


def ensure(count, mib=256):
    """Starts the holder unless it runs; returns the number of pool sections reachable (0 if it failed)."""
    if running():
        return reachable()
    # Detached, outside any job of the caller, no console: it must outlive the run that started it.
    flags = 0x00000008 | 0x00000200 | 0x01000000  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB
    log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "gpu_slab_pool.log")
    args = [sys.executable, "-B", os.path.abspath(__file__), "hold", "--count", str(count), "--mib", str(mib)]
    with open(log_path, "a") as log:
        try:
            subprocess.Popen(args, creationflags=flags, stdout=log, stderr=log, stdin=subprocess.DEVNULL, close_fds=True)
        except OSError:  # the caller's job may refuse breakaway: start without it
            print("gpu_slab_pool: the holder could not leave the caller's job and ends with it; a later holder charges "
                  "its pool again", flush=True)
            subprocess.Popen(args, creationflags=flags & ~0x01000000, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                             close_fds=True)
    for _ in range(100):
        if running():
            return reachable()
        time.sleep(0.1)
    return 0


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "status"
    count, mib = opt("--count", 12), opt("--mib", 256)
    if what == "hold":
        return hold(count, mib)
    if what == "status":
        print(f"holder {'running' if running() else 'not running'}; {reachable()} pool section(s) reachable")
        return 0
    if what == "quit":
        h = k32.OpenEventW(EVENT_MODIFY_STATE, False, QUIT)
        if h:
            k32.SetEvent(wt.HANDLE(h))
            k32.CloseHandle(wt.HANDLE(h))
        print("quit signalled" if h else "holder not running")
        return 0
    if what == "ensure":
        n = ensure(count, mib)
        print(f"{n} pool section(s) reachable" if n else "holder did not report ready within 10 s")
        return 0 if n else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
