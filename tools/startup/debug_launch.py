"""Run one EXE under a minimal debugger inside a kill-on-close job (startup diagnostics; see docs/startup-runtime.md).

usage: debug_launch.py <exe> <cwd> <logdir> [--timeout S] [--memory-gib N] [--stop-first-chance 1]

Records module loads, every exception (code, address as module+offset, first/second chance) and, for
the stopping exception, the integer registers, the stack words and the code bytes around RIP.
Stops at the first second-chance exception, at a breakpoint inside the main image, or at the timeout.
"""
import bisect
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenThread.restype = wt.HANDLE
k32.CreateJobObjectW.restype = wt.HANDLE
k32.CreateFileW.restype = wt.HANDLE

from launch import STARTUPINFOW, PROCESS_INFORMATION, EXTENDED_LIMIT, inheritable_file  # noqa: E402
import host_cpu  # noqa: E402

EXCEPTION, CREATE_THREAD, CREATE_PROCESS, EXIT_THREAD, EXIT_PROCESS, LOAD_DLL, UNLOAD_DLL, OUTPUT_STRING, RIP = range(1, 10)
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
NAMES = {0x80000003: "breakpoint", 0x80000004: "single step", 0xC0000005: "access violation",
         0xC000001D: "illegal instruction", 0xC0000409: "fast fail", 0xE06D7363: "C++ exception",
         0x406D1388: "thread name", 0xC0000374: "heap corruption", 0x4000001F: "wow breakpoint",
         0xC0000094: "integer divide by zero", 0xC00000FD: "stack overflow", 0x40010006: "debug print"}
