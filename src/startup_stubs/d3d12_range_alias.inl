// LOCAL indirect (board msgs/064900, compute's ring-binding proposal): X2W_RANGE_ALIAS=1. Default off.
// Included in d3d12_xs_device.cpp's anonymous namespace after d3d12_compute.inl (desktop_range calls in here).
//
// desktop_range refuses a buffer range of chunk pages whose base has a window but whose 64 KiB pages are not at
// consecutive offsets of ONE section ("split"; run 112: ok 2,460,271, split 130,383). A whole-range binding of such a
// range binds NULL (d3d12_compute.inl). The proposal is to map one reserved buffer's tiles onto the section heaps.
//
// That mechanism is refused by the D3D12 runtime, MEASURED on 2026-11-25 (tests/tile_mapping_shared_heap_check.cpp):
// - the heaps section_heap() opens (OpenExistingHeapFromFileMapping) carry D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER
//   (flags 0x421), as do OpenExistingHeapFromAddress heaps;
// - UpdateTileMappings onto such a heap removes the device (reason 0x887a0001, debug id 493 "UpdateTileMappings does
//   not work with cross-adapter shared heaps"), with and without the debug layer, on WARP and on the RX 9070 XT.
//   The same mapping onto CreateHeap heaps (flags 0xc0) is exact on both.
// So no tile mapping is ever issued here. With the switch on, this file:
// - checks the capabilities once (tiled resources tier, the section heap's flags) and says why it refuses;
// - keeps the binding NULL, as without the switch (a wrong alias is worse than NULL), and counts every binding it
//   affects (the frame-cached repeats too);
// - describes each distinct split range once (capped): pages, linear runs, sections, first break (page index, section
//   change or offset jump), so the next step (xmem keeping such ranges in one section, a shadow copy, ...) can be
//   chosen from measured layouts.
// Counters: built (always 0), reused (always 0), failed (bindings), ranges (distinct).
bool range_alias_on()
{
    static const bool on = x2w_local::env_on("X2W_RANGE_ALIAS");
    return on;
}

LONG range_alias_built, range_alias_reused, range_alias_failed, range_alias_ranges;

namespace range_alias {
using Location = BOOL(WINAPI*)(const void*, HANDLE*, ULONG64*);
Location location()
{
    static const Location f = [] {
        const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
        return xmem ? reinterpret_cast<Location>(GetProcAddress(xmem, "x2w_xmem_gpu_location")) : nullptr;
    }();
    return f;
}

// The gate, checked once (on the first split range). The reason text goes on every refusal line.
const char* refusal(HANDLE first_section)
{
    static const char* reason = nullptr;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (!reason) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const HRESULT got = device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        ID3D12Heap* heap = first_section ? section_heap(first_section) : nullptr;
        const D3D12_HEAP_FLAGS flags = heap ? heap->GetDesc().Flags : D3D12_HEAP_FLAG_NONE;
        if (FAILED(got) || options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1)
            reason = "no tiled resources (tier below 1)";
        else if (!heap)
            reason = "the section has no desktop heap";
        else if (flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)
            reason = "section heaps are cross-adapter shared: UpdateTileMappings onto them removes the device "
                     "(tests/tile_mapping_shared_heap_check)";
        else
            reason = "section heap is not cross-adapter, but this build has no alias builder";
        log("x2w-range-alias: X2W_RANGE_ALIAS=1; tiled resources tier %d (0x%08lx), section heap %p flags 0x%x; xmem location %s: "
            "REFUSED (%s). Split ranges stay NULL, as without the switch.",
            SUCCEEDED(got) ? static_cast<int>(options.TiledResourcesTier) : -1, static_cast<unsigned long>(got), static_cast<void*>(heap),
            static_cast<unsigned>(flags), location() ? "found" : "MISSING", reason);
    }
    ReleaseSRWLockExclusive(&lock);
    return reason;
}

void summary()
{
    log("x2w-range-alias: built %ld, reused %ld, failed %ld bindings over %ld distinct ranges (frame %ld)", range_alias_built, range_alias_reused,
        range_alias_failed, range_alias_ranges, static_cast<long>(frame_number));
}

void note_failed()
{
    const LONG n = InterlockedIncrement(&range_alias_failed);
    if ((n & (n - 1)) == 0 && n >= 64) summary();  // 64, 128, 256, ...
}
}  // namespace range_alias

// desktop_range: a binding of a frame-cached split range (counted, no line).
void range_alias_repeat()
{
    range_alias::note_failed();
}

// desktop_range: [address, address + bytes) is chunk pages whose base has a window and which failed the per-page
// check. Describes the range (first sighting, capped), refuses, counts. Always returns false (no alias).
bool range_alias_split(u64 address, u64 bytes)
{
    const range_alias::Location location = range_alias::location();
    HANDLE first_section = nullptr;
    ULONG64 first_at = 0;
    if (location) location(reinterpret_cast<const void*>(address), &first_section, &first_at);
    const char* reason = range_alias::refusal(first_section);
    range_alias::note_failed();
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_set<u64> seen;
    const u64 key = address * 1099511628211ull ^ bytes;
    AcquireSRWLockExclusive(&lock);
    const bool fresh = seen.size() < 4096 && seen.insert(key).second;
    ReleaseSRWLockExclusive(&lock);
    if (!fresh) return false;
    const LONG distinct = InterlockedIncrement(&range_alias_ranges);
    if (!x2w_local::first_of(distinct, 48) || !location) return false;
    // The page walk: one location per 64 KiB page (as desktop_range's check), from the range's first page.
    const u64 first_page = address & ~0xffffull, end = address + bytes;
    std::vector<HANDLE> sections;
    unsigned long pages = 0, runs = 0, missing = 0;
    long first_break = -1;
    const char* break_kind = "";
    long long jump = 0;
    HANDLE previous = nullptr;
    ULONG64 previous_at = 0;
    for (u64 at = first_page; at < end; at += 0x10000, ++pages) {
        HANDLE section = nullptr;
        ULONG64 where = 0;
        if (!location(reinterpret_cast<const void*>(at), &section, &where)) {
            ++missing;
            previous = nullptr;
            continue;
        }
        where &= ~0xffffull;
        const bool linear = previous && section == previous && where == previous_at + 0x10000;
        if (!linear) {
            ++runs;
            if (previous && first_break < 0) {
                first_break = static_cast<long>(pages);
                break_kind = section != previous ? "section change" : "offset jump";
                jump = static_cast<long long>(where) - static_cast<long long>(previous_at + 0x10000);
            }
        }
        if (std::find(sections.begin(), sections.end(), section) == sections.end()) sections.push_back(section);
        previous = section;
        previous_at = where;
    }
    char where_broken[96] = "none";
    if (first_break >= 0)
        std::snprintf(where_broken, sizeof(where_broken), "page %ld, %s %s0x%llx", first_break, break_kind, jump < 0 ? "-" : "+",
                      static_cast<unsigned long long>(jump < 0 ? -jump : jump));
    log("x2w-range-alias range 0x%llx %llu bytes (frame %ld): %lu pages, %lu linear runs, %zu sections, %lu unmapped; first break: %s: NULL (%s)",
        address, static_cast<unsigned long long>(bytes), static_cast<long>(frame_number), pages, runs, sections.size(), missing, where_broken, reason);
    return false;
}
