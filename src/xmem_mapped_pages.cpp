#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "era_physical.h"
#include "xmem_low.h"

#include <cstring>
#include "startup_stubs/local_log.h"

#include <vector>

#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace {
struct Backing {
    HANDLE section;
    ULONG_PTR views = 0;
    ULONG64 offset = 0;    // byte offset of the page in its section
    bool shared = false;   // the section is a chunk shared by many pages (not closed per page)
};

// LOCAL EXPERIMENT (X2W_XMEM_CHUNKS=1): pages live in 1 GiB committed sections (16384 pages each,
// page p at (p % 16384) * 64 KiB of chunk p / 16384) instead of one section per page. A desktop GPU
// heap can be opened over a whole chunk (OpenExistingHeapFromFileMapping accepts 1 GiB, not 4 GiB),
// so the GPU reads and writes the same pages the title maps. x2w_xmem_gpu_location() names the
// chunk and offset behind a mapped address.
constexpr ULONG_PTR chunk_pages = 16384;
bool chunks_on()
{
    static const bool on = [] {
        char value[8];
        const DWORD n = GetEnvironmentVariableA("X2W_XMEM_CHUNKS", value, sizeof value);
        return n && n < sizeof value && value[0] == '1';
    }();
    return on;
}
HANDLE chunk_sections[(std::uint64_t{1} << 18) / chunk_pages];

// Keep grant transitions and section views serialized. A grant can outlive all its
// views so its bytes survive unmapping. Its backing is released before reuse.
struct PhysicalMemory {
    std::mutex mutex;
    std::map<ULONG_PTR, Backing> backings;
    std::map<ULONG_PTR, ULONG_PTR> views;
    std::map<ULONG_PTR, SIZE_T> reservations;
    // Local experiment: a no-access graphics reservation (protection 0x801) made right after a
    // read-write one of the same size is that memory's GPU view on the console. Pages mapped into
    // the CPU view are mapped at the same offset into the GPU view too (the same section).
    ULONG_PTR last_cpu_base = 0;
    SIZE_T last_cpu_bytes = 0;
    ULONG_PTR mirror_cpu = 0, mirror_gpu = 0;
    SIZE_T mirror_bytes = 0;

    ~PhysicalMemory()
    {
        for (const auto& view : views)
            UnmapViewOfFile(reinterpret_cast<void*>(view.first));
        for (const auto& backing : backings)
            if (!backing.second.shared) CloseHandle(backing.second.section);
    }
};

PhysicalMemory memory;
constexpr SIZE_T page_bytes = era_physical::page_bytes;

