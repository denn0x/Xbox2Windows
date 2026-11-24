// LOCAL accounting (xmem_census.h, out/gamerun/runtime_work/NOTES.md).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "xmem_census.h"
#include "startup_stubs/local_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr ULONGLONG mib = 1ull << 20;

ULONGLONG system_commit(ULONGLONG* limit) noexcept
{
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof(info);
    if (!GetPerformanceInfo(&info, sizeof(info))) return 0;
    if (limit) *limit = static_cast<ULONGLONG>(info.CommitLimit) * info.PageSize;
    return static_cast<ULONGLONG>(info.CommitTotal) * info.PageSize;
}

// The system commit when xmem.dll was loaded: the reference for "this run".
const ULONGLONG commit_at_load = system_commit(nullptr);
const ULONGLONG tick_at_load = GetTickCount64();

SRWLOCK block_lock = SRWLOCK_INIT;
std::vector<std::pair<ULONG_PTR, SIZE_T>>* blocks;  // sorted by base when the census reads it

int append(char* out, int at, int capacity, const char* format, ...) noexcept
{
    if (at >= capacity - 1) return at;
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(out + at, static_cast<size_t>(capacity - at), format, args);
    va_end(args);
    if (n < 0) return at;
    return std::min(capacity - 1, at + n);
}

// GPU memory of this process per adapter that has some (DXGI segment groups).
int gpu_text(char* out, int at, int capacity) noexcept
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return append(out, at, capacity, " gpu unknown");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    bool any = false;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter3> three;
        if (FAILED(adapter.As(&three))) continue;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
        if (FAILED(three->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) continue;
        three->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
        if (!local.CurrentUsage && !nonlocal.CurrentUsage) continue;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        at = append(out, at, capacity, " gpu%u(%04x) local=%llu/%llu non-local=%llu/%llu MiB", i, desc.DeviceId,
                    local.CurrentUsage / mib, local.Budget / mib, nonlocal.CurrentUsage / mib, nonlocal.Budget / mib);
        any = true;
    }
    return any ? at : append(out, at, capacity, " gpu none");
}

int heap_text(char* out, int at, int capacity) noexcept
{
    HANDLE heaps[256];
    const DWORD n = GetProcessHeaps(256, heaps);
    ULONGLONG committed = 0, largest = 0;
    DWORD counted = 0;
    for (DWORD i = 0; i < n && i < 256; ++i) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heaps[i], 0, &summary)) continue;
        committed += summary.cbCommitted;
        largest = std::max<ULONGLONG>(largest, summary.cbCommitted);
        ++counted;
    }
    return append(out, at, capacity, " heaps %lu committed=%llu MiB (largest %llu)", counted, committed / mib, largest / mib);
}

// LOCAL memcut: resident (working-set) MiB of each startup block of 16 MiB or more, a lower bound of
// the pages the title has touched (untouched committed pages are never resident; touched ones can be
// trimmed). Ranks the "commit startup blocks on first touch" cut (memcut_work/PLAN.md).
// The walk (~1.2 M pages in run 82) runs at most every 9 s; the lines in between repeat its text.
int block_resident_walk(char* out, int at, int capacity);
int block_resident_text(char* out, int at, int capacity) noexcept
{
    static char cached[384];
    static ULONGLONG last;
    const ULONGLONG now = GetTickCount64();
    if (!cached[0] || now - last >= 9000) {
        last = now;
        cached[0] = 0;
        try {
            block_resident_walk(cached, 0, sizeof(cached));
        } catch (...) {
            cached[0] = 0;  // out of memory: no residency text this time
        }
    }
    return cached[0] ? append(out, at, capacity, "%s", cached) : at;
}
int block_resident_walk(char* out, int at, int capacity)
{
    std::vector<std::pair<ULONG_PTR, SIZE_T>> known;
    AcquireSRWLockShared(&block_lock);
    if (blocks) known = *blocks;
    ReleaseSRWLockShared(&block_lock);
    if (known.empty()) return at;
    std::sort(known.begin(), known.end());
    constexpr SIZE_T page = 4096, chunk = 65536;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(chunk);
    at = append(out, at, capacity, "; block resident MiB");
    for (const auto& [base, bytes] : known) {
        if (bytes < (16ull << 20)) continue;
        ULONGLONG resident = 0;
        for (SIZE_T done = 0; done < bytes / page;) {
            const SIZE_T n = std::min(chunk, bytes / page - done);
            for (SIZE_T i = 0; i < n; ++i) info[i].VirtualAddress = reinterpret_cast<void*>(base + (done + i) * page);
            if (!QueryWorkingSetEx(GetCurrentProcess(), info.data(), static_cast<DWORD>(n * sizeof(info[0])))) break;
            for (SIZE_T i = 0; i < n; ++i) resident += info[i].VirtualAttributes.Valid;
            done += n;
        }
        at = append(out, at, capacity, " %llx:%llu/%llu", static_cast<unsigned long long>(base), resident * page / mib,
                    static_cast<unsigned long long>(bytes / mib));
    }
    return at;
}

