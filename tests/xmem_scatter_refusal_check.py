"""Refused scatter maps in the permissive xmem shim (X2W_XMEM_PERMISSIVE=1): the run-shaped batch, two threads
naming the same addresses, and the loud line a refusal must leave behind.

usage: xmem_scatter_refusal_check.py <xmem.dll> [--control]
Each case runs in a child process (--child <case> <xmem.dll> <log>) with X2W_XMEM_PERMISSIVE=1,
X2W_XMEM_GPU_POOL=0 (no pool section is opened) and X2W_TRAP_LOG=<log>. No D3D12.
Cases:
  shape    a 717-page batch of 25 scattered ranges in unsorted order, every page already mapped at another
           address (a second view, as the startup run's streaming batches): maps, and both views show the same bytes.
  race     thread A maps and unmaps that batch, thread B maps and unmaps one of its addresses with another page,
           for a few seconds. The console applies every call; none may fail.
  foreign  the target page is a view that the shim did not make: the call fails with error 87, and an
           "x2w-xmem REFUSED" line names the entry, each Windows call's error and what VirtualQuery shows there.
  unowned  a token for a page that was never granted: fails with 87 and a REFUSED line that says so.
  remap    thread A remaps one address between two pages (each call unmaps and maps it again), thread B checks it:
           a plain VirtualQuery sometimes sees it uncommitted (the gap; must be seen, or the case proves nothing),
           x2w_xmem_write_settled never refuses it, and x2w_xmem_page_event reports the last map / unmap.
--control: the same cases against a build without the fix (e.g. 08d2ca72): passes when the race case fails and
           the foreign / unowned cases leave no REFUSED line (the negative control trips); remap there only shows
           the gap (the build has no x2w_xmem_write_settled).
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

PAGE = 0x10000
PHYSICAL_64K = 0x20400000
STARTUP_RESERVE = 0x20402000
MEM_RELEASE = 0x8000
MEM_REPLACE_PLACEHOLDER, MEM_PRESERVE_PLACEHOLDER = 0x4000, 0x2
# The 25 destination ranges (pages) of the refused batch of the startup run, in its order; offsets are synthetic.
RANGES = [1, 13, 6, 3, 7, 1, 9, 2, 5, 5, 8, 117, 7, 120, 8, 119, 6, 1, 17, 7, 112, 7, 132, 3, 1]
assert sum(RANGES) == 717


class Shim:
    def __init__(self, path):
        lib = C.WinDLL(str(Path(path).resolve()), use_last_error=True, winmode=0x900)
        k = C.WinDLL("kernelbase.dll", use_last_error=True)
        vp, n, dw = C.c_void_p, C.c_size_t, C.c_ulong
        def bind(owner, name, args, res):
            f = getattr(owner, name)
            f.argtypes, f.restype = args, res
            return f
        self.allocate = bind(lib, "XMemAllocatePhysicalPages", [n, C.POINTER(n), C.POINTER(n)], C.c_int)
        self.scatter_raw = bind(lib, "XMemMapPhysicalPagesScatter", [C.POINTER(vp), n, C.POINTER(n)], C.c_int)
        self.virtual_alloc = bind(lib, "XMemVirtualAlloc", [vp, n, dw, n, dw], vp)
        self.map3 = bind(k, "MapViewOfFile3", [vp, vp, vp, C.c_ulonglong, n, dw, dw, vp, dw], vp)
        self.unmap2 = bind(k, "UnmapViewOfFile2", [vp, vp, dw], C.c_int)
        self.mapping = bind(k, "CreateFileMappingW", [vp, vp, dw, dw, dw, vp], vp)
        self.process = bind(k, "GetCurrentProcess", [], vp)()
        self.base = self.virtual_alloc(None, 16 << 30, STARTUP_RESERVE, 0x21, 4)
        assert self.base, C.get_last_error()

    def pages(self, count):
        got, tokens = C.c_size_t(count), (C.c_size_t * count)()
        assert self.allocate(PHYSICAL_64K, C.byref(got), tokens) and got.value == count, C.get_last_error()
        return list(tokens)

    def scatter(self, addresses, tokens):
        a = (C.c_void_p * len(addresses))(*addresses)
        t = (C.c_size_t * len(tokens))(*tokens)
        ok = self.scatter_raw(a, len(addresses), t)
        return ok, C.get_last_error()


def batch_addresses(base):
    """717 page addresses in 25 ranges spread over the reservation, in the run's (unsorted) range order."""
    order = [5, 1, 14, 22, 9, 13, 15, 2, 8, 17, 12, 19, 7, 20, 0, 18, 10, 3, 6, 11, 21, 23, 4, 16, 24]
    addresses = []
    for slot, pages in zip(order, RANGES):
        start = base + 0x10000000 + slot * 0x18000000  # 384 MiB apart, 256 MiB in
        addresses += [start + i * PAGE for i in range(pages)]
    return addresses


def word(address):
    return C.c_uint32.from_address(address)