BOOL physical_result(era_physical::Status status) noexcept
{
    switch (status) {
    case era_physical::Status::ok:
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    case era_physical::Status::exhausted:
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

// LOCAL EXPERIMENT (X2W_XMEM_LAZY=1): sections are reserved, not committed, so a page counts
// against system commit only once it is touched. A touch through a mapped view faults; the vectored
// handler below commits the page there (the commit belongs to the section, so every view sees it).
// The shim's own writes commit first. Kernel-mode accesses (ReadFile into such a page) do not fault
// and fail instead; none are expected on these pages.
bool lazy_commit()
{
    static const bool on = [] {
        char value[8];
        const DWORD n = GetEnvironmentVariableA("X2W_XMEM_LAZY", value, sizeof value);
        return n && n < sizeof value && value[0] == '1';
    }();
    return on;
}
LONG lazy_commits;

HANDLE section_for(ULONG_PTR page)
{
    const auto existing = memory.backings.find(page);
    if (existing != memory.backings.end())
        return existing->second.section;
    if (chunks_on()) {
        const ULONG_PTR chunk = page / chunk_pages;
        if (chunk >= std::size(chunk_sections)) return nullptr;
        if (!chunk_sections[chunk]) {
            const ULONG64 bytes = ULONG64{chunk_pages} * page_bytes;
            chunk_sections[chunk] = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                                       static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
            x2w_local::logf("x2w-xmem chunk %llu section %p", static_cast<unsigned long long>(chunk), chunk_sections[chunk]);
            if (!chunk_sections[chunk]) return nullptr;
        }
        Backing backing{chunk_sections[chunk]};
        backing.offset = ULONG64{page % chunk_pages} * page_bytes;
        backing.shared = true;
        memory.backings.emplace(page, backing);
        return chunk_sections[chunk];
    }
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | (lazy_commit() ? SEC_RESERVE : 0),
                                              0, page_bytes, nullptr);
    if (section) {
        try {
            memory.backings.emplace(page, Backing{section});
        } catch (const std::bad_alloc&) {
            CloseHandle(section);
            throw;
        }
    }
    return section;
}

auto owned_reservation(ULONG_PTR address)
{
    auto it = memory.reservations.upper_bound(address);
    if (it == memory.reservations.begin()) return memory.reservations.end();
    --it;
    return address - it->first < it->second ? it : memory.reservations.end();
}

bool map_view(ULONG_PTR address, HANDLE section, ULONG64 offset = 0)
{
    auto* target = reinterpret_cast<void*>(address);
    auto map = [&] {
        return MapViewOfFile3(section, GetCurrentProcess(), target, offset,
                              page_bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != nullptr;
    };
    if (map()) return true;
    if (owned_reservation(address) == memory.reservations.end()) return false;
    return VirtualFree(target, page_bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) && map();
}

bool unmap_view(ULONG_PTR address)
{
    return UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(address),
                           MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

void coalesce_unused(const std::map<ULONG_PTR, ULONG_PTR>& batch)
{
    auto previous = memory.reservations.end();
    for (const auto& entry : batch) {
        const auto reservation = owned_reservation(entry.first);
        if (reservation == memory.reservations.end() || reservation == previous) continue;
        previous = reservation;
        const auto [base, size] = *reservation;
        const auto view = memory.views.lower_bound(base);
        if (view == memory.views.end() || view->first - base >= size)
            VirtualFree(reinterpret_cast<void*>(base), size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    }
}

// Destinations must be caller-owned, page-sized Windows placeholders. Live views
// are owned by this shim until it unmaps them. The caller must not replace them.
template <typename AddressAt>
BOOL map_pages(ULONG_PTR count, const ULONG_PTR* tokens, const AddressAt& address_at) noexcept
{
    if (!count || count > era_physical::capacity_pages)
        return physical_result(era_physical::Status::invalid);
    try {
        const std::lock_guard guard(memory.mutex);
        std::map<ULONG_PTR, ULONG_PTR> batch;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const auto address = address_at(i);
            const auto live = memory.views.find(address);
            if (!address || address % page_bytes ||
                (tokens ? live != memory.views.end() || !era_physical::owns(tokens[i])
                        : live == memory.views.end()))
                return physical_result(era_physical::Status::invalid);
            if (!batch.emplace(address, tokens ? tokens[i] : live->second).second)
                return physical_result(era_physical::Status::invalid);
        }
        if (tokens) {
            for (const auto& [address, page] : batch) {
                if (!section_for(page)) return FALSE;
            }
            auto next = batch.begin();
            for (; next != batch.end(); ++next) {
                if (!map_view(next->first, memory.backings.at(next->second).section, memory.backings.at(next->second).offset)) {
                    const DWORD error = GetLastError();
                    while (next != batch.begin()) {
                        const auto previous = --next;
                        if (!unmap_view(previous->first)) {
                            ++memory.backings.at(previous->second).views;
                            const auto position = std::next(previous);
                            memory.views.insert(batch.extract(previous));
                            next = position;
                        }
                    }
                    coalesce_unused(batch);
                    SetLastError(error);
                    return FALSE;
                }
            }
            for (const auto& [address, page] : batch)
                ++memory.backings.at(page).views;
            memory.views.merge(batch);
        } else {
            auto next = batch.begin();
            for (; next != batch.end(); ++next) {
                if (!unmap_view(next->first)) {
                    const DWORD error = GetLastError();
                    while (next != batch.begin()) {
                        const auto& [address, page] = *--next;
                        if (!map_view(address, memory.backings.at(page).section, memory.backings.at(page).offset)) {
                            --memory.backings.at(page).views;
                            memory.views.erase(address);
                        }
                    }
                    SetLastError(error);
                    return FALSE;
                }
            }
            for (const auto& [address, page] : batch) {
                --memory.backings.at(page).views;
                memory.views.erase(address);
            }
            coalesce_unused(batch);
        }
    } catch (const std::bad_alloc&) {
        return physical_result(era_physical::Status::exhausted);
    }
    return physical_result(era_physical::Status::ok);
}
}

// ---------------------------------------------------------------------------------------------
// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed). With X2W_XMEM_PERMISSIVE=1 the exports
// below accept the call shapes of a local startup run on top of the section-backed pages above.
// Observations behind it:
// - Page numbers: the caller rebuilds them as (21-bit entry << 4), so it expects 4 KiB frame numbers
//   with the low four bits clear. Callers get page << 4 and pass it back.
// - XMemAllocatePhysicalPages flags other than MEM_64K_PAGES occur (0x280) and are served the same.
// - XMemVirtualAlloc: commits (type 0x20003000/0x20403000) become plain commits; reservations become
//   placeholders that pages can be mapped into; a "replace placeholder" call (0x20406000) on a piece
//   the caller split from its own placeholder keeps the placeholder and records it for mapping.
// - Scatter mapping passes page 0 to unmap and may name pages that are already mapped elsewhere
//   (a second view; the sections give real sharing).
namespace {
bool local_permissive() noexcept { return x2w_local::env_on("X2W_XMEM_PERMISSIVE"); }

void record_reservation(void* base, SIZE_T bytes)
{
    const std::lock_guard guard(memory.mutex);
    memory.reservations.insert_or_assign(reinterpret_cast<ULONG_PTR>(base), bytes);
}

// LOCAL EXPERIMENT (X2W_XMEM_LAZY_POOLS=1): the title commits a few pools of 0.5-2.2 GiB up front
// (measured: 4.5 GiB at startup, most never touched before loading runs out of system commit).
// Pools of 256 MiB or more are reserved instead; the first touch of a 64 KiB chunk faults and the
// handler commits it with the pool's protection. None of these pools is aliased by a D3D12 heap
// (every placed resource on them falls back to its own buffer), so no driver needs them committed.
// Kernel-mode writes (ReadFile straight into a pool) would fail instead of faulting.
struct LazyPool { ULONG_PTR base; SIZE_T bytes; DWORD protect; };
SRWLOCK lazy_pools_lock = SRWLOCK_INIT;
std::vector<LazyPool> lazy_pools;
LONG lazy_pool_commits;
bool lazy_pools_on()
{
    static const bool on = x2w_local::env_on("X2W_XMEM_LAZY_POOLS");
    return on;
}
LONG CALLBACK lazy_pool_handler(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = record->ExceptionInformation[1];
    DWORD protect = 0;
    AcquireSRWLockShared(&lazy_pools_lock);
    for (const LazyPool& pool : lazy_pools)
        if (address >= pool.base && address - pool.base < pool.bytes) protect = pool.protect;
    ReleaseSRWLockShared(&lazy_pools_lock);
    if (!protect) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR chunk = address & ~ULONG_PTR{0xFFFF};
    if (!VirtualAlloc(reinterpret_cast<void*>(chunk), 0x10000, MEM_COMMIT, protect)) return EXCEPTION_CONTINUE_SEARCH;
    const LONG n = InterlockedIncrement(&lazy_pool_commits);
    if ((n & (n - 1)) == 0) x2w_local::logf("x2w-xmem lazy pool commit #%ld (%ld MiB committed on touch)", n, n / 16);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void* local_virtual_alloc(void* address, SIZE_T bytes, DWORD type, ULONGLONG attributes, DWORD protection)
{
    const DWORD low = protection & 0xFF;
    const bool accessible = low == PAGE_READONLY || low == PAGE_READWRITE || low == PAGE_EXECUTE_READ ||
                            low == PAGE_EXECUTE_READWRITE;
    const DWORD protect = accessible ? low : PAGE_READWRITE;
    void* result = nullptr;
    const char* how = "";
    if ((type & MEM_REPLACE_PLACEHOLDER) && address) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_RESERVE) {
            record_reservation(address, bytes);
            result = address;
        }
        how = "placeholder kept for page mapping";
    } else if (type & MEM_COMMIT) {
        if (!address && lazy_pools_on() && bytes >= (SIZE_T{256} << 20)) {
            static const bool installed = AddVectoredExceptionHandler(1, lazy_pool_handler) != nullptr;
            result = installed ? x2w_low::alloc(bytes, MEM_RESERVE, protect) : nullptr;
            if (result) {
                AcquireSRWLockExclusive(&lazy_pools_lock);
                lazy_pools.push_back({reinterpret_cast<ULONG_PTR>(result), bytes, protect});
                ReleaseSRWLockExclusive(&lazy_pools_lock);
            }
            how = "reserve, committed on touch";
        } else if (!address) {
            result = x2w_low::alloc(bytes, MEM_COMMIT | MEM_RESERVE, protect);
            how = "commit";
        } else {
            result = VirtualAlloc(address, bytes, MEM_COMMIT, protect);
            if (!result) {
                VirtualFree(address, bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
                result = VirtualAlloc2(GetCurrentProcess(), address, bytes,
                                       MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
            }
            how = "commit at address";
        }
    } else {
        result = address ? VirtualAlloc2(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                         PAGE_NOACCESS, nullptr, 0)
                         : x2w_low::alloc(bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS);
        // A caller that asks for a placeholder splits it itself; other reservations are split here on mapping.
        if (result && !(type & MEM_RESERVE_PLACEHOLDER)) record_reservation(result, bytes);
        how = "placeholder reserve";
    }
    const DWORD error = result ? 0 : GetLastError();
    x2w_local::logf("x2w-xmem XMemVirtualAlloc addr=%p size=0x%llx type=0x%lx attr=0x%llx prot=0x%lx -> %p (%s, err %lu)",
                    address, static_cast<unsigned long long>(bytes), static_cast<unsigned long>(type),
                    static_cast<unsigned long long>(attributes), static_cast<unsigned long>(protection), result, how,
                    static_cast<unsigned long>(error));
    SetLastError(error);
    return result;
}

// Applies (address, page) pairs; page 0 unmaps. An address that already shows the requested page is
// left alone; one that shows another page is unmapped first.
BOOL local_apply(ULONG_PTR count, const ULONG_PTR* addresses, const ULONG_PTR* pages, const char* what)
{
    std::map<ULONG_PTR, ULONG_PTR> want;
    for (ULONG_PTR i = 0; i < count; ++i) want[addresses[i]] = pages[i];
    std::vector<ULONG_PTR> unmap, map_at, map_page;
    {
        const std::lock_guard guard(memory.mutex);
        for (const auto& [address, page] : want) {
            const auto live = memory.views.find(address);
            if (!page) {
                if (live != memory.views.end()) unmap.push_back(address);
                continue;
            }
            if (live != memory.views.end()) {
                if (live->second == page) continue;
                unmap.push_back(address);
            }
            map_at.push_back(address);
            map_page.push_back(page);
        }
    }
    BOOL ok = TRUE;
    DWORD error = 0;
    if (!unmap.empty() && !map_pages(unmap.size(), nullptr, [&](ULONG_PTR i) { return unmap[i]; })) {
        ok = FALSE;
        error = GetLastError();
    }
    if (ok && !map_at.empty() &&
        !map_pages(map_at.size(), map_page.data(), [&](ULONG_PTR i) { return map_at[i]; })) {
        ok = FALSE;
        error = GetLastError();
    }
    static LONG calls;
    if (!ok || x2w_local::first_calls(calls, 16))
        x2w_local::logf("x2w-xmem %s count=0x%llx first=%p map=%llu unmap=%llu -> %d (err %lu)", what,
                        static_cast<unsigned long long>(count), count ? reinterpret_cast<void*>(addresses[0]) : nullptr,
                        static_cast<unsigned long long>(map_at.size()), static_cast<unsigned long long>(unmap.size()), ok,
                        static_cast<unsigned long>(error));
    SetLastError(error);
    return ok;
}

// Applies the same (address, page) pairs again at the GPU view for those addresses in the CPU view.
void local_mirror(ULONG_PTR count, const ULONG_PTR* addresses, const ULONG_PTR* pages)
{
    ULONG_PTR cpu, gpu;
    SIZE_T bytes;
    {
        const std::lock_guard guard(memory.mutex);
        cpu = memory.mirror_cpu, gpu = memory.mirror_gpu, bytes = memory.mirror_bytes;
    }
    if (!cpu) return;
    std::vector<ULONG_PTR> at, page;
    for (ULONG_PTR i = 0; i < count; ++i)
        if (addresses[i] >= cpu && addresses[i] - cpu < bytes) {
            at.push_back(addresses[i] - cpu + gpu);
            page.push_back(pages[i]);
        }
    if (!at.empty()) local_apply(at.size(), at.data(), page.data(), "GPU view mirror");
}

bool to_pages(ULONG_PTR count, const ULONG_PTR* tokens, std::vector<ULONG_PTR>& pages)
{
    pages.resize(count);
    for (ULONG_PTR i = 0; i < count; ++i) {
        if (tokens[i] & 0xF) return false;
        pages[i] = tokens[i] >> 4;
    }
    return true;
}
}  // namespace

namespace {
BOOL free_pages(ULONG_PTR count, PULONG_PTR pages);
}

extern "C" BOOL WINAPI XMemAllocatePhysicalPages(ULONG_PTR flags, PULONG_PTR count, PULONG_PTR pages)
{
    static_assert(sizeof(ULONG_PTR) == sizeof(std::uint64_t));
    if (local_permissive()) {
        const ULONG_PTR requested = count ? *count : 0;
        era_physical::Status status;
        {
            const std::lock_guard guard(memory.mutex);
            status = era_physical::allocate(era_physical::mem_64k_pages, reinterpret_cast<std::uint64_t*>(count),
                                            reinterpret_cast<std::uint64_t*>(pages));
        }
        if (status == era_physical::Status::ok)
            for (ULONG_PTR i = 0; i < *count; ++i) pages[i] <<= 4;
        x2w_local::logf("x2w-xmem XMemAllocatePhysicalPages flags=0x%llx pages=0x%llx -> %d",
                        static_cast<unsigned long long>(flags), static_cast<unsigned long long>(requested),
                        status == era_physical::Status::ok);
        return physical_result(status);
    }
    const std::lock_guard guard(memory.mutex);
    return physical_result(era_physical::allocate(flags, reinterpret_cast<std::uint64_t*>(count),
                                                  reinterpret_cast<std::uint64_t*>(pages)));
}

extern "C" BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR pages)
{
    if (local_permissive() && pages && count && count <= era_physical::capacity_pages) {
        std::vector<ULONG_PTR> converted;
        if (!to_pages(count, pages, converted)) return physical_result(era_physical::Status::invalid);
        return free_pages(count, converted.data());
    }
    return free_pages(count, pages);
}

namespace {
BOOL free_pages(ULONG_PTR count, PULONG_PTR pages)
{
    if (!pages || !count || count > era_physical::capacity_pages)
        return physical_result(era_physical::Status::invalid);
    const std::lock_guard guard(memory.mutex);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto backing = memory.backings.find(pages[i]);
        if (backing != memory.backings.end() && backing->second.views)
            return physical_result(era_physical::Status::invalid);
    }
    const auto status = era_physical::release(count, reinterpret_cast<const std::uint64_t*>(pages));
    if (status == era_physical::Status::ok) {
        for (ULONG_PTR i = 0; i < count; ++i) {
            const auto backing = memory.backings.find(pages[i]);
            if (backing != memory.backings.end()) {
                if (!backing->second.shared) CloseHandle(backing->second.section);
                memory.backings.erase(backing);
            }
        }
    }
    return physical_result(status);
}
}  // namespace

// Local experiment export (local/startup-v2): writes bytes into physical pages named by the title's
// page tokens (page number << 4, as XMemMapPhysicalPages takes them), starting offset bytes into the
// first page. DirectStorage requests can name such pages as their destination.
extern "C" BOOL WINAPI x2w_xmem_write_physical(const ULONG_PTR* tokens, ULONG_PTR count, SIZE_T offset, const void* data, SIZE_T bytes)
{
    if (!tokens || !data || offset >= page_bytes) return FALSE;
    const auto* source = static_cast<const unsigned char*>(data);
    ULONG_PTR index = 0;
    while (bytes) {
        if (index >= count || (tokens[index] & 0xF)) return FALSE;
        HANDLE section = nullptr;
        ULONG64 section_offset = 0;
        {
            const std::lock_guard guard(memory.mutex);
            section = section_for(tokens[index] >> 4);
            if (section) section_offset = memory.backings.at(tokens[index] >> 4).offset;
        }
        if (!section) return FALSE;
        void* view = MapViewOfFile(section, FILE_MAP_WRITE, static_cast<DWORD>(section_offset >> 32), static_cast<DWORD>(section_offset),
                                   page_bytes);
        if (!view) return FALSE;
        if (lazy_commit() && !VirtualAlloc(view, page_bytes, MEM_COMMIT, PAGE_READWRITE)) {
            UnmapViewOfFile(view);
            return FALSE;
        }
        const SIZE_T chunk = bytes < page_bytes - offset ? bytes : page_bytes - offset;
        std::memcpy(static_cast<unsigned char*>(view) + offset, source, chunk);
        UnmapViewOfFile(view);
        source += chunk;
        bytes -= chunk;
        offset = 0;
        ++index;
    }
    return TRUE;
}

namespace {
// First touch of a reserved section page through a mapped view: commit that page and retry.
LONG CALLBACK lazy_commit_handler(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = record->ExceptionInformation[1];
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.Type != MEM_MAPPED || mbi.State != MEM_RESERVE)
        return EXCEPTION_CONTINUE_SEARCH;
    // A mapped view that is still reserved can only come from the SEC_RESERVE sections above.
    const ULONG_PTR page = address & ~(ULONG_PTR{page_bytes} - 1);
    if (!VirtualAlloc(reinterpret_cast<void*>(page), page_bytes, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    const LONG n = InterlockedIncrement(&lazy_commits);
    if ((n & (n - 1)) == 0) x2w_local::logf("x2w-xmem lazy commit #%ld (%ld MiB committed on touch)", n, n / 16);
    return EXCEPTION_CONTINUE_EXECUTION;
}
const bool lazy_handler_installed = lazy_commit() && AddVectoredExceptionHandler(1, lazy_commit_handler) != nullptr;

// LOCAL (X2W_XMEM_LAZY=1): the kernel writes ReadFile's buffer without faulting, so a read into a
// still-reserved section page fails (ERROR_NOACCESS) instead of committing it. The title's ReadFile
// import is wrapped: reserved mapped pages in the buffer are committed first, then the call goes on.
LONG read_commits;
void commit_reserved_views(void* buffer, DWORD bytes)
{
    auto address = reinterpret_cast<ULONG_PTR>(buffer);
    const ULONG_PTR end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi)) return;
        const ULONG_PTR region_end = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.Type == MEM_MAPPED && mbi.State == MEM_RESERVE) {
            const ULONG_PTR first = address & ~(ULONG_PTR{page_bytes} - 1);
            const ULONG_PTR last = (end < region_end ? end : region_end);
            for (ULONG_PTR page = first; page < last; page += page_bytes)
                if (VirtualAlloc(reinterpret_cast<void*>(page), page_bytes, MEM_COMMIT, PAGE_READWRITE)) {
                    const LONG n = InterlockedIncrement(&read_commits);
                    if ((n & (n - 1)) == 0) x2w_local::logf("x2w-xmem lazy commit before ReadFile #%ld", n);
                }
        }
        address = region_end;
    }
}
BOOL WINAPI lazy_read_file(HANDLE file, void* buffer, DWORD bytes, DWORD* read, OVERLAPPED* overlapped)
{
    if (buffer && bytes) commit_reserved_views(buffer, bytes);
    return ReadFile(file, buffer, bytes, read, overlapped);
}
int patch_read_file()
{
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return 0;
    int patched = 0;
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name; ++import) {
        if (!import->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(by_name->Name, "ReadFile") != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) {
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&lazy_read_file);
                VirtualProtect(&slots->u1.Function, 8, old, &old);
                ++patched;
            }
        }
    }
    return patched;
}
}  // namespace

