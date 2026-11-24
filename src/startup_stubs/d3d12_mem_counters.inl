// LOCAL memcut (out/gamerun/memcut_work/NOTES.md). Included once after the stand-in's anonymous
// namespace in d3d12_xs_device.cpp. Exports x2w_memory_counters (xmem_census.h): the x2w-mem
// reporter (xmem.dll's thread) calls it every 2-10 s and appends the short text to its line; at most
// every 10 s a detail line "x2w-memcut detail" is logged as well.
//
// What it counts: every desktop resource and heap the stand-in holds for a wrapper (the registry),
// by class, by heap kind (local = default heap; system = upload/readback/custom-L0 heaps and placed
// resources on heaps over title memory), live or released by the title (refs <= 0: release() keeps
// the desktop object for the process), plus the tiled-address SRV textures materialized on first
// sample, the upload ring use per frame and the frame-copy snapshots. Sizes are the desktop
// GetResourceAllocationInfo size of the resource's own description (cached per wrapper; a wrapper
// address reused with another desktop object is classified again). For live local textures it also
// sums the title address spans they stand for: "sum" vs "union" shows textures placed over the same
// title pages (each held separately here), "in mapped" the part over slab (mapped) memory.
// Placed resources on title-created heaps (device slot 29) would be counted both in their heap and
// as a resource (none seen in run 82).
// Locks: the registry lock (shared) while copying wrapper fields and classifying new wrappers (D3D12
// calls only); afterwards, separately, the note lock (shared) for buffer placements and the tiled-SRV
// mutex. No draw lock. Never calls xmem (x2w_xmem_gpu_location may move a slab).
namespace {
enum MemcutClass : int { mc_tex_target, mc_tex_uav, mc_tex, mc_buf_copied, mc_buf_aliased, mc_buf, mc_tiled, mc_heap, mc_count };
const char* const memcut_class_names[mc_count] = {"tex-target", "tex-uav", "tex", "buf-copied", "buf-aliased", "buf", "tiled-srv", "heap"};
struct MemcutEntry {
    const void* real;  // the desktop object classified (reclassify if the wrapper now holds another)
    u64 bytes;
    u64 title;         // the title address the resource stands for, or 0
    int cls;
    bool system;       // not a default (video-memory) heap
    bool placement_known;
    signed char mapped;  // title address in MEM_MAPPED memory: -1 not asked yet, 0 no, 1 yes
    DXGI_FORMAT format;
};
struct MemcutTotals {
    u64 bytes[mc_count][2][2]{};  // [class][system][dead]
    u64 count[mc_count][2][2]{};
    u64 tex_size_bytes[4]{}, tex_size_count[4]{};  // live local textures: <1, <16, <64, >=64 MiB
    std::unordered_map<int, u64> format_bytes;      // live local textures by desktop format
    std::vector<std::pair<u64, u64>> spans;         // live local textures: [title, title + bytes)
    u64 span_mapped = 0;
    u64 wrappers = 0, unsized = 0;
};
std::mutex memcut_mutex;
std::unordered_map<const void*, MemcutEntry>& memcut_cache()
{
    static std::unordered_map<const void*, MemcutEntry> m;
    return m;
}

bool memcut_system_heap(ID3D12Resource* resource)
{
    D3D12_HEAP_PROPERTIES props{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&props, &flags))) return false;  // reserved: no own memory
    if (props.Type == D3D12_HEAP_TYPE_DEFAULT) return false;
    if (props.Type == D3D12_HEAP_TYPE_CUSTOM) return props.MemoryPoolPreference != D3D12_MEMORY_POOL_L1;
    return true;
}

MemcutEntry memcut_classify(ID3D12Resource* resource, void* title)
{
    MemcutEntry e{};
    e.real = resource;
    e.mapped = -1;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_of(the_device)->GetResourceAllocationInfo(0, 1, &desc);
    e.bytes = info.SizeInBytes == ~0ull ? 0 : info.SizeInBytes;
    e.system = memcut_system_heap(resource);
    e.format = desc.Format;
    e.title = reinterpret_cast<u64>(title);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        e.cls = mc_buf;  // refined after the registry lock (memcut_placements)
    } else {
        e.placement_known = true;
        e.cls = desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? mc_tex_target
              : desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ? mc_tex_uav : mc_tex;
    }
    return e;
}

MemcutEntry memcut_heap(ID3D12Heap* heap)
{
    MemcutEntry e{};
    e.real = heap;
    e.mapped = 0;
    e.placement_known = true;
    const D3D12_HEAP_DESC desc = heap->GetDesc();
    e.bytes = desc.SizeInBytes;
    e.cls = mc_heap;
    e.system = desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD || desc.Properties.Type == D3D12_HEAP_TYPE_READBACK ||
               (desc.Properties.Type == D3D12_HEAP_TYPE_CUSTOM && desc.Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_L1);
    return e;
}

bool memcut_texture(int cls) { return cls == mc_tex || cls == mc_tex_target || cls == mc_tex_uav || cls == mc_tiled; }