def child(case, dll, log):
    shim = Shim(dll)
    addresses = batch_addresses(shim.base)
    tokens = shim.pages(len(addresses) + 16)
    pages = tokens[:len(addresses)]
    if case == "shape":
        home = shim.base + 0x3F0000000  # the pages' first view, as the title's contiguous mapping
        ok, err = shim.scatter([home + i * PAGE for i in range(len(pages))], pages)
        assert ok, f"first views refused: {err}"
        for i in range(len(pages)):
            word(home + i * PAGE).value = 0xA5000000 + i
        ok, err = shim.scatter(addresses, pages)
        assert ok, f"the run-shaped batch was refused: error {err}"
        bad = [i for i, a in enumerate(addresses) if word(a).value != 0xA5000000 + i]
        assert not bad, f"second views do not show the pages' bytes at entries {bad[:5]}"
        word(addresses[0]).value = 0x12345678
        assert word(home).value == 0x12345678, "a write through the second view is not seen through the first"
        print("shape ok: 717 pages, 25 ranges, unsorted, second views agree")
        return 0
    if case == "race":
        other = tokens[len(addresses):]
        target = addresses[400]
        stop = time.perf_counter() + float(os.environ.get("RACE_SECONDS", "6"))
        result = {"a_calls": 0, "a_fail": [], "b_calls": 0, "b_fail": []}
        def a():
            while time.perf_counter() < stop:
                for toks in (pages, [0] * len(pages)):
                    ok, err = shim.scatter(addresses, toks)
                    result["a_calls"] += 1
                    if not ok: result["a_fail"].append(err)
        def b():
            i = 0
            while time.perf_counter() < stop:
                ok, err = shim.scatter([target], [other[i % len(other)] if i % 2 == 0 else 0])
                result["b_calls"] += 1
                if not ok: result["b_fail"].append(err)
                i += 1
        threads = [threading.Thread(target=a), threading.Thread(target=b)]
        for t in threads: t.start()
        for t in threads: t.join()
        fails = result["a_fail"] + result["b_fail"]
        print(f"race calls A={result['a_calls']} B={result['b_calls']} failures A={len(result['a_fail'])} "
              f"B={len(result['b_fail'])} errors={sorted(set(fails))}")
        return 3 if fails else 0
    if case == "foreign":
        target = addresses[0]
        # A view elsewhere in the reservation keeps the shim from coalescing its placeholders after the unmap below.
        ok, err = shim.scatter([shim.base + 0x3F0000000], [pages[5]])
        assert ok, err
        ok, err = shim.scatter([target], [pages[0]])
        assert ok, err
        ok, err = shim.scatter([target], [0])  # unmapped again: a 64 KiB placeholder stays
        assert ok, err
        section = shim.mapping(C.c_void_p(-1), None, 4, 0, PAGE, None)
        assert section
        view = shim.map3(section, shim.process, target, 0, PAGE, MEM_REPLACE_PLACEHOLDER, 4, None, 0)
        assert view == target, f"the test could not put its own view there: {C.get_last_error()}"
        second = target + 0x1000000  # above the target: the target is entry 0 of the sorted call
        ok, err = shim.scatter([second, target], [pages[1], pages[0]])
        print(f"foreign view at the target: ok={ok} error={err}")
        assert not ok and err == 87, (ok, err)
        assert shim.unmap2(shim.process, target, MEM_PRESERVE_PLACEHOLDER)
        ok, err = shim.scatter([second, target], [pages[1], pages[0]])
        assert ok, f"after the foreign view is gone the same call must map: {err}"
        return 0
    if case == "remap":
        lib = C.WinDLL(str(Path(dll).resolve()), use_last_error=True, winmode=0x900)
        k = C.WinDLL("kernelbase.dll", use_last_error=True)
        k.VirtualQuery.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        k.VirtualQuery.restype = C.c_size_t
        settled = getattr(lib, "x2w_xmem_write_settled", None)
        event = getattr(lib, "x2w_xmem_page_event", None)
        if settled:
            settled.argtypes, settled.restype = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_size_t], C.c_int
            u32, u64 = C.POINTER(C.c_ulong), C.POINTER(C.c_ulonglong)
            event.argtypes, event.restype = [C.c_void_p, u32, u64, u32, u64, u64, u64], C.c_int
        target = addresses[200]
        ok, err = shim.scatter([shim.base + 0x3F0000000], [pages[5]])  # keeps the reservation from coalescing
        assert ok, err
        ok, err = shim.scatter([target], [pages[0]])
        assert ok, err
        stop = time.perf_counter() + float(os.environ.get("RACE_SECONDS", "4"))
        counts = {"remaps": 0, "remap_fail": 0, "checks": 0, "gap": 0, "settled": 0, "settled_fail": 0}
        def a():
            i = 0
            while time.perf_counter() < stop:
                ok, _ = shim.scatter([target], [pages[1 + (i % 2)]])  # P -> Q -> P ...: unmap + map in one call
                counts["remaps"] += 1
                counts["remap_fail"] += not ok
                i += 1
        def b():
            info = (C.c_ubyte * 48)()
            payload = (C.c_ubyte * 16)(*range(16))
            while time.perf_counter() < stop:
                assert k.VirtualQuery(target, info, 48) == 48
                state = int.from_bytes(bytes(info[32:36]), "little")
                counts["checks"] += 1
                counts["gap"] += state != 0x1000
                if settled:
                    counts["settled"] += 1
                    counts["settled_fail"] += not settled(target, payload, 16, PAGE)
        threads = [threading.Thread(target=a), threading.Thread(target=b)]
        for t in threads: t.start()
        for t in threads: t.join()
        line = " ".join(f"{key}={value}" for key, value in counts.items())
        if not settled:
            print(f"remap (no x2w_xmem_write_settled in this build): {line}")
            return 0 if counts["gap"] else 4
        kind, thread = C.c_ulong(), C.c_ulong()
        page, age, serial, remaps = C.c_ulonglong(), C.c_ulonglong(), C.c_ulonglong(), C.c_ulonglong()
        view = event(target, C.byref(kind), C.byref(page), C.byref(thread), C.byref(age), C.byref(serial), C.byref(remaps))
        ok, err = shim.scatter([target], [0])
        assert ok, err
        view_after = event(target, C.byref(kind), C.byref(page), C.byref(thread), C.byref(age), C.byref(serial), None)
        never = C.c_ulong()
        view_never = event(shim.base + 0x3E0000000, C.byref(never), None, None, None, None, None)
        print(f"remap: {line}; page_event mapped={view} remaps={remaps.value}; after unmap view={view_after} "
              f"kind={kind.value} page=0x{page.value:x}; untouched address view={view_never} kind={never.value}")
        good = (counts["gap"] > 0 and counts["settled_fail"] == 0 and counts["remap_fail"] == 0 and view == 1 and
                remaps.value >= counts["remaps"] - 1 and view_after == 0 and kind.value == 2 and page.value == pages[1 + ((counts["remaps"] - 1) % 2)] >> 4
                and view_never == 0 and never.value == 0)
        return 0 if good else 5
    if case == "unowned":
        ok, err = shim.scatter([addresses[0], addresses[1]], [pages[0], (max(tokens) >> 4 << 4) + (1000 << 4)])
        print(f"unowned token: ok={ok} error={err}")
        assert not ok and err == 87, (ok, err)
        return 0
    raise SystemExit(f"unknown case {case}")


