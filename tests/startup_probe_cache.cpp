// LOCAL stall: probe_readable with and without its unreadable-range cache (local_log.h, X2W_PROBE_CACHE).
//
//   startup_probe_cache <cache|nocache>
//   (cache: X2W_PROBE_CACHE=1 X2W_PROBE_CACHE_MS=100)
//
// A 1 MiB reservation with its first 64 KiB committed. First-chance access violations are counted by a vectored
// handler (each one is what costs a debugger round trip in a run).
//   1. 100 probes of [base, base + 128 KiB): 64 KiB readable every time. Violations: cache 1, nocache 100.
//   2. 100 probes at base + 512 KiB (reserved): 0 readable. Violations added: cache 0 (same region), nocache 100.
//   3. +64 KiB committed, no invalidation, within the expiry: cache still says 64 KiB (the stale direction is
//      "unreadable", never "readable"); nocache 128 KiB.
//   4. invalidate_probe_cache(): cache 128 KiB.
//   5. Expiry: a probe at base + 512 KiB faults once (cached), again 0 faults, then after 150 ms faults once more.
//   6. The reservation released (free memory): 0 readable, no crash.
// Exit 0 pass, other values name the step.
#include "startup_stubs/local_log.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
volatile LONG violations;
LONG CALLBACK count_violation(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) InterlockedIncrement(&violations);
    return EXCEPTION_CONTINUE_SEARCH;
}
int failures;
void expect(const char* what, long long got, long long want)
{
    std::printf("%s: %lld (want %lld)%s\n", what, got, want, got == want ? "" : "  <-- FAIL");
    if (got != want) ++failures;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode != "cache" && mode != "nocache") return 2;
    const bool cache = mode == "cache";
    if (x2w_local::probe_cache_on() != cache) {
        std::printf("X2W_PROBE_CACHE is %s, mode %s\n", x2w_local::probe_cache_on() ? "on" : "off", mode.c_str());
        return 3;
    }
    AddVectoredExceptionHandler(1, count_violation);
    auto* base = static_cast<unsigned char*>(VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE, PAGE_READWRITE));
    if (!base || !VirtualAlloc(base, 64u << 10, MEM_COMMIT, PAGE_READWRITE)) return 4;
    std::memset(base, 1, 64u << 10);
    const auto b = reinterpret_cast<std::uintptr_t>(base);

    bool all = true;
    for (int i = 0; i < 100; ++i) all = all && x2w_local::probe_readable(b, 128u << 10) == (64u << 10);
    expect("1. 100 probes of 128 KiB, all 64 KiB readable", all, 1);
    expect("1. access violations", violations, cache ? 1 : 100);

    LONG before = violations;
    all = true;
    for (int i = 0; i < 100; ++i) all = all && x2w_local::probe_readable(b + (512u << 10), 4096) == 0;
    expect("2. 100 probes in the reserved part, all 0", all, 1);
    expect("2. access violations added", violations - before, cache ? 0 : 100);

    if (!VirtualAlloc(base + (64u << 10), 64u << 10, MEM_COMMIT, PAGE_READWRITE)) return 5;
    expect("3. committed +64 KiB, no invalidation: readable", static_cast<long long>(x2w_local::probe_readable(b, 128u << 10)),
           cache ? (64u << 10) : (128u << 10));
    x2w_local::invalidate_probe_cache();
    expect("4. after invalidate_probe_cache: readable", static_cast<long long>(x2w_local::probe_readable(b, 128u << 10)), 128u << 10);

    x2w_local::invalidate_probe_cache();
    before = violations;
    x2w_local::probe_readable(b + (512u << 10), 4096);
    expect("5. first probe after invalidation: violations", violations - before, 1);
    before = violations;
    x2w_local::probe_readable(b + (512u << 10), 4096);
    expect("5. second probe: violations", violations - before, cache ? 0 : 1);
    Sleep(150);
    before = violations;
    x2w_local::probe_readable(b + (512u << 10), 4096);
    expect("5. after 150 ms (expiry 100 ms): violations", violations - before, 1);

    VirtualFree(base, 0, MEM_RELEASE);
    x2w_local::invalidate_probe_cache();
    expect("6. released: readable", static_cast<long long>(x2w_local::probe_readable(b, 4096)), 0);

    const x2w_local::ProbeCache& c = x2w_local::probe_cache();
    std::printf("cache counters: hits %lld faults %lld queries %lld (%.3f ms total) ranges kept %lld\n", c.hits, c.faults, c.queries,
                [&] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(c.query_ticks) * 1000.0 / static_cast<double>(f.QuadPart); }(),
                c.cached);
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 16 : 0;
}