void memcut_add(MemcutTotals& t, const MemcutEntry& e, bool dead)
{
    if (!e.bytes) ++t.unsized;
    t.bytes[e.cls][e.system][dead] += e.bytes;
    ++t.count[e.cls][e.system][dead];
    if (!dead && !e.system && memcut_texture(e.cls)) {
        const u64 m = e.bytes >> 20;
        const int s = m < 1 ? 0 : m < 16 ? 1 : m < 64 ? 2 : 3;
        t.tex_size_bytes[s] += e.bytes;
        ++t.tex_size_count[s];
        t.format_bytes[static_cast<int>(e.format)] += e.bytes;
        if (e.title) {
            t.spans.emplace_back(e.title, e.title + e.bytes);
            if (e.mapped == 1) t.span_mapped += e.bytes;
        }
    }
}

// Buffer placements (note lock) and title-memory kinds (VirtualQuery, slow here) for new entries,
// outside the registry lock. Keys are only looked up, never dereferenced.
void memcut_refine(const std::vector<const void*>& fresh)
{
    auto& cache = memcut_cache();
    for (const void* p : fresh) {
        MemcutEntry& e = cache.at(p);
        if (!e.placement_known) {
            Placement placement = Placement::none;
            AcquireSRWLockShared(&note_lock);
            if (const auto found = resource_infos().find(static_cast<const Wrapper*>(p)); found != resource_infos().end())
                placement = found->second.placement;
            ReleaseSRWLockShared(&note_lock);
            // A placement retired before this first look reads as none: such a buffer counts as "buf".
            e.cls = placement == Placement::aliased_buffer ? mc_buf_aliased : placement == Placement::copied_buffer ? mc_buf_copied : mc_buf;
            e.placement_known = true;
        }
        if (e.mapped < 0 && e.title && memcut_texture(e.cls)) {
            u64 base = 0, end = 0;
            MEMORY_BASIC_INFORMATION info{};
            if (known_private(e.title, &base, &end)) e.mapped = 0;
            else e.mapped = timed_query(reinterpret_cast<const void*>(e.title), &info) == sizeof(info) && info.Type == MEM_MAPPED;
        }
    }
}

void memcut_collect(MemcutTotals& t)
{
    std::lock_guard guard(memcut_mutex);
    auto& cache = memcut_cache();
    std::vector<std::pair<const void*, LONG>> seen;
    std::vector<const void*> fresh;
    AcquireSRWLockShared(&registry_lock);
    t.wrappers = registry().size();
    for (const void* p : registry()) {
        auto* w = const_cast<Wrapper*>(static_cast<const Wrapper*>(p));
        const Kind kind = w->own().kind;
        IUnknown* const real = w->own().real;
        if (!real || (kind != resource_kind && kind != heap_kind)) continue;
        auto found = cache.find(p);
        if (found == cache.end() || found->second.real != real) {
            const MemcutEntry e = kind == heap_kind ? memcut_heap(static_cast<ID3D12Heap*>(static_cast<void*>(real)))
                                                    : memcut_classify(static_cast<ID3D12Resource*>(static_cast<void*>(real)), w->own().title_memory);
            found = cache.insert_or_assign(p, e).first;
            fresh.push_back(p);
        }
        seen.emplace_back(p, InterlockedCompareExchange(&w->own().refs, 0, 0));
    }
    ReleaseSRWLockShared(&registry_lock);
    memcut_refine(fresh);
    for (const auto& [p, refs] : seen) memcut_add(t, cache.at(p), refs <= 0);

    struct Tiled { Wrapper* wrapper; ID3D12Resource* desktop; LONG refs; };
    std::vector<Tiled> tiled;
    {
        std::lock_guard lock(tiled_srv_mutex);  // entries are never erased; desktop textures never released
        for (auto& [wrapper, texture] : tiled_srv_textures)
            if (texture.desktop) tiled.push_back({wrapper, texture.desktop.Get(), InterlockedCompareExchange(&wrapper->own().refs, 0, 0)});
    }
    for (const Tiled& x : tiled) {
        auto found = cache.find(x.desktop);
        if (found == cache.end()) {
            MemcutEntry e = memcut_classify(x.desktop, x.wrapper->own().title_memory);
            e.cls = mc_tiled;
            e.placement_known = true;
            found = cache.emplace(x.desktop, e).first;
            memcut_refine({x.desktop});
        }
        memcut_add(t, found->second, x.refs <= 0);
    }
}

u64 memcut_union(std::vector<std::pair<u64, u64>>& spans)
{
    std::sort(spans.begin(), spans.end());
    u64 total = 0, start = 0, end = 0;
    for (const auto& [a, b] : spans) {
        if (a > end) {
            total += end - start;
            start = a;
            end = b;
        } else if (b > end) {
            end = b;
        }
    }
    return total + (end - start);
}