// Counters of other stand-ins (x2w_memory_counters exports).
int stand_in_text(char* out, int at, int capacity) noexcept
{
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return at;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 512);
    for (DWORD i = 0; i < count; ++i) {
        const auto counters = reinterpret_cast<X2wMemoryCounters>(GetProcAddress(modules[i], "x2w_memory_counters"));
        if (!counters) continue;
        char name[64]{};
        GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, sizeof(name));
        char text[512]{};
        const int n = counters(text, sizeof(text));
        if (n > 0) at = append(out, at, capacity, "; %s: %s", name, text);
    }
    return at;
}

void report(ULONGLONG second) noexcept
{
    char line[1536];  // LOCAL memcut: was 1400; logf's own limit is 1536
    ULONGLONG limit = 0;
    const ULONGLONG committed = system_commit(&limit);
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    X2wXmemStats x{};
    x2w_xmem_memory_stats(&x);
    const long long delta = static_cast<long long>(committed) - static_cast<long long>(commit_at_load);
    int at = std::snprintf(line, sizeof(line),
                           "x2w-mem %llu s: system %+lld MiB since load (committed %llu / limit %llu MiB, headroom %llu); "
                           "private %llu MiB (peak %llu); slabs %llu = %llu MiB, %llu file-backed, %llu moved for the GPU%s (pages %llu live, %llu free slots%s%llu%s); "
                           "xmem commits: startup blocks %llu MiB (%llu), permissive %llu MiB (%llu);",
                           second, delta / static_cast<long long>(mib), committed / mib, limit / mib,
                           limit > committed ? (limit - committed) / mib : 0, counters.PrivateUsage / mib,
                           counters.PeakPagefileUsage / mib, x.slabs, x.slab_bytes / mib, x.file_slabs, x.migrations,
                           x.migration_failures ? " (SOME MOVES FAILED)" : "", x.live_pages, x.free_slots,
                           x.slab_budget_bytes ? ", budget " : "", x.slab_budget_bytes / mib,
                           x.slab_refusals ? " MiB, REFUSED pages" : (x.slab_budget_bytes ? " MiB" : ""),
                           x.startup_block_bytes / mib, x.startup_block_calls, x.permissive_commit_bytes / mib,
                           x.permissive_commit_calls);
    if (at < 0) return;
    at = heap_text(line, at, sizeof(line));
    at = gpu_text(line, at, sizeof(line));
    at = block_resident_text(line, at, sizeof(line));  // LOCAL memcut
    at = stand_in_text(line, at, sizeof(line));
    x2w_local::logf("%s", line);
}

