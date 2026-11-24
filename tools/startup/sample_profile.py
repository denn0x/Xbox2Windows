"""Clock-driven sampling profiler for debug_launch.py (startup diagnostics; off unless X2W_PROFILE is set).

X2W_PROFILE=<start s>,<samples>,<interval ms>[,<repeat every s>] takes <samples> samples, <interval ms>
apart, from <start s> after launch (again every <repeat every s> when given), and writes
profile_<start>.txt per batch into the log folder.

A sample is taken from the debugger's loop when the clock says so, between debug events (never at
one): every live thread is suspended, and its CPU times, integer context and the top of its stack are
copied. Then all threads are resumed and the stacks are walked afterwards from the copies (dbghelp
StackWalk64 with a read routine that serves the copied stack). The target only gets memory reads and
thread queries; nothing runs in it, so a thread suspended inside the loader lock or a heap lock cannot
stall the debugger. Each report states how long the process was frozen per sample.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.SuspendThread.argtypes = k32.ResumeThread.argtypes = [wt.HANDLE]
k32.SuspendThread.restype = k32.ResumeThread.restype = wt.DWORD
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(ctypes.c_uint64)] * 4
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetCurrentProcess.restype = wt.HANDLE
k32.DuplicateHandle.argtypes = [wt.HANDLE, wt.HANDLE, wt.HANDLE, ctypes.POINTER(wt.HANDLE), wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
k32.LocalFree.argtypes = [ctypes.c_void_p]
if hasattr(k32, "GetThreadDescription"):
    k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    k32.GetThreadDescription.restype = ctypes.c_long
ntdll.NtQueryInformationThread.argtypes = [wt.HANDLE, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p]

FRAMES = 12          # return addresses kept per sample (after the leaf)
STACK_BYTES = 0x8000  # stack copied per thread per sample
WAIT_WORDS = ("Wait", "Delay", "Remove", "ReplyWait", "Alert", "GetMessage")
SYSTEM = ("ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll", "user32.dll")
CONTEXT_CONTROL_INTEGER = 0x100003

READ_MEMORY = ctypes.WINFUNCTYPE(wt.BOOL, wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD))


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", ctypes.c_uint32), ("PartitionId", ctypes.c_uint16),
                ("RegionSize", ctypes.c_uint64), ("State", ctypes.c_uint32),
                ("Protect", ctypes.c_uint32), ("Type", ctypes.c_uint32)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_wchar * 512)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


dbghelp.SymInitializeW.argtypes = [wt.HANDLE, ctypes.c_wchar_p, wt.BOOL]
dbghelp.SymRefreshModuleList.argtypes = [wt.HANDLE]
dbghelp.SymCleanup.argtypes = [wt.HANDLE]
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
dbghelp.SymGetModuleInfoW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p]
dbghelp.StackWalk64.argtypes =[wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]


def parse(spec):
    """(start s, samples, interval s, repeat s) from X2W_PROFILE, or None when it is unset or malformed."""
    try:
        parts = [float(p) for p in spec.split(",")]
        if len(parts) < 3 or parts[1] < 2 or parts[2] <= 0:
            return None
        return parts[0], int(parts[1]), parts[2] / 1000.0, parts[3] if len(parts) > 3 else 0.0
    except ValueError:
        return None


class Profiler:
    def __init__(self, plan, process, logdir, where, modules, module_paths, log):
        self.first, self.count, self.interval, self.repeat = plan
        self.logdir, self.where, self.modules, self.module_paths, self.log = logdir, where, modules, module_paths, log
        # A handle of our own for dbghelp: its sessions are keyed by handle value, and the debugger's
        # thread dumps open and close a session on the process handle itself.
        own = wt.HANDLE()
        k32.DuplicateHandle(k32.GetCurrentProcess(), process, k32.GetCurrentProcess(), ctypes.byref(own), 0, False, 2)
        self.process = own
        self.next = self.first
        self.batch = self.first
        self.done = False
        self.samples = []   # (seconds, {tid: (kernel 100ns, user 100ns, frames)})
        self.freeze = []    # seconds each process freeze lasted
        self.walk_cost = []
        self.handles = {}   # tid -> handle (SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION)
        self.info = {}      # tid -> (description, start address)
        self.session = False
        self.snapshot = (0, b"")
        self.reader = READ_MEMORY(self._read)
        self.table_access = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
        self.module_base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
        self.symbols = {}
        self.pdb = {}

    def timeout_ms(self, now):
        """How long the debugger may wait for an event before the next sample is due."""
        if self.done:
            return 200
        return max(1, min(200, int((self.next - now) * 1000)))

    def poll(self, now, live):
        if self.done or now < self.next:
            return
        self.sample(now, live)
        self.next += self.interval
        if self.next < now:
            self.next = now  # late (a long debug event): no catch-up burst
        if len(self.samples) >= self.count:
            self.write()
            self.samples, self.freeze, self.walk_cost = [], [], []
            if self.repeat > 0:
                self.batch += self.repeat
                self.next = max(self.batch, now)
            else:
                self.done = True

    def _read(self, _process, base, buffer, size, got):
        low, data = self.snapshot
        if low <= base and base + size <= low + len(data):
            ctypes.memmove(buffer, data[base - low:base - low + size], size)
            got[0] = size
            return True
        n = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(self.process, base, buffer, size, ctypes.byref(n))
        got[0] = n.value
        return bool(ok or n.value)

    def thread_info(self, handle):
        text = ""
        try:
            pointer = ctypes.c_void_p()
            if k32.GetThreadDescription(handle, ctypes.byref(pointer)) >= 0 and pointer.value:
                text = ctypes.wstring_at(pointer.value)
                k32.LocalFree(pointer)
        except AttributeError:
            pass
        start = ctypes.c_uint64()
        ntdll.NtQueryInformationThread(handle, 9, ctypes.byref(start), 8, None)  # ThreadQuerySetWin32StartAddress
        return text, start.value

    def sample(self, now, live):
        for tid in [t for t in self.handles if t not in live]:
            k32.CloseHandle(self.handles.pop(tid))
        if not self.session:
            dbghelp.SymSetOptions(0x2 | 0x4 | 0x10 | 0x200 | 0x80000)  # UNDNAME DEFERRED LINES FAIL_CRITICAL NO_PROMPTS
            folders = [self.logdir] + sorted({os.path.dirname(p) for p in self.module_paths.values() if p})
            self.session = bool(dbghelp.SymInitializeW(self.process, ";".join(folders), True))
        elif len(self.samples) == 0:
            dbghelp.SymRefreshModuleList(self.process)  # modules loaded since the last batch
        began = time.perf_counter()
        suspended = []
        for tid in sorted(live):
            handle = self.handles.get(tid)
            if handle is None:
                handle = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)
                if not handle:
                    continue
                self.handles[tid] = handle
                self.info[tid] = self.thread_info(handle)
            if k32.SuspendThread(handle) != 0xFFFFFFFF:
                suspended.append((tid, handle))
        copies = []
        raw = (ctypes.c_ubyte * (1232 + 16))()
        ctx_addr = (ctypes.addressof(raw) + 15) & ~15
        ctx_view = (ctypes.c_ubyte * 1232).from_address(ctx_addr)
        for tid, handle in suspended:
            times = [ctypes.c_uint64() for _ in range(4)]
            k32.GetThreadTimes(handle, *[ctypes.byref(t) for t in times])
            struct.pack_into("<I", ctx_view, 0x30, CONTEXT_CONTROL_INTEGER)
            if not k32.GetThreadContext(handle, ctx_addr):
                continue
            ctx = bytes(ctx_view)
            rsp = struct.unpack_from("<Q", ctx, 0x98)[0]
            info = MBI()
            size = STACK_BYTES
            if k32.VirtualQueryEx(self.process, rsp, ctypes.byref(info), ctypes.sizeof(info)):
                size = max(0, min(size, info.BaseAddress + info.RegionSize - rsp))
            buffer = (ctypes.c_ubyte * max(size, 1))()
            got = ctypes.c_size_t()
            k32.ReadProcessMemory(self.process, rsp, buffer, size, ctypes.byref(got))
            copies.append((tid, handle, times[2].value, times[3].value, ctx, rsp, bytes(buffer[:got.value])))
        for _, handle in suspended:
            k32.ResumeThread(handle)
        resumed = time.perf_counter()
        threads = {}
        for tid, handle, kernel, user, ctx, rsp, stack in copies:
            threads[tid] = (kernel, user, self.walk(handle, ctx, rsp, stack))
        self.freeze.append(resumed - began)
        self.walk_cost.append(time.perf_counter() - resumed)
        self.samples.append((now, threads))

    def walk(self, handle, ctx, rsp, stack):
        raw = (ctypes.c_ubyte * (1232 + 16))()
        ctx_addr = (ctypes.addressof(raw) + 15) & ~15
        ctypes.memmove(ctx_addr, ctx, 1232)
        rip = struct.unpack_from("<Q", ctx, 0xF8)[0]
        if not self.session:
            return (rip,)
        frame = (ctypes.c_ubyte * 512)()
        for offset, value in ((0, rip), (32, struct.unpack_from("<Q", ctx, 0xA0)[0]), (48, rsp)):
            struct.pack_into("<Q", frame, offset, value)
            struct.pack_into("<I", frame, offset + 12, 3)  # AddrModeFlat
        self.snapshot = (rsp, stack)
        frames = []
        for _ in range(FRAMES + 1):
            if not dbghelp.StackWalk64(0x8664, self.process, handle, frame, ctx_addr, self.reader,
                                       self.table_access, self.module_base, None):
                break
            pc = struct.unpack_from("<Q", frame, 0)[0]
            if not pc:
                break
            frames.append(pc)
        return tuple(frames) or (rip,)

    def module_of(self, address):
        name = self.where(address)
        return name.split("+")[0].lower() if "+" in name else ""

    def name(self, address):
        """module+offset, with the function (and line) when dbghelp has a pdb or an export close by."""
        if address in self.symbols:
            return self.symbols[address]
        text = self.where(address)
        module = self.module_of(address)
        if self.session and module:
            info = SYMBOL_INFOW(SizeOfStruct=88, MaxNameLen=511)  # sizeof(SYMBOL_INFOW) with Name[1]; 90 is refused
            displacement = ctypes.c_uint64()
            # Names from a pdb (the stand-ins, source-built probes), and export names in the system
            # modules (the system call stubs); an image without a pdb gets none (an export nearby would mislead).
            if dbghelp.SymFromAddrW(self.process, address, ctypes.byref(displacement), ctypes.byref(info)) \
                    and displacement.value < 0x1000 and (module in SYSTEM or self.has_pdb(info.ModBase)):
                text += f" {info.Name}+{displacement.value:#x}"
                line = IMAGEHLP_LINEW64(SizeOfStruct=ctypes.sizeof(IMAGEHLP_LINEW64))
                d32 = wt.DWORD()
                if dbghelp.SymGetLineFromAddrW64(self.process, address, ctypes.byref(d32), ctypes.byref(line)):
                    text += f" [{os.path.basename(line.FileName)}:{line.LineNumber}]"
        self.symbols[address] = text
        return text

    def has_pdb(self, base):
        """True when dbghelp loaded a pdb for the module at base (IMAGEHLP_MODULEW64.SymType == SymPdb)."""
        if base not in self.pdb:
            info = (ctypes.c_ubyte * 0xCC0)()
            struct.pack_into("<I", info, 0, 0xCC0)
            ok = dbghelp.SymGetModuleInfoW64(self.process, ctypes.c_uint64(base), info)
            self.pdb[base] = bool(ok) and struct.unpack_from("<I", info, 32)[0] == 3
        return self.pdb[base]

    def state(self, frames):
        """('wait', call) / ('syscall', call) / ('run', '') for a sample's leaf."""
        leaf = frames[0]
        if self.module_of(leaf) in ("ntdll.dll", "win32u.dll"):
            label = self.name(leaf)
            call = label.split(" ")[1].split("+")[0] if " " in label else ""
            if call.startswith(("Nt", "Zw")):
                return ("wait" if any(w in call for w in WAIT_WORDS) else "syscall"), call
        return "run", ""

    def site(self, frames):
        """The first two frames outside the system modules: where a thread waits or calls the system."""
        outside = [f for f in frames if self.module_of(f) not in SYSTEM]
        return " < ".join(self.name(f) for f in outside[:2]) or "?"

    def write(self):
        samples = self.samples
        if len(samples) < 2:
            return
        t0, t1 = samples[0][0], samples[-1][0]
        wall = max(t1 - t0, 1e-6)
        dbghelp.SymSetOptions(0x2 | 0x4 | 0x10 | 0x200 | 0x80000)  # the thread dumps reset the options
        if self.session:
            dbghelp.SymRefreshModuleList(self.process)
        tids = sorted({tid for _, threads in samples for tid in threads})
        rows = []
        for tid in tids:
            seen = [threads[tid] for _, threads in samples if tid in threads]
            first = next(threads[tid] for _, threads in samples if tid in threads)
            last = seen[-1]
            kernel, user = (last[0] - first[0]) / 1e4, (last[1] - first[1]) / 1e4  # ms
            rows.append((kernel + user, kernel, user, tid, seen))
        rows.sort(key=lambda r: -r[0])
        ms = sorted(self.freeze)
        out = [f"# profile {t0:.1f}-{t1:.1f} s: {len(samples)} samples, {self.interval * 1000:g} ms apart, "
               f"{len(tids)} threads",
               f"# process frozen per sample: median {ms[len(ms) // 2] * 1000:.1f} ms, max {ms[-1] * 1000:.1f} ms "
               f"(suspend, context, {STACK_BYTES // 1024} KiB stack copy); stack walks afterwards "
               f"{sum(self.walk_cost) / len(self.walk_cost) * 1000:.1f} ms per sample",
               "# a thread whose leaf is a stand-in probe (an access violation) may be blocked reporting a debug event",
               "", f"## (a) CPU per thread between the first and the last sample ({wall:.2f} s wall)",
               "   tid  cpu%  user_ms kernel_ms  run/sys/wait  name / start address / most frequent leaf"]
        total = 0.0
        for cpu, kernel, user, tid, seen in rows:
            total += cpu
            states = [self.state(s[2])[0] for s in seen]
            leaves = {}
            for s in seen:
                leaves[s[2][0]] = leaves.get(s[2][0], 0) + 1
            top = max(leaves.items(), key=lambda kv: kv[1])[0]
            text, begin = self.info.get(tid, ("", 0))
            out.append(f"{tid:6d} {cpu / 10 / wall:5.1f} {user:8.0f} {kernel:9.0f}  "
                       f"{states.count('run'):3d}/{states.count('syscall'):3d}/{states.count('wait'):3d}  "
                       f"{text or '-'} / {self.where(begin) if begin else '-'} / {self.name(top)}")
        out.append(f"total: {total / 1000 / wall:.2f} cores")
        out += ["", "## (b) per thread, busiest first: leaf and inclusive frame histograms, top stacks"]
        for cpu, kernel, user, tid, seen in rows:
            states = [self.state(s[2]) for s in seen]
            if cpu / 10 / wall < 1.0 and all(k == "wait" for k, _ in states):
                continue  # idle waiters: section (c)
            text, begin = self.info.get(tid, ("", 0))
            out.append(f"thread {tid} ({text or '-'}, start {self.where(begin) if begin else '-'}): cpu "
                       f"{cpu / 10 / wall:.1f}%, samples {len(seen)}: " +
                       ", ".join(f"{k}{' ' + c if c else ''} x{n}" for (k, c), n in _count(states)[:6]))
            out.append("  leaf: " + ", ".join(f"{self.name(f)} x{n}" for f, n in _count([s[2][0] for s in seen])[:8]))
            inclusive = _count([f for s in seen for f in set(s[2][1:])])
            out.append("  inclusive: " + ", ".join(f"{self.name(f)} x{n}" for f, n in inclusive[:12]))
            for stack, n in _count([s[2][:6] for s in seen])[:4]:
                out.append(f"  x{n}: " + " < ".join(self.name(f) for f in stack))
        out += ["", "## (c) wait sites (waiting samples: wait call @ first two frames outside the system modules)"]
        for cpu, kernel, user, tid, seen in rows:
            waits = [f"{self.state(s[2])[1]} @ {self.site(s[2])}" for s in seen if self.state(s[2])[0] == "wait"]
            if waits:
                out.append(f"thread {tid}: " + ", ".join(f"{w} x{n}" for w, n in _count(waits)[:3]) +
                           f"  ({len(waits)}/{len(seen)} samples waiting)")
        path = os.path.join(self.logdir, f"profile_{self.batch:g}.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(out) + "\n")
        self.log(f"{t1:7.3f} profile: {len(samples)} samples of {len(tids)} threads -> {os.path.basename(path)} "
                 f"(frozen median {ms[len(ms) // 2] * 1000:.1f} ms per sample)")

    def close(self):
        """At the end of the run: a batch cut short is still written."""
        if not self.done and len(self.samples) >= 2:
            try:
                self.write()
            except Exception as error:  # diagnostics only
                self.log(f"profile: partial batch not written: {error}")
        for handle in self.handles.values():
            k32.CloseHandle(handle)
        self.handles.clear()
        if self.session:
            dbghelp.SymCleanup(self.process)
            self.session = False


def _count(items):
    counts = {}
    for item in items:
        counts[item] = counts.get(item, 0) + 1
    return sorted(counts.items(), key=lambda kv: -kv[1])