def run(case, dll, log):
    if log.exists(): log.unlink()
    env = dict(os.environ, X2W_XMEM_PERMISSIVE="1", X2W_XMEM_GPU_POOL="0", X2W_TRAP_LOG=str(log))
    for name in ("X2W_XMEM_SLAB_DIR", "X2W_XMEM_SLAB_BUDGET_MIB", "X2W_XMEM_SHARED_VIEWS", "X2W_XMEM_GPU_MIRROR"):
        env.pop(name, None)
    done = subprocess.run([sys.executable, "-B", __file__, "--child", case, str(dll), str(log)], env=env,
                          capture_output=True, text=True, timeout=180)
    text = log.read_text(errors="replace") if log.exists() else ""
    return done.returncode, done.stdout.strip(), text


def main():
    if sys.argv[1] == "--child":
        return child(sys.argv[2], sys.argv[3], sys.argv[4])
    dll = Path(sys.argv[1]).resolve()
    control = "--control" in sys.argv
    logs = Path(os.environ.get("XMEM_CHECK_LOGS", Path(dll).parent))
    results = {}
    for case in ("shape", "race", "foreign", "unowned", "remap"):
        code, out, text = run(case, dll, logs / f"scatter_{case}.log")
        refused = [line for line in text.splitlines() if "x2w-xmem REFUSED" in line]
        results[case] = (code, out, refused, text)
        print(f"[{case}] exit {code}: {out}")
        for line in refused[:2]:
            print(f"    {line[:700]}")
    failures = []
    if not control:
        if results["shape"][0]: failures.append("shape refused")
        if results["race"][0]: failures.append("race: a call failed")
        code, out, refused, _ = results["foreign"]
        if code: failures.append("foreign: wrong outcome")
        need = ("entry 0 of 2", "MapViewOfFile3 err 487", "split err 87", "state 0x1000", "type 0x40000", "no view")
        if not refused or not all(n in refused[0] for n in need):
            failures.append(f"foreign: REFUSED line missing or incomplete (needs {need})")
        code, out, refused, _ = results["unowned"]
        if code: failures.append("unowned: wrong outcome")
        if not refused or "NOT granted" not in refused[0] or "not granted (era_physical::owns)" not in refused[0]:
            failures.append("unowned: REFUSED line missing or does not name the check")
        if results["remap"][0]: failures.append("remap: gap not seen, or a settled write / page event was wrong")
        print("FAIL: " + "; ".join(failures) if failures else "PASS xmem_scatter_refusal (shape, race, foreign, unowned, remap)")
    else:
        if results["shape"][0]: failures.append("shape refused on the control build too")
        if not results["race"][0]: failures.append("race did not fail on the control build")
        if results["foreign"][2] or results["unowned"][2]: failures.append("control build printed REFUSED lines")
        if results["remap"][0]: failures.append("remap: the gap was not seen on the control build")
        print("FAIL control: " + "; ".join(failures) if failures else
              "PASS control: the build without the fix fails the race and logs no cause")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
