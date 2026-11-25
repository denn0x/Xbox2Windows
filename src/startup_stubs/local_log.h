// Experimental (docs/startup-runtime.md): opt-in switches and logging shared by the
// permissive startup paths. Lines go to stderr and are appended to %X2W_TRAP_LOG% when set.
#pragma once

#include "startup_stubs/log_cap.h"
#include "startup_stubs/trap.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace x2w_local {

// Reads a switch without disturbing the caller's last-error value (GetEnvironmentVariable sets
// ERROR_ENVVAR_NOT_FOUND for an absent variable, and shim callers observe GetLastError).
inline bool env_on(const char* name) noexcept
{
    const DWORD saved = GetLastError();
    char value[4]{};
    const bool on = GetEnvironmentVariableA(name, value, sizeof(value)) && value[0] == '1';
    SetLastError(saved);
    return on;
}

inline void logf(const char* format, ...) noexcept
{
    char line[1536];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 3) n = sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    const DWORD error = GetLastError();
    x2w_trap::write_all(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n));
    x2w_trap::append_trap_log(line, static_cast<DWORD>(n));  // one open handle per module (trap.h)
    SetLastError(error);
}

// first_calls (a saturating log cap) is in startup_stubs/log_cap.h.

// Number of contiguous readable bytes at start, up to bytes, found by touching one byte per page.
// VirtualQuery is not used for this: on the title's multi-gigabyte regions one query takes about a
// millisecond (it walks the region to size it). A bad pointer costs a first-chance exception, so
// callers on hot paths should pass values that normally are pointers. A guard page that the probe
// trips is re-armed.
//
// LOCAL stall: X2W_PROBE_CACHE=1 (default off) remembers the regions a probe faulted in. After a fault (not a guard
// page) one VirtualQuery at the faulting page; a region that is free, reserved or no-access is kept for
// X2W_PROBE_CACHE_MS (default 100 ms; each module has its own cache), and later probes return at its start without
// touching it (no exception). Only UNREADABLE ranges are cached: a readable range is never assumed, because the title
// and the other stub modules (xmem, the title's own VirtualAlloc / VirtualFree) change pages without any hook this
// module sees, and a stale "readable" would turn a fault the probe catches into one the caller's memcpy does not. The
// cost of a stale entry is the other way round: memory committed in that range inside the expiry reads as unreadable
// (the caller binds null / skips, as for any unreadable range) until the entry expires or invalidate_probe_cache().
struct ProbeCache {
    struct Range { std::uintptr_t start, end; ULONGLONG until; };
    static constexpr unsigned size = 64;
    SRWLOCK lock = SRWLOCK_INIT;
    Range ranges[size] = {};
    unsigned next = 0;
    volatile LONG64 hits = 0, faults = 0, cached = 0, queries = 0, query_ticks = 0;
    volatile LONG generation = 0;   // invalidate_probe_cache: entries of an older generation are ignored
    LONG range_generation[size] = {};
};
inline ProbeCache& probe_cache() noexcept
{
    static ProbeCache cache;
    return cache;
}
inline bool probe_cache_on() noexcept
{
    static const bool on = env_on("X2W_PROBE_CACHE");
    return on;
}
inline ULONGLONG probe_cache_ms() noexcept
{
    static const ULONGLONG ms = [] {
        const DWORD saved = GetLastError();
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_PROBE_CACHE_MS", value, sizeof(value));
        SetLastError(saved);
        const unsigned long parsed = n && n < sizeof(value) ? std::strtoul(value, nullptr, 10) : 100;
        return static_cast<ULONGLONG>(parsed);
    }();
    return ms;
}
inline void invalidate_probe_cache() noexcept { InterlockedIncrement(&probe_cache().generation); }
// The end of the part of [start, end) before the first cached unreadable range; start itself when start is in one.
inline std::uintptr_t probe_cache_clamp(std::uintptr_t start, std::uintptr_t end) noexcept
{
    ProbeCache& c = probe_cache();
    const ULONGLONG now = GetTickCount64();
    const LONG generation = c.generation;
    AcquireSRWLockShared(&c.lock);
    for (unsigned i = 0; i < ProbeCache::size; ++i) {
        const ProbeCache::Range& r = c.ranges[i];
        if (r.end <= r.start || now >= r.until || c.range_generation[i] != generation || r.end <= start || r.start >= end) continue;
        end = r.start <= start ? start : r.start;
        if (end == start) break;
    }
    ReleaseSRWLockShared(&c.lock);
    return end;
}
inline void probe_cache_fault(std::uintptr_t at) noexcept
{
    ProbeCache& c = probe_cache();
    InterlockedIncrement64(&c.faults);
    MEMORY_BASIC_INFORMATION info{};
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info));
    QueryPerformanceCounter(&t1);
    InterlockedIncrement64(&c.queries);
    InterlockedAdd64(&c.query_ticks, t1.QuadPart - t0.QuadPart);
    if (got != sizeof(info) || !info.RegionSize) return;
    const DWORD protect = info.Protect & 0xFF;
    const bool unreadable = info.State != MEM_COMMIT || protect == PAGE_NOACCESS || protect == 0;
    if (!unreadable || (info.Protect & PAGE_GUARD)) return;
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    AcquireSRWLockExclusive(&c.lock);
    const unsigned slot = c.next++ % ProbeCache::size;
    c.ranges[slot] = {begin, begin + info.RegionSize, GetTickCount64() + probe_cache_ms()};
    c.range_generation[slot] = c.generation;
    ReleaseSRWLockExclusive(&c.lock);
    InterlockedIncrement64(&c.cached);
}
inline std::size_t probe_readable(std::uintptr_t start, std::size_t bytes) noexcept
{
    if (start < 0x10000 || start > 0x00007FFFFFFEFFFFull || bytes == 0) return 0;
    std::size_t done = 0;
    DWORD code = 0;
    std::uintptr_t at = start;
    std::uintptr_t end = start + bytes;
    const bool cache = probe_cache_on();
    if (cache) {
        const std::uintptr_t clamped = probe_cache_clamp(start, end);
        if (clamped != end) InterlockedIncrement64(&probe_cache().hits);
        end = clamped;
        if (end == start) return 0;
    }
    __try {
        volatile unsigned char sink = 0;
        while (at < end) {
            sink = *reinterpret_cast<const volatile unsigned char*>(at);
            const std::uintptr_t next = (at | 0xFFF) + 1;
            done = static_cast<std::size_t>((next < end ? next : end) - start);
            at = next;
        }
        (void)sink;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        if (code == 0x80000001u) {  // STATUS_GUARD_PAGE_VIOLATION: the system dropped the guard bit
            MEMORY_BASIC_INFORMATION info{};
            DWORD old = 0;
            if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) && info.State == MEM_COMMIT)
                VirtualProtect(reinterpret_cast<void*>(at & ~std::uintptr_t{0xFFF}), 0x1000, info.Protect | PAGE_GUARD, &old);
        } else if (cache) {
            probe_cache_fault(at);
        }
    }
    return done;
}

// Readability by probing (see probe_readable); writability by one VirtualQuery (rare callers).
inline bool accessible(const void* pointer, std::size_t bytes, bool write) noexcept
{
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFEFFFFull || bytes == 0) return false;
    if (!write) return probe_readable(start, bytes) == bytes;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    const DWORD protect = info.Protect & 0xFF;
    if ((info.Protect & PAGE_GUARD) || protect == PAGE_NOACCESS || protect == 0) return false;
    const bool writable = protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_WRITECOPY;
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = begin + info.RegionSize;
    // start lies inside the queried region, so end - start cannot wrap; start + bytes could.
    if (bytes > end - start || (write && !writable)) return false;
    return true;
}
inline bool readable(const void* pointer, std::size_t bytes) noexcept { return accessible(pointer, bytes, false); }
inline bool writable(const void* pointer, std::size_t bytes) noexcept { return accessible(pointer, bytes, true); }

}  // namespace x2w_local