REGS = ["Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "Rip"]


def read_qword(read, address):
    data = read(address, 8)
    return int.from_bytes(data, "little") if len(data) == 8 else None


# Breakpoint sites whose first instruction is one of these few forms are not stepped: the
# debugger performs the instruction on the stopped thread's context and continues after it, so
# the int3 stays in place (one debug event per hit instead of a breakpoint, a single step and two
# code writes; and no window in which another thread passes the site unseen). Anything else is
# stepped as before. X2W_DEBUG_EMULATE=0 turns this off.
NOPS = (bytes.fromhex("90"), bytes.fromhex("6690"), bytes.fromhex("0f1f00"), bytes.fromhex("0f1f4000"),
        bytes.fromhex("0f1f440000"), bytes.fromhex("660f1f440000"), bytes.fromhex("0f1f8000000000"),
        bytes.fromhex("0f1f840000000000"))
GPR = 0x78  # CONTEXT.Rax; Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8-R15 follow in encoding order
EFLAGS, RIP = 0x44, 0xF8


def plan_emulation(code):
    """The emulation for the instruction at the start of code, or None when it must be stepped."""
    if len(code) < 8:
        return None
    for nop in NOPS:
        if code.startswith(nop):
            return ("nop", len(nop))
    if code[:3] == b"\x4c\x8b\xd1":                    # mov r10, rcx (every ntdll system-call stub)
        return ("mov", 3, 10, 1, 64)
    if code[:3] == b"\x48\x8b\xc4":                    # mov rax, rsp
        return ("mov", 3, 0, 4, 64)
    if code[0] == 0xC3:                                 # ret
        return ("ret", 1)
    if code[0] == 0xE9:                                 # jmp rel32
        return ("jmp", 5, int.from_bytes(code[1:5], "little", signed=True))
    if code[0] == 0xEB:                                 # jmp rel8
        return ("jmp", 2, int.from_bytes(code[1:2], "little", signed=True))
    # mov [rsp+disp8], r64: REX.W (+R) 89, modrm 01 reg 100, sib 0x24
    if code[0] in (0x48, 0x4C) and code[1] == 0x89 and code[2] & 0xC7 == 0x44 and code[3] == 0x24:
        return ("store", 5, ((code[2] >> 3) & 7) + (8 if code[0] == 0x4C else 0), code[4])
    # test r, r with the same register (32- or 64-bit; optional REX)
    rex = code[0] if 0x40 <= code[0] <= 0x4F else 0
    body = code[1:] if rex else code
    if body[0] == 0x85 and body[1] >> 6 == 3 and (body[1] >> 3) & 7 == body[1] & 7 and (rex & 0x5) in (0, 0x5):
        return ("test", 2 + (1 if rex else 0), (body[1] & 7) + (8 if rex & 1 else 0), 64 if rex & 8 else 32)
    return None


def apply_emulation(plan, address, ctx, read, write):
    """Performs plan on ctx (a writable CONTEXT view) for the instruction at address; False when it cannot."""
    def gpr(n):
        return struct.unpack_from("<Q", ctx, GPR + 8 * n)[0]

    def set_gpr(n, value):
        struct.pack_into("<Q", ctx, GPR + 8 * n, value & 0xFFFFFFFFFFFFFFFF)

    kind, length = plan[0], plan[1]
    rip = address + length
    if kind == "mov":
        _, _, dst, src, width = plan
        set_gpr(dst, gpr(src) if width == 64 else gpr(src) & 0xFFFFFFFF)
    elif kind == "ret":
        target = read_qword(read, gpr(4))
        if target is None:
            return False
        set_gpr(4, gpr(4) + 8)
        rip = target
    elif kind == "jmp":
        rip = address + length + plan[2]
    elif kind == "store":
        if not write(gpr(4) + plan[3], struct.pack("<Q", gpr(plan[2]))):
            return False
    elif kind == "test":
        value = gpr(plan[2]) & (0xFFFFFFFFFFFFFFFF if plan[3] == 64 else 0xFFFFFFFF)
        flags = struct.unpack_from("<I", ctx, EFLAGS)[0] & ~(0x1 | 0x4 | 0x40 | 0x80 | 0x800)  # CF PF ZF SF OF
        if bin(value & 0xFF).count("1") % 2 == 0:
            flags |= 0x4
        if value == 0:
            flags |= 0x40
        if value >> (plan[3] - 1):
            flags |= 0x80
        struct.pack_into("<I", ctx, EFLAGS, flags)  # AF is undefined after TEST and is left as it was
    struct.pack_into("<Q", ctx, RIP, rip)
    return True


def final_path(handle, full=False):
    buf = ctypes.create_unicode_buffer(1024)
    n = k32.GetFinalPathNameByHandleW(wt.HANDLE(handle), buf, 1024, 0)
    if full:
        return buf.value[4:] if n and buf.value.startswith("\\\\?\\") else (buf.value if n else "")
    return os.path.basename(buf.value) if n else "?"


def main():
    exe, cwd, logdir = (os.path.abspath(a) for a in sys.argv[1:4])
    opts = dict(zip(sys.argv[4::2], sys.argv[5::2]))
    # Everything printed below is also kept for finished.txt, which the live console shows at the end.
    import builtins
    summary = []

    def print(*args, **kwargs):
        summary.append(" ".join(str(a) for a in args))
        builtins.print(*args, **kwargs)
    timeout = float(opts.get("--timeout", 15))
    memory = int(float(opts.get("--memory-gib", 16)) * 2**30)
    stop_first = opts.get("--stop-first-chance") == "1"
    skip_asserts = int(opts.get("--continue-asserts", "0"))  # main-image int3s to log and continue past
    asserts_seen = {}
    # X2W_HOST_CPU=1: rewrite main-image instructions the host processor lacks (host_cpu.py).
    host_cpu_on = os.environ.get("X2W_HOST_CPU") == "1"
    host_cpu_sites = {}
    flushopt_missing = [False]  # set when a CLFLUSHOPT faults: CLWB then becomes a NOP instead
    stub_area = [0, 0]  # next free address and end of the block the stubs go to
    os.makedirs(logdir, exist_ok=True)
    trap_log = os.path.join(logdir, "trap.log")
    stop_file = os.path.join(logdir, "stop")  # created from outside: end the run as at a timeout
    snapshot_file = os.path.join(logdir, "snapshot")  # created from outside: write threads_<s>.txt, go on
    for stale in (trap_log, os.path.join(logdir, "finished.txt"), stop_file, snapshot_file):  # a reused run directory starts clean
        if os.path.exists(stale):
            os.remove(stale)
    if os.path.exists(trap_log):
        os.remove(trap_log)
    events = open(os.path.join(logdir, "events.txt"), "w", encoding="utf-8")
    # The logs are written through Python's buffer and flushed at most every half second (the
    # live console tails them), at a stop and at the end, instead of once per line.
    # X2W_DEBUG_FLUSH_EACH=1 restores a flush per line.
    flush_each = os.environ.get("X2W_DEBUG_FLUSH_EACH") == "1"
    open_logs = [events]
    last_flush = [time.time()]

    def flush_logs(force=False):
        now = time.time()
        if force or flush_each or now - last_flush[0] >= 0.5:
            last_flush[0] = now
            for handle in open_logs:
                if not handle.closed:
                    handle.flush()

    def log(text):
        events.write(text + "\n")
        if flush_each:
            events.flush()

    # --log-repeats N: an exception, continued assert or diagnostic hook (runtime API, movie
    # player) that repeats from the same place is logged N times, then only counted; the counts
    # go to finished.txt. 0 logs every one, as before. File and network hooks are never capped.
    log_repeats = int(opts.get("--log-repeats", "200"))
    repeat_counts = {}

    def repeat_ok(key):
        """True while key has been seen at most --log-repeats times (always true with 0)."""
        seen = repeat_counts.get(key, 0) + 1
        repeat_counts[key] = seen
        return not log_repeats or seen <= log_repeats
    emulate_on = os.environ.get("X2W_DEBUG_EMULATE", "1") != "0"
    fast_exit = opts.get("--fast-exit") == "1"

    # LOCAL (out/gamerun/memleak_work/NOTES.md): a pagefile section opened as a D3D12 heap keeps its commit after the
    # title is gone, until reboot. With file-backed slabs, the slabs xmem moves for the GPU come from a long-lived pool
    # (gpu_slab_pool.py) so that every run reuses the same sections. Already-charged pool bytes are taken off the
    # slab budget in the guard below. X2W_XMEM_GPU_POOL=0: off.
    # X2W_XMEM_GPU_POOL=all: EVERY slab comes from the pool from the start (no file-backed slabs, no live moves);
    # X2W_XMEM_GPU_POOL_SLABS sections (default 32 = 8 GiB; runs 89-91 used 28), charged once per boot. The slab budget then only
    # covers fresh sections beyond the pool.
    pool_mode = os.environ.get("X2W_XMEM_GPU_POOL", "")
    pool_all = pool_mode == "all"
    pooled_bytes = 0
    if pool_all or (os.environ.get("X2W_XMEM_SLAB_DIR") and pool_mode != "0"):
        try:  # a pool failure must never cost a run: xmem then falls back to fresh sections
            import gpu_slab_pool
            count = (int(os.environ.get("X2W_XMEM_GPU_POOL_SLABS") or "32") if pool_all
                     else max(1, int(os.environ.get("X2W_XMEM_SLAB_BUDGET_MIB") or "3072") // 256))
            pooled_bytes = gpu_slab_pool.ensure(count) << 28
        except Exception as error:
            count = 0
            print(f"GPU slab pool not started: {error}")
        print(f"GPU slab pool: {pooled_bytes >> 20} MiB reachable (X2W_XMEM_GPU_POOL={pool_mode or 'on'})")
        if pool_all and pooled_bytes < count << 28:
            # =all is an explicit opt-in: a short pool would push the title onto the (small) beyond-pool budget and
            # make its page maps fail (err 1455) mid-run. Refuse instead.
            print(f"refused to launch: X2W_XMEM_GPU_POOL=all needs {count} pool sections, {pooled_bytes >> 28} reachable")
            return 3
    # Host safety (runtime_work/NOTES.md): the job limit covers private commit only. Pagefile page
    # slabs are capped by X2W_XMEM_SLAB_BUDGET_MIB; a launch whose job limit plus slab budget does not
    # fit in the host's commit headroom minus 2 GiB is refused. X2W_ALLOW_UNBUDGETED=1 skips this.
    if os.environ.get("X2W_XMEM_PERMISSIVE") == "1" and os.environ.get("X2W_ALLOW_UNBUDGETED") != "1":  # title runs (run.ps1)
        class _PI(ctypes.Structure):
            _fields_ = [("cb", wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
                "CommitTotal", "CommitLimit", "CommitPeak", "PhysicalTotal", "PhysicalAvailable", "SystemCache",
                "KernelTotal", "KernelPaged", "KernelNonpaged", "PageSize")] + [(n, wt.DWORD) for n in (
                "HandleCount", "ProcessCount", "ThreadCount")]
        info0 = _PI()
        info0.cb = ctypes.sizeof(info0)
        ctypes.WinDLL("psapi").GetPerformanceInfo(ctypes.byref(info0), info0.cb)
        headroom = (info0.CommitLimit - info0.CommitTotal) * info0.PageSize
        budget = int(os.environ.get("X2W_XMEM_SLAB_BUDGET_MIB") or "0") << 20
        beyond_pool = budget if pool_all else max(0, budget - pooled_bytes)  # the pool is already in CommitTotal
        if not budget or memory + beyond_pool > headroom - (2 << 30):
            print(f"refused to launch: job limit {memory >> 20} MiB + pagefile slab budget {budget >> 20} MiB "
                  f"(X2W_XMEM_SLAB_BUDGET_MIB) must be set and fit in the commit headroom {headroom >> 20} MiB - 2048")
            return 3
    k32.SetErrorMode(0x8003)
    job = k32.CreateJobObjectW(None, None)
    info = EXTENDED_LIMIT()
    info.Basic.LimitFlags = 0x2000 | 0x200 | 0x8
    info.Basic.ActiveProcessLimit = 1
    info.JobMemoryLimit = memory
    k32.SetInformationJobObject(wt.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info))

    out = inheritable_file(os.path.join(logdir, "stdout.txt"))
    err = inheritable_file(os.path.join(logdir, "stderr.txt"))
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = 0x100
    si.hStdOutput, si.hStdError = out, err
    env = dict(os.environ, X2W_TRAP_LOG=trap_log)
    # A process started under a debugger gets the debug heap: no low-fragmentation heap and checks on
    # every allocation and free. That multiplies the cost of each free, and a startup run frees a lot.
    # _NO_DEBUG_HEAP=1 keeps the ordinary heap; a caller who wants the checks sets _NO_DEBUG_HEAP=0.
    env.setdefault("_NO_DEBUG_HEAP", "1")
    block = "".join(f"{k}={v}\0" for k, v in env.items()) + "\0"
    pi = PROCESS_INFORMATION()
    flags = 0x2 | 0x4 | 0x400  # DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT
    if not k32.CreateProcessW(exe, ctypes.create_unicode_buffer(f'"{exe}"'), None, None, True, flags,
                              ctypes.create_unicode_buffer(block), cwd, ctypes.byref(si), ctypes.byref(pi)):
        print("CreateProcess failed", ctypes.get_last_error())
        return 2
    k32.AssignProcessToJobObject(wt.HANDLE(job), wt.HANDLE(pi.hProcess))
    k32.DebugSetProcessKillOnExit(True)
    k32.ResumeThread(wt.HANDLE(pi.hThread))
    if opts.get("--console", "1") == "1":
        # Live debug console in its own window: follows this run's logs (live_view.py).
        import subprocess
        try:
            subprocess.Popen([sys.executable, "-B", os.path.join(os.path.dirname(os.path.abspath(__file__)), "live_view.py"),
                              logdir, cwd], creationflags=0x00000010)  # CREATE_NEW_CONSOLE
        except OSError as error:
            builtins.print(f"live console not started: {error}")

    modules = {}  # base -> name
    module_paths = {}  # base -> full path (symbol search for the sampling profiler)
    main_base = None
    event = (ctypes.c_ubyte * 176)()
    start = time.time()
    loader_break_seen = False
    stop = None
    exit_code = None

    module_index = [[], 0]  # sorted bases, and the module count they were built from

    def where(addr):
        if module_index[1] != len(modules) or len(module_index[0]) != len(modules):
            module_index[0] = sorted(modules)
            module_index[1] = len(modules)
        bases = module_index[0]
        i = bisect.bisect_right(bases, addr) - 1
        if i >= 0 and addr - bases[i] < 0x20000000:
            return f"{modules[bases[i]]}+{addr - bases[i]:#x}"
        return f"{addr:#x}"

    def read(addr, size):
        buf = (ctypes.c_ubyte * size)()
        got = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(wt.HANDLE(pi.hProcess), ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
        return bytes(buf[:got.value]) if ok or got.value else b""

    def write(addr, data):
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        old = wt.DWORD()
        k32.VirtualProtectEx(wt.HANDLE(pi.hProcess), ctypes.c_void_p(addr), len(data), 0x40, ctypes.byref(old))
        ok = k32.WriteProcessMemory(wt.HANDLE(pi.hProcess), ctypes.c_void_p(addr), buf, len(data), None)
        k32.VirtualProtectEx(wt.HANDLE(pi.hProcess), ctypes.c_void_p(addr), len(data), old.value, ctypes.byref(old))
        k32.FlushInstructionCache(wt.HANDLE(pi.hProcess), ctypes.c_void_p(addr), len(data))
        return ok

    def rewrite_for_host(address):
        """Rewrites the instruction at a faulting main-image address; returns its name, or None."""
        code = read(address, 48)
        clwb = host_cpu.clwb_rewrite(code)
        if clwb and not flushopt_missing[0]:
            return "clwb" if write(address + clwb[0], clwb[1]) else None
        nop = host_cpu.flush_nop(code)
        if nop:
            # CLFLUSHOPT itself faulted (a host older than it), or a CLWB after that was seen.
            flushopt_missing[0] = True
            return ("clwb-nop" if clwb else "clflushopt-nop") if write(address, nop) else None
        wait = host_cpu.wait_rewrite(code)
        if wait:
            return ("mwaitx" if code[2] == 0xFB else "monitorx") if write(address, wait) else None
        store = host_cpu.nt_store_rewrite(code)
        if store:
            return ("movntss" if code[0] == 0xF3 else "movntsd") if write(address + store[0], store[1]) else None
        rdpru = host_cpu.rdpru_rewrite(code)
        if rdpru:
            return "rdpru" if write(address, rdpru) else None
        found = host_cpu.stub_for(code)
        if not found:
            return None
        length, stub = found
        if stub_area[0] + len(stub) + 64 > stub_area[1]:
            # A block within rel32 reach of the image: the first free 64 KiB below its base.
            k32.VirtualAllocEx.restype = ctypes.c_void_p
            for step in range(1, 4096):
                block = k32.VirtualAllocEx(wt.HANDLE(pi.hProcess), ctypes.c_void_p(main_base - step * 0x10000),
                                           0x10000, 0x3000, 0x40)
                if block:
                    stub_area[:] = [block, block + 0x10000]
                    break
            else:
                return None
        try:
            plan = host_cpu.plan_replacement(code, address, length, stub, stub_area[0])
        except Exception as error:  # iced-x86 missing, or an instruction it cannot move
            log(f"{time.time() - start:7.3f} host-cpu no replacement at {where(address)}: {error}")
            plan = None
        if not plan or not write(stub_area[0], plan[0]) or not write(address, plan[1]):
            return None
        stub_area[0] += (len(plan[0]) + 15) & ~15
        if code[:3] == host_cpu.CLZERO:
            return "clzero"
        if host_cpu.rdpid_stub(code):
            return "rdpid"
        return "insertq" if code[0] == 0xF2 else "extrq"

    def sha_for_host(tid, address):
        """Performs a SHA instruction on the stopped thread's context; True when it did."""
        thread, ctx = get_ctx(tid)
        if not thread:
            return False

        def gpr(n):
            return struct.unpack_from("<Q", ctx, 0x78 + 8 * n)[0]

        def xmm(n):
            return int.from_bytes(bytes(ctx[0x1A0 + 16 * n:0x1B0 + 16 * n]), "little")

        try:
            done = host_cpu.sha_apply(read(address, 15), address, gpr, xmm, read)
        except ImportError:
            done = None
        if not done:
            release(thread)
            return False
        length, dest, value = done
        struct.pack_into("<16s", ctx, 0x1A0 + 16 * dest, value.to_bytes(16, "little"))
        struct.pack_into("<Q", ctx, 0xF8, address + length)
        set_ctx(thread)
        return True

    ctx_raw = (ctypes.c_ubyte * (1232 + 16))()
    ctx_addr = (ctypes.addressof(ctx_raw) + 15) & ~15
    ctx_view = (ctypes.c_ubyte * 1232).from_address(ctx_addr)

    # Thread handles from the CREATE_PROCESS and CREATE_THREAD events (they carry get/set context
    # access and stay valid until the thread's exit event), so a breakpoint needs no OpenThread.
    thread_handles = {}
    opened = set()

    def get_ctx(tid, flags=0x10000B):
        """The thread's context: flags 0x100003 (control, integer) for breakpoints, 0x10000B adds FP state."""
        thread = thread_handles.get(tid)
        if not thread:
            thread = k32.OpenThread(0x0008 | 0x0010 | 0x0002, False, tid)  # GET | SET_CONTEXT | SUSPEND
            if thread:
                opened.add(thread)
        struct.pack_into("<I", ctx_view, 0x30, flags)
        if not thread or not k32.GetThreadContext(wt.HANDLE(thread), ctypes.c_void_p(ctx_addr)):
            release(thread)
            return None, None
        return thread, ctx_view

    def release(thread):
        """Closes a handle get_ctx opened itself; the event-owned handles are left to the system."""
        if thread in opened:
            opened.discard(thread)
            k32.CloseHandle(wt.HANDLE(thread))

    def set_ctx(thread):
        k32.SetThreadContext(wt.HANDLE(thread), ctypes.c_void_p(ctx_addr))
        release(thread)

    def ustr(addr):  # UNICODE_STRING*
        raw = read(addr, 16)
        if len(raw) < 16:
            return "?"
        length, _, buffer = struct.unpack_from("<HH4xQ", raw)
        return read(buffer, length).decode("utf-16-le", errors="replace")

    def oa_name(addr):  # OBJECT_ATTRIBUTES*
        raw = read(addr, 24)
        if len(raw) < 24:
            return "?"
        root, name = struct.unpack_from("<8xQQ", raw)
        text = ustr(name) if name else ""
        return (f"[root {root:#x}]" if root else "") + text

    trace = opts.get("--trace-files") == "1" or opts.get("--offline", "1") == "1"  # offline mode needs the hooks
    k32.GetModuleHandleW.restype = wt.HMODULE
    ntdll = k32.GetModuleHandleW("ntdll.dll")
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_char_p]
    hooks = {}
    if trace:
        for name, arg in (("NtCreateFile", "r8"), ("NtOpenFile", "r8"), ("NtQueryAttributesFile", "rcx"),
                          ("NtQueryFullAttributesFile", "rcx"), ("NtOpenDirectoryObject", "r8"),
                          ("NtOpenSymbolicLinkObject", "r8"), ("NtAlpcConnectPort", "ustr:rdx"),
                          ("NtAlpcConnectPortEx", "rdx")):
            addr = k32.GetProcAddress(ntdll, name.encode())
            if addr:
                hooks[addr] = (name, arg)
    # X2W_VALLOC_MIN=<hex bytes>: log every virtual-memory allocation of at least that size with the
    # main-image and stand-in return addresses found on the caller's stack (files.txt, "VALLOC").
    # The call is logged on entry: "requested" is the address the caller asked for (0 for any), not
    # the address it got.
    valloc_min = int(os.environ.get("X2W_VALLOC_MIN") or "0", 16)
    if valloc_min and trace:
        for name in ("NtAllocateVirtualMemory", "NtAllocateVirtualMemoryEx"):
            addr = k32.GetProcAddress(ntdll, name.encode())
            if addr:
                hooks[addr] = (name, "valloc")
    bink_counts, bink_state = {}, {}
    hit_counts = {}
    redirects = {}
    # Offline by default (--offline 0 turns it off): no sockets, NetBIOS name devices or DNS resolver.
    offline = opts.get("--offline", "1") == "1"
    OFFLINE_DENY = ("\\device\\afd", "\\device\\netbt", "dnsresolver", "\\device\\tcp", "\\device\\udp",
                    "\\device\\rawip", "\\device\\ws2ifsl")
    denied = {}

    # X2W_BINK_PER_HANDLE=1: per movie handle, every NextFrame count 1-3 and each 100th, only pause
    # changes (not the per-frame repeats), and a frame count on close and in the exit report.
    bink_per_handle = os.environ.get("X2W_BINK_PER_HANDLE") == "1"
    bink_handles = {}  # handle -> {"frames", "paused", "opened"}

    def bink_call(name, regs, ret):
        """Counts the call, remembers the movie handle and frame buffers; returns a log line or None."""
        bink_counts[name] = bink_counts.get(name, 0) + 1
        if name == "BinkRegisterFrameBuffers":
            bink_state["bink"], bink_state["buffers"] = regs["Rcx"], regs["Rdx"]
        if name == "BinkNextFrame":
            bink_state["bink"] = regs["Rcx"]
        if bink_per_handle and name != "BinkOpenWithOptions":
            movie = bink_handles.setdefault(regs["Rcx"], {"frames": 0, "paused": None, "opened": False})
            if name == "BinkNextFrame":
                movie["frames"] += 1
                if movie["frames"] > 3 and movie["frames"] % 100:
                    return None
                return f"BinkNextFrame rcx={regs['Rcx']:#x} handle frame call {movie['frames']} from {where(ret)}"
            if name == "BinkPause":
                paused = regs["Rdx"] != 0
                if movie["paused"] == paused:
                    return None
                movie["paused"] = paused
                return (f"BinkPause rcx={regs['Rcx']:#x} -> {'paused' if paused else 'playing'} after "
                        f"{movie['frames']} NextFrame calls from {where(ret)}")
            if name == "BinkClose":
                frames = movie["frames"]
                bink_handles.pop(regs["Rcx"], None)
                return f"BinkClose rcx={regs['Rcx']:#x} after {frames} NextFrame calls from {where(ret)}"
        if name in ("BinkNextFrame", "BinkShouldSkip") and bink_counts[name] > 3:
            return None
        text = ""
        if name == "BinkOpenWithOptions":
            text = repr((read(regs["Rcx"], 200) or b"").split(bytes(1))[0].decode("latin-1", "replace"))
        return f"{name} rcx={regs['Rcx']:#x} rdx={regs['Rdx']:#x} r8={regs['R8']:#x} {text} from {where(ret)}"

    def bink_report(luma=True):
        """Call counts, the movie header and (luma=True) a PNG of the current frame's luma plane."""
        if not bink_counts:
            return
        print("  bink calls: " + ", ".join(f"{k} x{v}" for k, v in sorted(bink_counts.items())))
        for handle, movie in sorted(bink_handles.items()):
            print(f"  bink handle {handle:#x} still open: {movie['frames']} NextFrame calls, paused={movie['paused']}")
        header = read(bink_state["bink"], 32) if bink_state.get("bink") else None
        if header:
            print("  bink header (width, height, frames, frame, ...): " + " ".join(str(v) for v in struct.unpack("<8I", header)))
        buffers = read(bink_state["buffers"], 24 + 2 * 5 * 24) if bink_state.get("buffers") else None
        if not buffers:
            return
        total, ya_w, ya_h, c_w, c_h, current = struct.unpack_from("<iIIIII", buffers, 0)
        print(f"  bink frame buffers: total={total} luma={ya_w}x{ya_h} chroma={c_w}x{c_h} current={current}")
        if not luma or not (0 < ya_w <= 8192 and 0 < ya_h <= 8192):
            return
        plane = 24 + (current % 2) * 5 * 24  # header, then two plane sets of five 24-byte planes; luma first
        pointer = struct.unpack_from("<Q", buffers, plane + 8)[0]
        pitch = struct.unpack_from("<I", buffers, plane + 16)[0]
        rows = []
        for y in range(ya_h):
            row = read(pointer + y * pitch, ya_w)
            if not row:
                break
            rows.append(bytes(1) + row)
        if len(rows) != ya_h:
            print(f"  bink luma plane unreadable at {pointer:#x} pitch {pitch}")
            return
        import zlib

        def chunk(kind, payload):
            body = kind + payload
            return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

        signature = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
        png = (signature + chunk(b"IHDR", struct.pack(">IIBBBBB", ya_w, ya_h, 8, 0, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
        with open(os.path.join(logdir, "bink_luma.png"), "wb") as handle:
            handle.write(png)
        print(f"  bink luma plane -> bink_luma.png (pointer {pointer:#x}, pitch {pitch})")

    breakpoints = {}  # addr -> original byte
    emulations = {}  # addr -> plan_emulation() result for breakpoints that are not stepped
    returns = {}  # addr -> list of (tid, rsp after return, description)
    stepping = {}  # tid -> addr to re-arm
    capped = {}  # diagnostic hook -> calls not logged because of --log-repeats
    trace_callers = "CALLERS" in os.environ.get("X2W_TRACE", "")
    ever_armed = set()  # addresses that have held one of our int3s
    file_log = open(os.path.join(logdir, "files.txt"), "w", encoding="utf-8") if trace else None
    if file_log:
        open_logs.append(file_log)

    def arm(addr):
        if addr in breakpoints:
            return
        original = read(addr, 16)
        if original and write(addr, b"\xcc"):
            breakpoints[addr] = original[:1]
            ever_armed.add(addr)
            plan = plan_emulation(original) if emulate_on else None
            if plan:
                emulations[addr] = plan

    def disarm(addr):
        """Removes a return-site breakpoint that nothing waits for and that cannot be emulated."""
        if write(addr, breakpoints[addr]):
            del breakpoints[addr]
            emulations.pop(addr, None)

    def handle_breakpoint(tid, addr):
        thread, ctx = get_ctx(tid, 0x100003)
        if not thread:
            return False
        regs = dict(zip(REGS, struct.unpack_from("<16Q", ctx, 0x78) + struct.unpack_from("<Q", ctx, 0xF8)))
        rsp = regs["Rsp"]
        if addr in hooks:
            name, arg = hooks[addr]
            ret = read_qword(read, rsp) or 0
            if arg == "valloc":
                # NtAllocateVirtualMemory(process, &base, zero bits, &size, type, protect) and
                # NtAllocateVirtualMemoryEx(process, &base, &size, type, protect, ...).
                extended = name.endswith("Ex")
                size = read_qword(read, regs["R8" if extended else "R9"])
                if size is not None and size >= valloc_min:
                    base = read_qword(read, regs["Rdx"])
                    requested = "unreadable" if base is None else hex(base)
                    stack_args = struct.unpack("<2Q", (read(rsp + 0x28, 16) or b"").ljust(16, b"\0"))
                    kind, protect = (regs["R9"], stack_args[0]) if extended else stack_args
                    words = struct.unpack_from("<200Q", (read(rsp, 8 * 200) or b"").ljust(8 * 200, b"\0"))
                    chain = [where(w) for w in words if any(b <= w < b + 0x10000000 for b in modules)]
                    chain = [c for c in chain if not c.lower().startswith(("ntdll", "kernelbase", "kernel32"))]
                    file_log.write(f"{time.time() - start:7.3f} VALLOC {name} requested={requested} size={size:#x} "
                                   f"type={kind & 0xffffffff:#x} protect={protect & 0xffffffff:#x} callers: {' '.join(chain[:10])}\n")
                desc = None
            elif arg == "count":
                # Title function hit counter: first hits are logged with registers and the caller.
                hit_counts[name] = hit_counts.get(name, 0) + 1
                desc = None
                if name in redirects and hit_counts[name] == redirects[name][0]:
                    target = redirects[name][1]
                    file_log.write(f"{time.time() - start:7.3f} REDIRECT {name} hit {hit_counts[name]} -> {where(target)} "
                                   f"(experiment)\n")
                    struct.pack_into("<Q", ctx, 0xF8, target)
                    set_ctx(thread)
                    return True
                if hit_counts[name] <= 12:
                    file_log.write(f"{time.time() - start:7.3f} HIT {name} rcx={regs['Rcx']:#x} rdx={regs['Rdx']:#x} "
                                   f"r8={regs['R8']:#x} r9={regs['R9']:#x} [rdx]={(read(regs['Rdx'], 32) or b'').hex(' ')} "
                                   f"from {where(ret)}\n")
            elif arg == "runtime":
                def guid(address):
                    raw = read(address, 16) if address else None
                    if not raw:
                        return f"{address:#x}"
                    a, b, c = struct.unpack_from("<IHH", raw)
                    return f"{a:08x}-{b:04x}-{c:04x}-{raw[8:10].hex()}-{raw[10:].hex()}"
                desc = f"{name} " + (f"provider={guid(regs['Rcx'])} iid={guid(regs['Rdx'])}" if name == "QueryApiImpl"
                                      else f"rcx={regs['Rcx']:#x} rdx={regs['Rdx']:#x}") + f" from {where(ret)}"
                if not repeat_ok(desc):
                    capped[name] = capped.get(name, 0) + 1
                    desc = None
            elif arg == "bink":
                desc = bink_call(name, regs, ret)
                if desc and not repeat_ok(f"{name} from {where(ret)}"):
                    capped[name] = capped.get(name, 0) + 1
                    desc = None
            else:
                if arg.startswith("ustr:"):
                    target = ustr(regs[arg[5:].capitalize()])
                else:
                    target = oa_name(regs[arg.capitalize()] if arg != 'r8' else regs['R8'])
                desc = f"{name} {target} from {where(ret)}"
                if offline and any(pattern in target.lower() for pattern in OFFLINE_DENY):
                    # Offline mode: the call never runs. Return STATUS_ACCESS_DENIED to the caller.
                    denied[target] = denied.get(target, 0) + 1
                    file_log.write(f"{time.time() - start:7.3f} DENIED (offline) {desc}\n")
                    file_log.flush()  # a refusal is written at once
                    struct.pack_into("<Q", ctx, 0x78, 0xC0000022)   # rax
                    struct.pack_into("<Q", ctx, 0x98, rsp + 8)      # rsp: pop the return address
                    struct.pack_into("<Q", ctx, 0xF8, ret)          # rip
                    set_ctx(thread)
                    return True
            if desc and ("memory:" in desc or trace_callers):
                # Title-level callers: module addresses on the stack outside the system DLLs.
                words = struct.unpack_from("<160Q", (read(rsp, 8 * 160) or b"").ljust(8 * 160, b"\0"))
                chain = [where(w) for w in words if any(b <= w < b + 0x10000000 for b in modules)]
                chain = [c for c in chain if not c.lower().startswith(("ntdll", "kernelbase", "kernel32"))]
                desc += " callers: " + " ".join(chain[:8])
            if desc:
                returns.setdefault(ret, []).append((tid, rsp + 8, desc))
                arm(ret)
        pending = returns.get(addr)
        if pending:
            for item in list(pending):
                if item[0] == tid and item[1] == rsp:
                    pending.remove(item)
                    file_log.write(f"{time.time() - start:7.3f} {item[2]} -> {regs['Rax'] & 0xFFFFFFFF:#010x}\n")
        plan = emulations.get(addr)
        if plan and apply_emulation(plan, addr, ctx, read, write):
            # Performed here; the int3 stays in place.
            set_ctx(thread)
            return True
        if addr not in hooks and not returns.get(addr):
            # A return site nothing waits for: continue at the original instruction without a step.
            struct.pack_into("<Q", ctx, 0xF8, addr)
            set_ctx(thread)
            disarm(addr)
            return True
        # Step over the original instruction, then re-arm.
        struct.pack_into("<Q", ctx, 0xF8, addr)
        eflags = struct.unpack_from("<I", ctx, 0x44)[0]
        struct.pack_into("<I", ctx, 0x44, eflags | 0x100)
        set_ctx(thread)
        write(addr, breakpoints[addr])
        stepping[tid] = addr
        return True

    live_threads = set()

    def dump_threads(path, walk=True):
        """At a timeout: every thread's rip and the module addresses found on its stack.

        walk=False (--fast-exit) writes the rip of each thread only, in the same line shape."""
        lines = []
        dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
        dbghelp.SymSetOptions(0x00000004 | 0x00000200)  # DEFERRED_LOADS | FAIL_CRITICAL_ERRORS
        # Frames are printed as module+offset; no symbols are used. The search path is the log
        # folder so that dbghelp does not look for symbol files (or ask a symbol server) per module.
        if walk:
            dbghelp.SymInitializeW(wt.HANDLE(pi.hProcess), ctypes.c_wchar_p(logdir), True)
        dbghelp.StackWalk64.argtypes = [ctypes.c_uint32, wt.HANDLE, wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        table_access = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
        module_base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
        for thread_id in sorted(live_threads):
            thread = k32.OpenThread(0x0008 | 0x0002 | 0x0040, False, thread_id)
            if not thread:
                continue
            k32.SuspendThread(wt.HANDLE(thread))
            raw = (ctypes.c_ubyte * (1232 + 16))()
            ctx_addr = (ctypes.addressof(raw) + 15) & ~15
            struct.pack_into("<I", (ctypes.c_ubyte * 1232).from_address(ctx_addr), 0x30, 0x10000B)
            if k32.GetThreadContext(wt.HANDLE(thread), ctypes.c_void_p(ctx_addr)):
                ctx = bytes((ctypes.c_ubyte * 1232).from_address(ctx_addr))
                rsp = struct.unpack_from("<Q", ctx, 0x98)[0]
                rbp = struct.unpack_from("<Q", ctx, 0xA0)[0]
                rip = struct.unpack_from("<Q", ctx, 0xF8)[0]
                # Real unwinding through the modules' unwind tables (dbghelp StackWalk64).
                frame = (ctypes.c_ubyte * 512)()
                for offset, value in ((0, rip), (32, rbp), (48, rsp)):
                    struct.pack_into("<Q", frame, offset, value)
                    struct.pack_into("<I", frame, offset + 12, 3)  # AddrModeFlat
                frames = []
                for _ in range(28 if walk else 0):
                    if not dbghelp.StackWalk64(0x8664, wt.HANDLE(pi.hProcess), wt.HANDLE(thread), frame,
                                               ctypes.c_void_p(ctx_addr), None, table_access, module_base, None):
                        break
                    pc = struct.unpack_from("<Q", frame, 0)[0]
                    if not pc:
                        break
                    frames.append(where(pc))
                lines.append(f"thread {thread_id} rip={where(rip)} stack: " + " ".join(frames[1:]))
            k32.ResumeThread(wt.HANDLE(thread))
            k32.CloseHandle(wt.HANDLE(thread))
        if walk:
            dbghelp.SymCleanup(wt.HANDLE(pi.hProcess))
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        return lines

    watches = [(int(r, 16), int(n or "8", 16)) for r, _, n in
               (item.partition(":") for item in filter(None, os.environ.get("X2W_WATCH", "").split(",")))]
    watch_values, watch_next = {}, 0.0
    # X2W_DUMP_AT=<seconds>[,...]: write every thread's stack to threads_<seconds>.txt at those times,
    # for a run that stalls before a watchdog or a timeout ends it.
    dump_at = sorted(float(t) for t in filter(None, os.environ.get("X2W_DUMP_AT", "").split(",")))
    # X2W_STOP_AT=<rva>[,...]: stop at a main-image breakpoint at one of these addresses even while
    # --continue-asserts would step over it, and write threads_stop.txt.
    stop_at = {int(r, 16) for r in filter(None, os.environ.get("X2W_STOP_AT", "").split(","))}

    def poll_watches():
        for rva, size in watches:
            blob = read(main_base + rva, size) if main_base else None
            if blob is not None and watch_values.get(rva) != blob:
                watch_values[rva] = blob
                log(f"{time.time() - start:7.3f} watch +{rva:#x} = {blob.hex(' ')}")

    # Host safety (runtime_work/NOTES.md): the job memory limit counts private commit only, so the
    # title's page slabs (pagefile-backed sections) and driver allocations are not limited by it.
    # Every second the system commit headroom is read; below X2W_HEADROOM_STOP_MIB (default 1536)
    # the run ends as at a timeout. commit.txt gets a sample every 2 s: seconds, system committed,
    # commit limit, the title's private commit, this debugger's private commit (MiB).
    class PERF_INFO(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
            "CommitTotal", "CommitLimit", "CommitPeak", "PhysicalTotal", "PhysicalAvailable", "SystemCache",
            "KernelTotal", "KernelPaged", "KernelNonpaged", "PageSize")] + [(n, wt.DWORD) for n in (
            "HandleCount", "ProcessCount", "ThreadCount")]

    class PROC_MEM(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
            "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
    psapi = ctypes.WinDLL("psapi")
    psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD]
    k32.GetCurrentProcess.restype = wt.HANDLE
    headroom_stop_mib = int(os.environ.get("X2W_HEADROOM_STOP_MIB") or "3072")
    headroom_stop = None
    commit_log = open(os.path.join(logdir, "commit.txt"), "w", encoding="utf-8")
    commit_log.write("# seconds system_committed_MiB commit_limit_MiB title_private_MiB debugger_private_MiB\n")
    open_logs.append(commit_log)
    commit_next = [0.0, 0.0]  # next check, next commit.txt line

    def private_mib(handle):
        counters = PROC_MEM()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return -1
        return counters.PagefileUsage >> 20

    def commit_check():
        """Samples the commit; returns (committed, limit) MiB when the headroom is below the stop."""
        now = time.time()
        if now < commit_next[0]:
            return None
        commit_next[0] = now + 0.1
        perf = PERF_INFO()
        perf.cb = ctypes.sizeof(perf)
        if not psapi.GetPerformanceInfo(ctypes.byref(perf), perf.cb):
            return None
        committed, limit = (perf.CommitTotal * perf.PageSize) >> 20, (perf.CommitLimit * perf.PageSize) >> 20
        if now >= commit_next[1]:
            commit_next[1] = now + 2.0
            commit_log.write(f"{now - start:7.1f} {committed} {limit} {private_mib(wt.HANDLE(pi.hProcess))} "
                             f"{private_mib(k32.GetCurrentProcess())}\n")
        return (committed, limit) if limit - committed < headroom_stop_mib else None

    # X2W_PROFILE=<start s>,<samples>,<interval ms>[,<repeat every s>]: clock-driven samples of every
    # thread (CPU time, rip, return addresses) between debug events -> profile_<s>.txt (sample_profile.py).
    profiler = None
    if os.environ.get("X2W_PROFILE"):
        import sample_profile
        plan = sample_profile.parse(os.environ["X2W_PROFILE"])
        if plan:
            profiler = sample_profile.Profiler(plan, wt.HANDLE(pi.hProcess), logdir, lambda a: where(a), modules,
                                               module_paths, log)
        else:
            print(f"X2W_PROFILE={os.environ['X2W_PROFILE']} ignored: expected <start s>,<samples>,<interval ms>[,<repeat s>]")
    interrupted = False
    stop_requested = False
    stop_next = 0.0
    asserts_total = 0
    exception_repeats = {}  # (code, address) -> events not logged because of --log-repeats
    try:
        while time.time() - start < timeout:
            flush_logs()
            low = commit_check()
            if low:
                headroom_stop = low
                log(f"{time.time() - start:7.3f} host commit headroom {low[1] - low[0]} MiB < {headroom_stop_mib} MiB "
                    f"(committed {low[0]} / limit {low[1]} MiB): ending the run (X2W_HEADROOM_STOP_MIB)")
                break
            if time.time() >= stop_next:
                stop_next = time.time() + 0.5
                if os.path.exists(stop_file):
                    stop_requested = True
                    log(f"{time.time() - start:7.3f} stop file {stop_file} found: ending the run")
                    break
                # <logdir>/snapshot asks for every thread's full stack now, in threads_<seconds>.txt;
                # the run continues. For a run that stops making progress without faulting.
                if os.path.exists(snapshot_file):
                    os.remove(snapshot_file)
                    when = time.time() - start
                    taken = dump_threads(os.path.join(logdir, f"threads_{when:.0f}.txt"))
                    log(f"{when:7.3f} snapshot: {len(taken)} threads -> threads_{when:.0f}.txt "
                        f"({time.time() - start - when:.2f} s)")
            if dump_at and time.time() - start >= dump_at[0]:
                dump_threads(os.path.join(logdir, f"threads_{dump_at.pop(0):g}.txt"))
            if watches and time.time() >= watch_next:
                watch_next = time.time() + 1.0
                poll_watches()
            if profiler:
                profiler.poll(time.time() - start, live_threads)
            if not k32.WaitForDebugEvent(event, profiler.timeout_ms(time.time() - start) if profiler else 200):
                continue
            code, pid, tid = struct.unpack_from("<III", event, 0)
            status = DBG_CONTINUE
            if code == CREATE_PROCESS:
                live_threads.add(tid)
                h_file, _, h_thread, base = struct.unpack_from("<QQQQ", event, 16)
                if h_thread:
                    thread_handles[tid] = h_thread
                main_base = base
                for item in filter(None, os.environ.get("X2W_COUNT", "").split(",")):
                    hooks[base + int(item, 16)] = (f"+{item}", "count")
                # Experiment switch X2W_REDIRECT=<rva>@<n>:<target rva>[,...]: on the n-th hit of a title
                # address, execution continues at another address instead. Used to force one code path.
                for item in filter(None, os.environ.get("X2W_REDIRECT", "").split(",")):
                    source, _, target = item.partition(":")
                    rva, _, nth = source.partition("@")
                    hooks[base + int(rva, 16)] = (f"+{rva}", "count")
                    redirects[f"+{rva}"] = (int(nth or "1"), base + int(target, 16))
                modules[base] = os.path.basename(exe)
                log(f"{time.time() - start:7.3f} process {os.path.basename(exe)} at {base:#x}")
                if h_file:
                    k32.CloseHandle(wt.HANDLE(h_file))
            elif code == LOAD_DLL:
                h_file, base = struct.unpack_from("<QQ", event, 16)
                name = final_path(h_file) if h_file else "?"
                modules[base] = name
                module_paths[base] = final_path(h_file, True) if h_file else ""
                log(f"{time.time() - start:7.3f} load {name} at {base:#x}")
                if trace and name.lower() == "xgameruntime.dll":
                    # Gaming runtime: log which API interfaces the title asks for and the result.
                    k32.LoadLibraryExW.restype = wt.HMODULE
                    local = k32.LoadLibraryExW(r"C:\Windows\System32\xgameruntime.dll", None, 1)
                    for export in ("QueryApiImpl", "InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2", "XErrorReport"):
                        address = k32.GetProcAddress(local, export.encode()) if local else 0
                        if address:
                            hooks[base + (address - local)] = (export, "runtime")
                            if loader_break_seen:
                                arm(base + (address - local))
                if trace and name.lower() == "bink2scarlett.dll":
                    # Movie player: log opens/closes/frames and remember the registered frame buffers.
                    k32.LoadLibraryExW.restype = wt.HMODULE
                    local = k32.LoadLibraryExW(os.path.join(os.path.dirname(exe), name), None, 1)
                    for export in ("BinkOpenWithOptions", "BinkClose", "BinkNextFrame", "BinkRegisterFrameBuffers",
                                   "BinkPause", "BinkShouldSkip"):
                        address = k32.GetProcAddress(local, export.encode()) if local else 0
                        if address:
                            hooks[base + (address - local)] = (export, "bink")
                            if loader_break_seen:
                                arm(base + (address - local))
                if h_file:
                    k32.CloseHandle(wt.HANDLE(h_file))
            elif code == CREATE_THREAD:
                log(f"{time.time() - start:7.3f} thread {tid} created")
                live_threads.add(tid)
                h_thread = struct.unpack_from("<Q", event, 16)[0]
                if h_thread:
                    thread_handles[tid] = h_thread
            elif code == 4:  # EXIT_THREAD
                live_threads.discard(tid)
                thread_handles.pop(tid, None)  # the system closes it when the event is continued
            elif code == EXIT_PROCESS:
                exit_code = struct.unpack_from("<I", event, 16)[0]
                log(f"{time.time() - start:7.3f} exit {exit_code:#010x}")
                k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                break
            elif code == OUTPUT_STRING:
                addr, unicode, length = struct.unpack_from("<QHH", event, 16)
                raw = read(addr, length * (2 if unicode else 1))
                text = raw.decode("utf-16-le" if unicode else "latin-1", errors="replace").rstrip("\0\r\n")
                log(f"{time.time() - start:7.3f} debug-output {text[:300]}")
            elif code == EXCEPTION:
                ex_code, ex_flags, _, ex_addr, nparams = struct.unpack_from("<IIQQI", event, 16)
                params = struct.unpack_from("<" + "Q" * min(nparams, 15), event, 16 + 32) if nparams else ()
                first = struct.unpack_from("<I", event, 16 + 152)[0]
                label = NAMES.get(ex_code, "")
                if ex_code == 0x80000003 and ex_addr in breakpoints:
                    handle_breakpoint(tid, ex_addr)
                    k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                    continue
                if ex_code == 0x80000003 and ex_addr in ever_armed and read(ex_addr, 1) != b"\xcc":
                    # A thread that reached one of our int3s just before it was removed (its event was
                    # queued behind the removal): run the original instruction there.
                    thread, ctx = get_ctx(tid, 0x100003)
                    if thread:
                        struct.pack_into("<Q", ctx, 0xF8, ex_addr)
                        set_ctx(thread)
                        k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                        continue
                if ex_code == 0x80000004 and tid in stepping:
                    addr = stepping.pop(tid)
                    if addr in breakpoints:
                        write(addr, b"\xcc")
                    k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                    continue
                if (ex_code == 0xC000001D and first and host_cpu_on and not stop_first and main_base
                        and main_base <= ex_addr < main_base + 0x10000000):
                    name = rewrite_for_host(ex_addr)
                    if not name and sha_for_host(tid, ex_addr):
                        name = "sha (per hit)"
                    if name:
                        host_cpu_sites[name] = host_cpu_sites.get(name, 0) + 1
                        if sum(host_cpu_sites.values()) <= 40:
                            log(f"{time.time() - start:7.3f} host-cpu {name} at {where(ex_addr)} rewritten")
                        k32.ContinueDebugEvent(pid, tid, DBG_CONTINUE)
                        continue
                # A first-chance exception that repeats from one address is logged --log-repeats times,
                # then counted (finished.txt). The stopping exception is always logged.
                if not first or stop_first or repeat_ok((ex_code, ex_addr)):
                    log(f"{time.time() - start:7.3f} exception {ex_code:#010x} {label} at {where(ex_addr)} "
                        f"{'first' if first else 'second'}-chance params={[hex(p) for p in params]}")
                else:
                    exception_repeats[(ex_code, ex_addr)] = exception_repeats.get((ex_code, ex_addr), 0) + 1
                if ex_code == 0x80000003 and not loader_break_seen:
                    loader_break_seen = True  # the loader's initial breakpoint
                    for addr in hooks:
                        arm(addr)
                elif (ex_code == 0x80000003 and first and main_base and main_base <= ex_addr < main_base + 0x10000000
                      and asserts_total < skip_asserts and (ex_addr - main_base) not in stop_at):
                    asserts_seen[ex_addr] = asserts_seen.get(ex_addr, 0) + 1
                    asserts_total += 1
                    status = DBG_CONTINUE  # rip is already past the int3
                elif (not first) or stop_first or (ex_code == 0x80000003 and main_base and
                                                      main_base <= ex_addr < main_base + 0x10000000):
                    if (ex_code, ex_addr) in exception_repeats:  # make sure the stop itself is in events.txt
                        log(f"{time.time() - start:7.3f} exception {ex_code:#010x} {label} at {where(ex_addr)} "
                            f"{'first' if first else 'second'}-chance params={[hex(p) for p in params]}")
                    stop = (ex_code, ex_addr, tid, first, params)
                    break
                elif ex_code in (0x406D1388, 0x40010006):
                    status = DBG_CONTINUE
                else:
                    status = DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(pid, tid, status)

    except KeyboardInterrupt:
        # Ctrl+C or a closed console: end the run as at a timeout, but with --fast-exit's
        # short close, so the reports are still written.
        interrupted = True
        fast_exit = True
    finally:
        if profiler:
            profiler.close()
        # Also on an unexpected error in the loop: the buffered last half second is the part
        # that explains a crash.
        flush_logs(force=True)
    elapsed = time.time() - start
    close_phases = []  # (name, seconds): how long each step of the close took, for finished.txt

    def timed(name, function, *args):
        began = time.time()
        try:
            return function(*args)
        finally:
            close_phases.append((name, time.time() - began))
    if stop and (stop_at or not stop[3]):
        # At an X2W_STOP_AT breakpoint and at a second-chance exception: every thread's full stack.
        dumped = timed("threads_stop.txt", dump_threads, os.path.join(logdir, "threads_stop.txt"))
        print(f"  {len(dumped)} threads -> threads_stop.txt")
    if stop:
        ex_code, ex_addr, tid, first, params = stop
        print(f"stopped at {NAMES.get(ex_code, hex(ex_code))} ({ex_code:#010x}) at {where(ex_addr)} after {elapsed:.2f} s "
              f"({'first' if first else 'second'} chance) params={[hex(p) for p in params]}")
        thread = k32.OpenThread(0x0008 | 0x0002 | 0x0040, False, tid)  # GET_CONTEXT | SUSPEND | QUERY
        raw = (ctypes.c_ubyte * (1232 + 16))()
        ctx_addr = (ctypes.addressof(raw) + 15) & ~15
        struct.pack_into("<I", (ctypes.c_ubyte * 1232).from_address(ctx_addr), 0x30, 0x10000B)
        if thread and k32.GetThreadContext(wt.HANDLE(thread), ctypes.c_void_p(ctx_addr)):
            ctx = bytes((ctypes.c_ubyte * 1232).from_address(ctx_addr))
            values = struct.unpack_from("<16Q", ctx, 0x78) + struct.unpack_from("<Q", ctx, 0xF8)
            regs = dict(zip(REGS, values))
            print("  " + " ".join(f"{k.lower()}={v:#x}" for k, v in regs.items()))
            stack = read(regs["Rsp"], 8 * 16)
            words = struct.unpack_from(f"<{len(stack) // 8}Q", stack) if stack else ()
            print("  stack: " + " ".join(where(w) if any(b <= w < b + 0x10000000 for b in modules) else f"{w:#x}" for w in words))
            # Likely return addresses: module-range words in the next 8 KiB whose preceding bytes are a call.
            deep = read(regs["Rsp"], 0x2000) or b""
            rets = []
            for i in range(0, len(deep) - 7, 8):
                w = struct.unpack_from("<Q", deep, i)[0]
                if not any(b <= w < b + 0x10000000 for b in modules):
                    continue
                before = read(w - 6, 6) or b""
                if len(before) == 6 and (before[1] == 0xE8 or before[0] == 0xFF and before[1] in (0x15, 0x50, 0x90, 0x10, 0x14)
                                         or before[3] == 0xFF and (before[4] & 0x38) == 0x10 or before[4] == 0xFF and (before[5] & 0xF8) == 0xD0):
                    rets.append(f"+{i:#x}:{where(w)}")
            print("  return addresses: " + " ".join(rets[:40]))
            raw_words = struct.unpack_from(f"<{min(len(deep), 0x300) // 8}Q", deep) if deep else ()
            for row in range(0, len(raw_words), 8):
                print(f"  raw stack +{row * 8:#05x}: " + " ".join(f"{w:#x}" for w in raw_words[row:row + 8]))
            sys.stdout.flush()
            # X2W_TRAP_DUMP="exe+RVA:len,reg+off:len,0xADDR:len": memory dumps at the stop.
            exe_base = next((b for b, n in modules.items() if n.lower().endswith(".exe")), 0)
            for spec in filter(None, os.environ.get("X2W_TRAP_DUMP", "").split(",")):
                try:
                    where_s, length = spec.split(":")
                    parts = where_s.replace("-", "+-").split("+")
                    addr = 0
                    for part in parts:
                        part = part.strip()
                        if not part:
                            continue
                        neg = part.startswith("-")
                        part = part.lstrip("-")
                        if part == "exe":
                            value = exe_base
                        elif part.capitalize() in regs:
                            value = regs[part.capitalize()]
                        else:
                            value = int(part, 16)
                        addr += -value if neg else value
                    data = read(addr, int(length, 16)) or b""
                    for o in range(0, len(data), 32):
                        print(f"  dump {spec} {addr + o:#x}: " + data[o:o + 32].hex(" ", 4))
                except Exception as error:
                    print(f"  dump {spec}: {error}")
            sys.stdout.flush()
            if ex_code == 0xC0000005 and len(params) >= 2:
                # Region around the faulting address and the allocations just below it.
                class MBI(ctypes.Structure):
                    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                                ("AllocationProtect", ctypes.c_uint32), ("PartitionId", ctypes.c_uint16),
                                ("RegionSize", ctypes.c_uint64), ("State", ctypes.c_uint32),
                                ("Protect", ctypes.c_uint32), ("Type", ctypes.c_uint32)]
                k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
                probe = params[1] & ~0xFFFF
                for _ in range(3):
                    mbi = MBI()
                    if not k32.VirtualQueryEx(wt.HANDLE(pi.hProcess), probe, ctypes.byref(mbi), ctypes.sizeof(mbi)):
                        break
                    print(f"  region base={mbi.BaseAddress:#x} alloc={mbi.AllocationBase:#x} size={mbi.RegionSize:#x} "
                          f"state={mbi.State:#x} protect={mbi.Protect:#x} type={mbi.Type:#x}")
                    if mbi.BaseAddress == 0:
                        break
                    probe = mbi.BaseAddress - 0x1000
            code_bytes = read(regs["Rip"] - 32, 48)
            print(f"  code at rip-32: {code_bytes.hex(' ')}")
        if thread:
            k32.CloseHandle(wt.HANDLE(thread))
    elif exit_code is not None:
        print(f"process exited with {exit_code:#010x} after {elapsed:.2f} s")
    else:
        if headroom_stop:
            print(f"stopped: host commit headroom {headroom_stop[1] - headroom_stop[0]} MiB below {headroom_stop_mib} MiB "
                  f"(committed {headroom_stop[0]} / limit {headroom_stop[1]} MiB) after {elapsed:.2f} s")
            fast_exit = True
        if stop_requested:
            print(f"stopped by request after {elapsed:.2f} s (stop file {stop_file})")
        print(f"{'interrupted' if interrupted else 'headroom stop' if headroom_stop else 'stop requested' if stop_requested else 'timeout'} after {elapsed:.2f} s")
        # --fast-exit: each thread's rip only (no stack walk); the full walk otherwise.
        dumped = timed("threads.txt", dump_threads, os.path.join(logdir, "threads.txt"), not fast_exit)
        for item in filter(None, os.environ.get("X2W_DUMP", "").split(",")):
            rva, _, size = item.partition(":")
            blob = read(main_base + int(rva, 16), int(size or "64", 16)) or b""
            print(f"  dump +{rva}: " + " ".join(f"{q:#x}" for q in struct.unpack_from(f"<{len(blob) // 8}Q", blob)))
        tops = {}
        for line in dumped:
            top = line.split(" stack:")[0].split("rip=")[1]
            tops[top] = tops.get(top, 0) + 1
        print(f"  {len(dumped)} threads -> threads.txt; rip histogram: " +
              ", ".join(f"{k} x{v}" for k, v in sorted(tops.items(), key=lambda kv: -kv[1])[:8]))
    if asserts_seen:
        print("  asserts continued: " + ", ".join(f"{where(a)} x{n}" for a, n in asserts_seen.items()))
    if host_cpu_sites:
        print("  host-cpu sites rewritten: " + ", ".join(f"{name} x{n}" for name, n in sorted(host_cpu_sites.items())))
    if capped or exception_repeats:
        top = sorted(exception_repeats.items(), key=lambda kv: -kv[1])[:8]
        print(f"  not logged after {log_repeats} repeats (--log-repeats): " +
              ", ".join([f"{name} x{n}" for name, n in sorted(capped.items())] +
                        [f"{NAMES.get(c, hex(c))} at {where(a)} x{n}" for (c, a), n in top]))
    timed("bink", bink_report, not fast_exit)
    if os.environ.get("X2W_COUNT"):
        print("  hit counts: " + ", ".join(f"+{item} x{hit_counts.get('+' + item, 0)}"
                                           for item in os.environ["X2W_COUNT"].split(",") if item))
    if offline:
        print(f"  offline: {sum(denied.values())} network opens refused" +
              ("" if not denied else " (" + ", ".join(f"{k} x{v}" for k, v in list(denied.items())[:6]) + ")"))

    def capture_window():
        """Saves what the title's window currently shows on screen (client area) as window.png."""
        import zlib
        user32, gdi32 = ctypes.WinDLL("user32"), ctypes.WinDLL("gdi32")
        found = []
        proto = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def visit(hwnd, _):
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pi.dwProcessId and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user32.EnumWindows(proto(visit), 0)
        if not found:
            print("  window capture: no visible window")
            return
        hwnd = found[0]
        rect = wt.RECT()
        user32.GetClientRect(wt.HWND(hwnd), ctypes.byref(rect))
        width, height = min(rect.right, 1920), min(rect.bottom, 1080)
        if width <= 0 or height <= 0:
            print("  window capture: empty client area")
            return
        user32.GetDC.restype = wt.HDC
        gdi32.CreateCompatibleDC.restype = wt.HDC
        gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
        source = user32.GetDC(wt.HWND(hwnd))
        memory = gdi32.CreateCompatibleDC(wt.HDC(source))
        bitmap = gdi32.CreateCompatibleBitmap(wt.HDC(source), width, height)
        gdi32.SelectObject(wt.HDC(memory), wt.HBITMAP(bitmap))
        gdi32.BitBlt(wt.HDC(memory), 0, 0, width, height, wt.HDC(source), 0, 0, 0x00CC0020)
        header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        pixels = (ctypes.c_ubyte * (width * height * 4))()
        gdi32.GetDIBits(wt.HDC(memory), wt.HBITMAP(bitmap), 0, height, pixels, header, 0)
        raw = bytes(pixels)
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
        stride = width * 3
        rows = [bytes(1) + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(height)]

        def chunk(kind, payload):
            body = kind + payload
            return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

        png = (bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) +
               chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
        with open(os.path.join(logdir, "window.png"), "wb") as handle:
            handle.write(png)
        title = ctypes.create_unicode_buffer(128)
        user32.GetWindowTextW(wt.HWND(hwnd), title, 128)
        print(f"  window capture: {width}x{height} -> window.png (client area as on screen; title text length {len(title.value)})")

    if exit_code is None and not fast_exit:
        try:
            timed("window", capture_window)
        except Exception as error:  # diagnostics only
            print(f"  window capture failed: {error}")
    timed("terminate", k32.TerminateJobObject, wt.HANDLE(job), 0xDEAD0002)
    for h in (out, err, pi.hThread, pi.hProcess):
        k32.CloseHandle(wt.HANDLE(h))
    for handle in open_logs:
        handle.close()
    print(f"modules loaded: {len(modules)}")
    print(f"  close: {time.time() - start - elapsed:.2f} s after the run ended" +
          (" (fast exit)" if fast_exit else "") + "".join(f", {name} {seconds:.2f} s" for name, seconds in close_phases))
    with open(os.path.join(logdir, "finished.txt"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(summary) + "\n")
    for name in ("stderr.txt", "trap.log"):
        path = os.path.join(logdir, name)
        if os.path.exists(path) and os.path.getsize(path):
            with open(path, "rb") as handle:  # only the end is shown: read the last 64 KiB, not the whole log
                handle.seek(max(0, os.path.getsize(path) - 65536))
                print(f"--- {name}\n" + handle.read().decode("utf-8", errors="replace").replace("\r\n", "\n")[-3000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