namespace {
// Runs during this DLL's initialisation; the loader has bound the main module's imports by then.
const bool read_file_patched = [] {
    if (!lazy_commit()) return false;
    x2w_local::logf("x2w-xmem lazy: %d ReadFile import(s) of the main module wrapped", patch_read_file());
    return true;
}();
}  // namespace

extern "C" void* WINAPI XMemMapPhysicalPages(void* base, ULONG_PTR count, const ULONG_PTR* tokens)
{
    const auto address = reinterpret_cast<ULONG_PTR>(base);
    if (!address || count > (std::numeric_limits<ULONG_PTR>::max() - address) / page_bytes) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (local_permissive()) {
        // A null array unmaps (seen on a cleanup path, also with a zero count).
        std::vector<ULONG_PTR> addresses(count), pages(count, 0);
        for (ULONG_PTR i = 0; i < count; ++i) addresses[i] = address + i * page_bytes;
        if (tokens && !to_pages(count, tokens, pages)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        if (!count) return base;
        if (!local_apply(count, addresses.data(), pages.data(), "XMemMapPhysicalPages")) return nullptr;
        local_mirror(count, addresses.data(), pages.data());
        return base;
    }
    return map_pages(count, tokens, [=](ULONG_PTR i) { return address + i * page_bytes; }) ? base : nullptr;
}

extern "C" BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, ULONG_PTR count,
                                                   const ULONG_PTR* tokens)
{
    if (!addresses) return physical_result(era_physical::Status::invalid);
    if (local_permissive() && tokens) {
        std::vector<ULONG_PTR> pages;
        if (!to_pages(count, tokens, pages)) return physical_result(era_physical::Status::invalid);
        if (!local_apply(count, reinterpret_cast<const ULONG_PTR*>(addresses), pages.data(), "XMemMapPhysicalPagesScatter"))
            return FALSE;
        local_mirror(count, reinterpret_cast<const ULONG_PTR*>(addresses), pages.data());
        return TRUE;
    }
    return map_pages(count, tokens, [=](ULONG_PTR i) { return reinterpret_cast<ULONG_PTR>(addresses[i]); });
}

