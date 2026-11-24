// LOCAL memcut (out/gamerun/memcut_work/NOTES.md). Included once after the stand-in's anonymous
// namespace in d3d12_xs_device.cpp. Exports x2w_memory_counters (xmem_census.h): the x2w-mem
// reporter (xmem.dll's thread) calls it every 2-10 s and appends the short text to its line; at most
// every 10 s a detail line "x2w-memcut ..." is logged as well.
//
// What it counts: every desktop resource the stand-in holds for a resource wrapper (the registry),
// by class, by heap kind (local = default heap; system = upload/readback/custom-L0 heaps and placed
// resources on heaps over title memory), live or released by the title (refs <= 0: release() keeps
// the desktop object for the process), plus the tiled-address SRV textures materialized on first
// sample, the upload ring use per frame and the frame-copy snapshots. Sizes are the desktop
// GetResourceAllocationInfo size of the resource's own description (cached per wrapper).
// It takes no draw lock; it takes the registry, note and tiled-SRV locks briefly (shared/short).
namespace {
enum MemcutClass : int { mc_tex_target, mc_tex_uav, mc_tex, mc_buf_copied, mc_buf_aliased, mc_buf, mc_tiled, mc_count };
const char* const memcut_class_names[mc_count] = {"tex-target", "tex-uav", "tex", "buf-copied", "buf-aliased", "buf", "tiled-srv"};
struct MemcutEntry {
    u64 bytes;
    int cls;
    bool system;  // not a default (video-memory) heap
    DXGI_FORMAT format;
};
struct MemcutTotals {
    u64 bytes[mc_count][2][2]{};  // [class][system][dead]
    u64 count[mc_count][2][2]{};
    u64 tex_size_bytes[4]{}, tex_size_count[4]{};  // live local textures: <1, <16, <64, >=64 MiB
    std::unordered_map<int, u64> format_bytes;      // live local textures by desktop format
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

MemcutEntry memcut_classify(Wrapper* w, ID3D12Resource* resource)
{
    MemcutEntry e{};
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_of(the_device)->GetResourceAllocationInfo(0, 1, &desc);
    e.bytes = info.SizeInBytes == ~0ull ? 0 : info.SizeInBytes;
    e.system = memcut_system_heap(resource);
    e.format = desc.Format;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        Placement placement = Placement::none;
        AcquireSRWLockShared(&note_lock);
        if (const auto found = resource_infos().find(w); found != resource_infos().end()) placement = found->second.placement;
        ReleaseSRWLockShared(&note_lock);
        // A retired placement reads as none: a released aliased buffer first seen after its release
        // is counted as "buf" (at most the 178 MiB of aliased buffers seen in run 82).
        e.cls = placement == Placement::aliased_buffer ? mc_buf_aliased
              : placement == Placement::copied_buffer ? mc_buf_copied : mc_buf;
    } else if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        e.cls = mc_tex_target;
    } else if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        e.cls = mc_tex_uav;
    } else {
        e.cls = mc_tex;
    }
    return e;
}

void memcut_add(MemcutTotals& t, const MemcutEntry& e, bool dead)
{
    if (!e.bytes) ++t.unsized;
    t.bytes[e.cls][e.system][dead] += e.bytes;
    ++t.count[e.cls][e.system][dead];
    if (!dead && !e.system && (e.cls == mc_tex || e.cls == mc_tex_target || e.cls == mc_tex_uav || e.cls == mc_tiled)) {
        const u64 m = e.bytes >> 20;
        const int s = m < 1 ? 0 : m < 16 ? 1 : m < 64 ? 2 : 3;
        t.tex_size_bytes[s] += e.bytes;
        ++t.tex_size_count[s];
        t.format_bytes[static_cast<int>(e.format)] += e.bytes;
    }
}

void memcut_collect(MemcutTotals& t)
{
    std::vector<const void*> all;
    AcquireSRWLockShared(&registry_lock);
    all.assign(registry().begin(), registry().end());
    ReleaseSRWLockShared(&registry_lock);
    t.wrappers = all.size();
    std::lock_guard guard(memcut_mutex);
    auto& cache = memcut_cache();
    for (const void* p : all) {
        auto* w = const_cast<Wrapper*>(static_cast<const Wrapper*>(p));
        if (w->own().kind != resource_kind || !w->own().real) continue;
        auto found = cache.find(p);
        if (found == cache.end())
            found = cache.emplace(p, memcut_classify(w, static_cast<ID3D12Resource*>(w->own().real))).first;
        memcut_add(t, found->second, InterlockedCompareExchange(&w->own().refs, 0, 0) <= 0);
    }
    std::vector<std::pair<Wrapper*, ID3D12Resource*>> tiled;
    {
        std::lock_guard lock(tiled_srv_mutex);
        for (auto& [wrapper, texture] : tiled_srv_textures)
            if (texture.desktop) tiled.emplace_back(wrapper, texture.desktop.Get());
    }
    for (auto& [wrapper, desktop] : tiled) {
        auto found = cache.find(desktop);
        if (found == cache.end()) {
            MemcutEntry e = memcut_classify(wrapper, desktop);
            e.cls = mc_tiled;
            found = cache.emplace(desktop, e).first;
        }
        memcut_add(t, found->second, InterlockedCompareExchange(&wrapper->own().refs, 0, 0) <= 0);
    }
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
        "buf-copied %llu/%llu buf-aliased %llu/%llu buf %llu/%llu; released-but-held %llu/%llu; ring %llu MiB total, "
        "frame peak %llu, refused %lld; frame-copy peak %llu; after-release bound %lld viewed %lld; ws %llu avail-phys %llu",
        local / mib, system / mib, live(mc_tex), live_n(mc_tex), live(mc_tex_target), live_n(mc_tex_target),
        live(mc_tex_uav), live_n(mc_tex_uav), live(mc_tiled), live_n(mc_tiled), live(mc_buf_copied), live_n(mc_buf_copied),
        live(mc_buf_aliased), live_n(mc_buf_aliased), live(mc_buf), live_n(mc_buf), dead_bytes / mib, dead_count,
        static_cast<u64>(k.ring_bytes) / mib, static_cast<u64>(k.ring_frame_peak) / mib, k.ring_refused,
        static_cast<u64>(k.copy_frame_peak) / mib, k.bound_after_release, k.viewed_after_release,
        static_cast<u64>(process.WorkingSetSize) / mib, static_cast<u64>(perf.PhysicalAvailable) * perf.PageSize / mib);
    if (n < 0) return 0;
    if (n >= capacity) n = capacity - 1;
    static ULONGLONG last_detail;
    if (began - last_detail >= 10000) {
        last_detail = began;
        char line[1400];
        int at = std::snprintf(line, sizeof(line), "x2w-memcut detail (%llu wrappers, %llu ms, %llu unsized): live local textures by size "
                               "<1M %llu/%llu <16M %llu/%llu <64M %llu/%llu >=64M %llu/%llu MiB/count;",
                               t.wrappers, took, t.unsized, t.tex_size_bytes[0] / mib, t.tex_size_count[0], t.tex_size_bytes[1] / mib,
                               t.tex_size_count[1], t.tex_size_bytes[2] / mib, t.tex_size_count[2], t.tex_size_bytes[3] / mib,
                               t.tex_size_count[3]);
        std::vector<std::pair<u64, int>> formats;
        for (const auto& [format, bytes] : t.format_bytes) formats.emplace_back(bytes, format);
        std::sort(formats.rbegin(), formats.rend());
        at += std::snprintf(line + at, sizeof(line) - at, " top formats");
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