int memcut_text(char* text, int capacity)
{
    if (!the_device || !text || capacity < 64) return 0;
    const ULONGLONG began = GetTickCount64();
    MemcutTotals t;
    memcut_collect(t);
    const ULONGLONG took = GetTickCount64() - began;
    constexpr u64 mib = 1ull << 20;
    u64 local = 0, system = 0, dead_bytes = 0, dead_count = 0;
    for (int c = 0; c < mc_count; ++c) {
        local += t.bytes[c][0][0] + t.bytes[c][0][1];
        system += t.bytes[c][1][0] + t.bytes[c][1][1];
        dead_bytes += t.bytes[c][0][1] + t.bytes[c][1][1];
        dead_count += t.count[c][0][1] + t.count[c][1][1];
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process));
    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    GetPerformanceInfo(&perf, sizeof(perf));
    const auto& k = x2w_memcut::counters();
    auto live = [&](int c) { return (t.bytes[c][0][0] + t.bytes[c][1][0]) / mib; };
    auto live_n = [&](int c) { return t.count[c][0][0] + t.count[c][1][0]; };
    int n = std::snprintf(text, static_cast<std::size_t>(capacity),
        "memcut MiB/count: local %llu system %llu; tex %llu/%llu target %llu/%llu uav %llu/%llu tiled-srv %llu/%llu "
        "buf-copied %llu/%llu buf-aliased %llu/%llu buf %llu/%llu heap %llu/%llu; released-held %llu/%llu; ring %llu "
        "peak %llu refused %lld; copy peak %llu; used-after-release bound %lld viewed %lld; ws %llu avail-phys %llu",
        local / mib, system / mib, live(mc_tex), live_n(mc_tex), live(mc_tex_target), live_n(mc_tex_target),
        live(mc_tex_uav), live_n(mc_tex_uav), live(mc_tiled), live_n(mc_tiled), live(mc_buf_copied), live_n(mc_buf_copied),
        live(mc_buf_aliased), live_n(mc_buf_aliased), live(mc_buf), live_n(mc_buf), live(mc_heap), live_n(mc_heap),
        dead_bytes / mib, dead_count,
        static_cast<u64>(k.ring_bytes) / mib, static_cast<u64>(k.ring_frame_peak) / mib, k.ring_refused,
        static_cast<u64>(k.copy_frame_peak) / mib, k.bound_after_release, k.viewed_after_release,
        static_cast<u64>(process.WorkingSetSize) / mib, static_cast<u64>(perf.PhysicalAvailable) * perf.PageSize / mib);
    if (n < 0) return 0;
    if (n >= capacity) n = capacity - 1;
    static ULONGLONG last_detail;
    if (began - last_detail >= 10000) {
        last_detail = began;
        u64 span_sum = 0;
        for (const auto& [a, b] : t.spans) span_sum += b - a;
        const u64 span_union = memcut_union(t.spans);
        char line[1400];
        int at = std::snprintf(line, sizeof(line), "x2w-memcut detail (%llu wrappers, %llu ms, %llu unsized): live local textures by size "
                               "<1M %llu/%llu <16M %llu/%llu <64M %llu/%llu >=64M %llu/%llu MiB/count; their title spans sum %llu union %llu "
                               "in mapped %llu MiB;",
                               t.wrappers, took, t.unsized, t.tex_size_bytes[0] / mib, t.tex_size_count[0], t.tex_size_bytes[1] / mib,
                               t.tex_size_count[1], t.tex_size_bytes[2] / mib, t.tex_size_count[2], t.tex_size_bytes[3] / mib,
                               t.tex_size_count[3], span_sum / mib, span_union / mib, t.span_mapped / mib);
        std::vector<std::pair<u64, int>> formats;
        for (const auto& [format, bytes] : t.format_bytes) formats.emplace_back(bytes, format);
        std::sort(formats.rbegin(), formats.rend());
        if (at > 0 && at < static_cast<int>(sizeof(line))) at += std::snprintf(line + at, sizeof(line) - at, " top formats");
        for (std::size_t i = 0; i < formats.size() && i < 8 && at > 0 && at < static_cast<int>(sizeof(line)); ++i)
            at += std::snprintf(line + at, sizeof(line) - at, " %d:%llu", formats[i].second, formats[i].first / mib);
        for (int c = 0; c < mc_count && at > 0 && at < static_cast<int>(sizeof(line)); ++c)
            at += std::snprintf(line + at, sizeof(line) - at, "; %s local %llu/%llu system %llu/%llu released local %llu/%llu system %llu/%llu",
                                memcut_class_names[c], t.bytes[c][0][0] / mib, t.count[c][0][0], t.bytes[c][1][0] / mib, t.count[c][1][0],
                                t.bytes[c][0][1] / mib, t.count[c][0][1], t.bytes[c][1][1] / mib, t.count[c][1][1]);
        log("%s", line);
    }
    return n;
}
}  // namespace

// xmem_census.h: "name=<MiB> ..." text for the x2w-mem line.
extern "C" __declspec(dllexport) int WINAPI x2w_memory_counters(char* text, int capacity)
{
    try {
        return memcut_text(text, capacity);
    } catch (...) {
        return 0;  // out of memory while counting: no text this time
    }
}