// Reserve-only desktop profile from the source-built #888 comparison.
extern "C" void* WINAPI XMemVirtualAlloc(void* address, SIZE_T bytes, DWORD allocation_type,
                                        ULONGLONG attributes, DWORD protection)
{
    constexpr DWORD startup_reserve = 0x20402000;
    constexpr SIZE_T gib = SIZE_T{1} << 30;
    const bool supported = (attributes == 0x21 && protection == PAGE_READWRITE)
                           || (attributes == 3 && protection == (0x800 | PAGE_NOACCESS));
    if (address || (bytes != 16 * gib && bytes != 24 * gib) || allocation_type != startup_reserve || !supported) {
        if (local_permissive()) return local_virtual_alloc(address, bytes, allocation_type, attributes, protection);
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const std::lock_guard guard(memory.mutex);
    // Callers release unused reservations with Win32. Drop entries for released
    // ranges before another allocation can reuse those virtual addresses.
    for (auto it = memory.reservations.begin(); it != memory.reservations.end();) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(it->first), &info, sizeof(info)) && info.State == MEM_FREE)
            it = memory.reservations.erase(it);
        else
            ++it;
    }
    auto* reservation = x2w_low::alloc(bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS);
    if (!reservation) return nullptr;
    try {
        memory.reservations.insert_or_assign(reinterpret_cast<ULONG_PTR>(reservation), bytes);
        const auto base = reinterpret_cast<ULONG_PTR>(reservation);
        if (protection == PAGE_READWRITE) {
            memory.last_cpu_base = base;
            memory.last_cpu_bytes = bytes;
        } else if (memory.last_cpu_base && memory.last_cpu_bytes == bytes && x2w_local::env_on("X2W_XMEM_GPU_MIRROR")) {
            memory.mirror_cpu = memory.last_cpu_base;
            memory.mirror_gpu = base;
            memory.mirror_bytes = bytes;
            x2w_local::logf("x2w-xmem GPU view %p (+0x%llx) mirrors CPU view %p, 0x%llx bytes", reservation,
                            static_cast<unsigned long long>(base - memory.last_cpu_base), reinterpret_cast<void*>(memory.last_cpu_base),
                            static_cast<unsigned long long>(bytes));
        }
    } catch (const std::bad_alloc&) {
        VirtualFree(reservation, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reservation;
}

// Local experiment export: the section (a 1 GiB chunk, X2W_XMEM_CHUNKS=1) and byte offset behind a
// mapped title address. FALSE when the address is not in a mapped page or chunks are off.
extern "C" BOOL WINAPI x2w_xmem_gpu_location(const void* address, HANDLE* section, ULONG64* offset)
{
    if (!chunks_on() || !section || !offset) return FALSE;
    const auto at = reinterpret_cast<ULONG_PTR>(address);
    const std::lock_guard guard(memory.mutex);
    const auto view = memory.views.find(at - at % page_bytes);
    if (view == memory.views.end()) return FALSE;
    const auto backing = memory.backings.find(view->second);
    if (backing == memory.backings.end() || !backing->second.shared) return FALSE;
    *section = backing->second.section;
    *offset = backing->second.offset + at % page_bytes;
    return TRUE;
}