// A VirtualQuery walk outside the stand-in's page reservations: private committed bytes by kind
// and the largest private allocations. Bounded to a few seconds.
void census(ULONGLONG second) noexcept
{
    const ULONGLONG began = GetTickCount64();
    std::vector<ULONG_PTR> ranges(2 * 4096);
    const SIZE_T total = x2w_xmem_reserved_ranges(ranges.data(), 4096);
    std::vector<std::pair<ULONG_PTR, ULONG_PTR>> skip;
    for (SIZE_T i = 0; i < std::min<SIZE_T>(total, 4096); ++i) skip.emplace_back(ranges[2 * i], ranges[2 * i] + ranges[2 * i + 1]);
    std::sort(skip.begin(), skip.end());
    std::vector<std::pair<ULONG_PTR, SIZE_T>> known;
    AcquireSRWLockShared(&block_lock);
    if (blocks) known = *blocks;
    ReleaseSRWLockShared(&block_lock);
    std::sort(known.begin(), known.end());
    auto in_block = [&](ULONG_PTR address) {
        auto it = std::upper_bound(known.begin(), known.end(), std::make_pair(address, ~SIZE_T{0}));
        return it != known.begin() && address - std::prev(it)->first < std::prev(it)->second;
    };
    struct Allocation { ULONG_PTR base; ULONGLONG committed, reserved; DWORD protect; };
    std::vector<Allocation> top;  // largest committed private allocations, at most 16
    ULONGLONG blocks_bytes = 0, private_bytes = 0, image_charged = 0, mapped_bytes = 0, skipped_bytes = 0;
    ULONGLONG buckets[5]{}, bucket_counts[5]{};  // <1 MiB, <16, <256, <1024, >=1024 MiB
    ULONGLONG queries = 0;
    Allocation current{0, 0, 0, 0};
    auto close = [&] {
        if (!current.base || !current.committed) return;
        if (in_block(current.base)) {
            blocks_bytes += current.committed;
        } else {
            const ULONGLONG m = current.committed / mib;
            const int b = m < 1 ? 0 : m < 16 ? 1 : m < 256 ? 2 : m < 1024 ? 3 : 4;
            buckets[b] += current.committed;
            ++bucket_counts[b];
            top.push_back(current);
            std::sort(top.begin(), top.end(), [](const Allocation& a, const Allocation& c) { return a.committed > c.committed; });
            if (top.size() > 16) top.pop_back();
        }
    };
    ULONG_PTR at = 0x10000;
    std::size_t next_skip = 0;
    bool partial = false;
    while (at < 0x7FFFFFFF0000ull) {
        while (next_skip < skip.size() && skip[next_skip].second <= at) ++next_skip;
        if (next_skip < skip.size() && skip[next_skip].first <= at) {
            skipped_bytes += skip[next_skip].second - at;
            at = skip[next_skip].second;
            continue;
        }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
        ++queries;
        ULONG_PTR end = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
        if (next_skip < skip.size() && end > skip[next_skip].first) end = skip[next_skip].first;  // the part before a reservation
        const ULONGLONG bytes = end - at;
        if (info.State == MEM_COMMIT) {
            if (info.Type == MEM_PRIVATE) {
                private_bytes += bytes;
                const auto base = reinterpret_cast<ULONG_PTR>(info.AllocationBase);
                if (base != current.base) {
                    close();
                    current = {base, 0, 0, info.Protect};
                }
                current.committed += bytes;
            } else if (info.Type == MEM_IMAGE) {
                const DWORD p = info.Protect & 0xFF;
                if (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY)
                    image_charged += bytes;
            } else if (info.Type == MEM_MAPPED) {
                mapped_bytes += bytes;
            }
        }
        if (info.Type == MEM_PRIVATE && info.State != MEM_FREE) {
            const auto base = reinterpret_cast<ULONG_PTR>(info.AllocationBase);
            if (base == current.base) current.reserved += bytes;
        }
        at = end;
        if (GetTickCount64() - began > 5000) {
            partial = true;
            break;
        }
    }
    close();
    char line[1500];
    int n = std::snprintf(line, sizeof(line),
                          "x2w-mem census %llu s (%llu ms, %llu queries%s, %llu MiB of %llu stand-in reservations skipped): "
                          "private committed %llu MiB = startup blocks %llu + other %llu [<1M %llu MiB/%llu, <16M %llu/%llu, "
                          "<256M %llu/%llu, <1G %llu/%llu, >=1G %llu/%llu]; image writable %llu MiB; mapped outside reservations %llu MiB; top:",
                          second, GetTickCount64() - began, queries, partial ? ", PARTIAL" : "", skipped_bytes / mib,
                          static_cast<unsigned long long>(total), private_bytes / mib, blocks_bytes / mib,
                          (private_bytes - blocks_bytes) / mib, buckets[0] / mib, bucket_counts[0], buckets[1] / mib,
                          bucket_counts[1], buckets[2] / mib, bucket_counts[2], buckets[3] / mib, bucket_counts[3],
                          buckets[4] / mib, bucket_counts[4], image_charged / mib, mapped_bytes / mib);
    if (n < 0) return;
    for (const Allocation& a : top)
        n = append(line, n, sizeof(line), " %p:%llu/%lluMiB p%lx", reinterpret_cast<void*>(a.base), a.committed / mib,
                   a.reserved / mib, a.protect);
    x2w_local::logf("%s", line);
}

DWORD WINAPI reporter(void*)
{
    std::vector<ULONGLONG> census_at;
    char text[256]{};
    const DWORD n = GetEnvironmentVariableA("X2W_MEM_CENSUS_AT", text, sizeof(text));
    if (n && n < sizeof(text))
        for (char* p = text; *p;) {
            char* end = nullptr;
            const ULONGLONG v = std::strtoull(p, &end, 10);
            if (end == p) break;
            census_at.push_back(v);
            p = *end ? end + 1 : end;
        }
    std::sort(census_at.begin(), census_at.end());
    ULONGLONG next_report = 0;  // a first line at once: a run may end within seconds
    for (;;) {
        const ULONGLONG second = (GetTickCount64() - tick_at_load) / 1000;
        if (second >= next_report) {
            report(second);
            next_report = second < 30 ? second + 2 : second + 10;
        }
        if (!census_at.empty() && second >= census_at.front()) {
            census_at.erase(census_at.begin());
            census(second);
        }
        Sleep(250);
    }
}
}  // namespace

namespace x2w_census {
void note_block(void* base, SIZE_T bytes) noexcept
{
    AcquireSRWLockExclusive(&block_lock);
    try {
        if (!blocks) blocks = new std::vector<std::pair<ULONG_PTR, SIZE_T>>;
        blocks->emplace_back(reinterpret_cast<ULONG_PTR>(base), bytes);
    } catch (...) {
    }
    ReleaseSRWLockExclusive(&block_lock);
}

void start() noexcept
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](INIT_ONCE*, void*, void**) -> BOOL {
        char value[4]{};
        const DWORD saved = GetLastError();
        // Startup runs only (the debugger sets X2W_TRAP_LOG): tests that load xmem.dll get no thread.
        const bool off = (GetEnvironmentVariableA("X2W_MEM_REPORT", value, sizeof(value)) && value[0] == '0') ||
                         !GetEnvironmentVariableA("X2W_TRAP_LOG", nullptr, 0);
        if (!off) {
            // Pin the module: the thread runs until the process ends.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&reporter), &self);
            if (HANDLE thread = CreateThread(nullptr, 64 << 10, reporter, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr))
                CloseHandle(thread);
        }
        SetLastError(saved);
        return TRUE;
    }, nullptr, nullptr);
}
}  // namespace x2w_census
