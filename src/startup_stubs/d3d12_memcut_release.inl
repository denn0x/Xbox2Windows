// LOCAL memcut (out/gamerun/memcut_work/NOTES.md): X2W_MEMCUT_RELEASE=1 frees the desktop copy of a plain
// texture the title has released (refs 0), and makes it again from title memory if the title binds it later.
// Default off. Included once at the end of d3d12_xs_device.cpp (inside the anonymous namespace there).
//
// Why: release() keeps every desktop object for the process. Runs 94-97 hold 1.7-3.3 GiB of released
// resources, ~80 % of it plain sampled textures (x2w-mem "released-held"). Those textures are copies of title
// memory (upload_texture fills them from the title's pages), so they can be rebuilt; render targets, depth
// and UAV textures are GPU-written and are never freed here.
//
// The title DOES bind views of released textures (memcut counter "used-after-release bound" 228-1,115 per
// run): a console descriptor is only an address. So a released texture is first HOLLOWED, then freed:
//  1. Candidate: refs reached 0 (release hook), a plain 2D texture (no RT/DS/UAV flag, one sample), on a
//     default heap, with title memory, not a tiled-address or linear memory SRV texture.
//  2. Hollow (at a submit, draw_lock held, once per frame): when the title has neither re-acquired nor bound
//     it for X2W_MEMCUT_RELEASE_FRAMES frames (default 8), the wrapper's desktop object is replaced by a
//     reserved resource of the same description (no memory; reads of unmapped tiles return 0 on tiled
//     resources tier 2+, so a missed path reads zeros instead of freed memory). own().real is never null,
//     so the 68 sites that read it need no change. The persistent view-id descriptors of translated compute
//     (fill_views) that name the wrapper are rewritten to the placeholder; texture_uploads forgets the old
//     object. The epoch counter is raised first.
//  3. Free the old object only when (a) every live command list has been Reset since the hollow epoch (a
//     closed list can be executed again until its Reset, and D3D12 lists hold no references) and then
//     (b) every queue's submit fence (queue_submits, note_submits) has passed the value it had at that moment.
//  4. Revive: the bind paths (sampled_address_texture for draws, texture_view with prepare for compute) call
//     memcut_revive first: a hollow wrapper gets a new committed texture (same description, heap, flags;
//     state COMMON as every desktop resource between lists), which the bind uploads from title memory as a
//     first bind; its view-id descriptors are rewritten; the placeholder retires by the same rule 3.
// Not covered (stay correct by construction, may show zeros): title-created desktop descriptors used directly
// by the GPU (title view heaps bound as they are), the present path's own lists (samples planes, which the
// title does not release while presenting). Fixtures only: X2W_MEMCUT_RELEASE_TEST_IMMEDIATE=1 frees at hollow
// time without rule 3 (the negative control of memcut_release_check).
#include <unordered_set>

namespace {
struct MemcutHollow {
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES props;
    D3D12_HEAP_FLAGS flags;
    DXGI_FORMAT packed;  // packed_stand_in_tag of the original, or UNKNOWN
    u64 bytes;
};
struct MemcutRetiring {
    ID3D12Resource* object;
    u64 epoch;
    u64 bytes;
    bool snapped;
    std::vector<std::pair<ID3D12Fence*, UINT64>> waits;
};
struct MemcutRelease {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<Wrapper*, LONG> pending;         // wrapper -> frame of its release or last bind
    std::unordered_map<Wrapper*, MemcutHollow> hollow;  // wrappers whose real is a placeholder now
    std::unordered_map<Wrapper*, u64> list_epochs;      // list -> epoch at its last Reset
    std::unordered_set<Wrapper*> refused;               // no placeholder could be made, or revived once: never again
    std::vector<MemcutRetiring> retiring;
    std::vector<std::pair<ID3D12Fence*, UINT64>> test_waits;  // fixtures: extra fences, as note_submits would add
    volatile LONG64 epoch = 0;
    volatile LONG active = 0;  // anything pending or hollow: the bind hook's fast path
    LONG last_hollow_frame = -1, last_report_frame = 0, lowest_frame = -1;
    u64 lowest = 0;  // lowest list Reset epoch, computed at lowest_frame
    u64 hollowed = 0, freed = 0, freed_bytes = 0, revived = 0, refused_count = 0, blocked = 0;
    u64 reported_freed = 0, window_us = 0, window_max_us = 0;  // report bookkeeping, reaper cost since the last line
};
MemcutRelease& memcut_release_state()
{
    static MemcutRelease s;
    return s;
}
bool memcut_release_on()
{
    static const bool on = x2w_local::env_on("X2W_MEMCUT_RELEASE");
    return on;
}
LONG memcut_release_frames()
{
    static const LONG frames = [] {
        char v[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_MEMCUT_RELEASE_FRAMES", v, sizeof(v));
        const long f = n && n < sizeof(v) ? std::atol(v) : 8;
        return static_cast<LONG>(f >= 1 ? f : 1);
    }();
    return frames;
}
bool memcut_release_immediate()
{
    static const bool on = x2w_local::env_on("X2W_MEMCUT_RELEASE_TEST_IMMEDIATE");
    return on;
}
// Reads of unmapped tiles of a reserved resource return 0 from tier 2 on; below that the placeholder
// would not be safe, so the cut stays off.
bool memcut_tiles_safe()
{
    static const bool ok = [] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        const bool got = the_device && SUCCEEDED(device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)));
        log("x2w-memcut release: X2W_MEMCUT_RELEASE=1, tiled resources tier %d%s, grace %ld frames%s", got ? static_cast<int>(o.TiledResourcesTier) : -1,
            got && o.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2 ? "" : " (below 2: OFF)", memcut_release_frames(),
            memcut_release_immediate() ? ", TEST IMMEDIATE (no fence wait)" : "");
        return got && o.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    }();
    return ok;
}
DXGI_FORMAT memcut_packed_of(ID3D12Resource* r) { return x2w_startup::packed_stand_in_of(r); }
void memcut_copy_packed(ID3D12Resource* to, DXGI_FORMAT packed)
{
    if (to && packed != DXGI_FORMAT_UNKNOWN) to->SetPrivateData(x2w_startup::packed_stand_in_tag, sizeof(packed), &packed);
}

// Writes the persistent view-id descriptors (translated compute) of every record that names one of these
// wrappers again, from its current desktop object: one pass over the records. draw_lock held.
void memcut_refill_views(const std::vector<Wrapper*>& wrappers)
{
    if (!draw_heap || wrappers.empty()) return;
    std::unordered_set<u64> names;
    for (Wrapper* w : wrappers) names.insert(reinterpret_cast<u64>(w));
    std::vector<std::pair<UINT, ViewRecord>> found;
    AcquireSRWLockShared(&note_lock);
    const UINT filled = views_filled < view_records().size() ? views_filled : static_cast<UINT>(view_records().size());
    for (UINT i = 0; i < filled; ++i)
        if (view_records()[i].resource && names.count(view_records()[i].resource)) found.emplace_back(i, view_records()[i]);
    ReleaseSRWLockShared(&note_lock);
    for (const auto& [i, record] : found) fill_view_kinds(nullptr, draw_heap_size + 8 + 8 * i, &record);
}

void memcut_swap_real(Wrapper* w, ID3D12Resource* to)
{
    AcquireSRWLockExclusive(&registry_lock);
    w->own().real = to;
    ReleaseSRWLockExclusive(&registry_lock);
}

// Release hook: refs just reached 0.
void memcut_released(Wrapper* self)
{
    if (!memcut_release_on() || self->own().kind != resource_kind || !self->own().title_memory || self->own().tiled_address_srv ||
        self->own().linear_memory_srv)
        return;
    auto* real = static_cast<ID3D12Resource*>(self->own().real);
    if (!real) return;
    const D3D12_RESOURCE_DESC desc = real->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)))
        return;
    D3D12_HEAP_PROPERTIES props{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(real->GetHeapProperties(&props, &flags)) || props.Type != D3D12_HEAP_TYPE_DEFAULT) return;
    MemcutRelease& s = memcut_release_state();
    AcquireSRWLockExclusive(&s.lock);
    if (!s.refused.count(self) && !s.hollow.count(self)) {
        s.pending[self] = frame_number;
        InterlockedExchange(&s.active, 1);
    }
    ReleaseSRWLockExclusive(&s.lock);
}

// List Reset hook (after a successful Reset).
void memcut_list_reset(Wrapper* list)
{
    if (!memcut_release_on()) return;
    MemcutRelease& s = memcut_release_state();
    AcquireSRWLockExclusive(&s.lock);
    s.list_epochs[list] = static_cast<u64>(InterlockedCompareExchange64(&s.epoch, 0, 0));
    ReleaseSRWLockExclusive(&s.lock);
}

// Bind hook (draw_lock held): a hollow texture is made again; a pending one counts as used now.
void memcut_revive(Wrapper* w)
{
    if (!w || !memcut_release_on()) return;
    MemcutRelease& s = memcut_release_state();
    if (!InterlockedCompareExchange(&s.active, 0, 0)) return;
    AcquireSRWLockExclusive(&s.lock);
    if (auto p = s.pending.find(w); p != s.pending.end()) {
        p->second = frame_number;
        ReleaseSRWLockExclusive(&s.lock);
        return;
    }
    const auto h = s.hollow.find(w);
    if (h == s.hollow.end()) {
        ReleaseSRWLockExclusive(&s.lock);
        return;
    }
    const MemcutHollow hollow = h->second;
    ID3D12Resource* created = nullptr;
    const HRESULT hr = device_of(the_device)->CreateCommittedResource(&hollow.props, hollow.flags, &hollow.desc, D3D12_RESOURCE_STATE_COMMON,
                                                                      nullptr, IID_PPV_ARGS(&created));
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&s.lock);
        static LONG failures;
        if (first_calls(failures, 6)) log("x2w-memcut release: revive of %p failed 0x%08lx (it stays a placeholder: reads zero)", static_cast<void*>(w), hr);
        return;
    }
    memcut_copy_packed(created, hollow.packed);
    auto* placeholder = static_cast<ID3D12Resource*>(w->own().real);
    memcut_swap_real(w, created);
    texture_uploads().erase(placeholder);
    s.hollow.erase(h);
    const u64 e = static_cast<u64>(InterlockedIncrement64(&s.epoch));
    s.lowest_frame = -1;  // as in memcut_reap: the next list check comes after this swap
    s.retiring.push_back({placeholder, e, 0, false, {}});
    // A texture bound after it was freed is one the title still draws: never hollow it again (no hollow/revive
    // cycle, each of which would be a re-upload under draw_lock).
    s.refused.insert(w);
    ++s.revived;
    ReleaseSRWLockExclusive(&s.lock);
    memcut_refill_views({w});
    static LONG shown;
    if (first_calls(shown, 8))
        log("x2w-memcut release: revived %p (%llux%u fmt %d, %llu KiB): bound after it was freed; uploaded again from title memory",
            static_cast<void*>(w), hollow.desc.Width, hollow.desc.Height, hollow.desc.Format, hollow.bytes >> 10);
}

// s.lock held. The lowest Reset epoch over the live command lists (0 for a list never Reset).
u64 memcut_min_list_epoch(MemcutRelease& s, u64* live)
{
    u64 lowest = ~0ull;
    *live = 0;
    AcquireSRWLockShared(&registry_lock);
    for (const void* p : registry()) {
        auto* w = const_cast<Wrapper*>(static_cast<const Wrapper*>(p));
        if (w->own().kind != list_kind || InterlockedCompareExchange(&w->own().refs, 0, 0) <= 0) continue;
        ++*live;
        const auto found = s.list_epochs.find(w);
        const u64 e = found == s.list_epochs.end() ? 0 : found->second;
        if (e < lowest) lowest = e;
    }
    ReleaseSRWLockShared(&registry_lock);
    return lowest;
}

// At a submit (draw_lock held), after note_submits.
void memcut_reap()
{
    if (!memcut_release_on()) return;
    MemcutRelease& s = memcut_release_state();
    if (!InterlockedCompareExchange(&s.active, 0, 0) && s.retiring.empty()) return;
    if (!memcut_tiles_safe()) return;
    LARGE_INTEGER started{};
    QueryPerformanceCounter(&started);
    AcquireSRWLockExclusive(&s.lock);
    const LONG frame = frame_number;
    // 2. Hollow, once per frame.
    if (frame != s.last_hollow_frame && !s.pending.empty()) {
        s.last_hollow_frame = frame;
        std::vector<Wrapper*> due;
        for (auto it = s.pending.begin(); it != s.pending.end();) {
            if (InterlockedCompareExchange(&it->first->own().refs, 0, 0) > 0) { it = s.pending.erase(it); continue; }
            if (frame - it->second >= memcut_release_frames() && due.size() < 256) due.push_back(it->first);
            ++it;
        }
        const u64 e = due.empty() ? 0 : static_cast<u64>(InterlockedIncrement64(&s.epoch));
        if (!due.empty()) s.lowest_frame = -1;  // the list check for epoch e must look at the lists after this hollow
        for (Wrapper* w : due) {
            s.pending.erase(w);
            auto* old = static_cast<ID3D12Resource*>(w->own().real);
            // As device slot 133 / 127 create them: a default heap, no heap flags (the implicit heap's own flags,
            // e.g. DENY_BUFFERS, are refused by CreateCommittedResource).
            MemcutHollow h{old->GetDesc(), D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT}, D3D12_HEAP_FLAG_NONE, memcut_packed_of(old), 0};
            const D3D12_RESOURCE_ALLOCATION_INFO info = device_of(the_device)->GetResourceAllocationInfo(0, 1, &h.desc);
            h.bytes = info.SizeInBytes == ~0ull ? 0 : info.SizeInBytes;
            D3D12_RESOURCE_DESC reserved = h.desc;
            reserved.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
            reserved.Alignment = 0;
            ID3D12Resource* placeholder = nullptr;
            if (FAILED(device_of(the_device)->CreateReservedResource(&reserved, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placeholder)))) {
                s.refused.insert(w);
                ++s.refused_count;
                static LONG failures;
                if (first_calls(failures, 6))
                    log("x2w-memcut release: no placeholder for %p (%llux%u mips %u fmt %d): kept", static_cast<void*>(w), h.desc.Width, h.desc.Height,
                        h.desc.MipLevels, h.desc.Format);
                continue;
            }
            memcut_copy_packed(placeholder, h.packed);
            memcut_swap_real(w, placeholder);
            texture_uploads().erase(old);
            s.hollow[w] = h;
            ++s.hollowed;
            if (memcut_release_immediate()) {  // fixtures only: the negative control
                old->Release();
                ++s.freed;
                s.freed_bytes += h.bytes;
            } else {
                s.retiring.push_back({old, e, h.bytes, false, {}});
            }
        }
        ReleaseSRWLockExclusive(&s.lock);
        memcut_refill_views(due);  // a refused one rewrites its own views: harmless
        AcquireSRWLockExclusive(&s.lock);
    }
    // 3. Free what no list and no queue can still reach.
    if (!s.retiring.empty()) {
        // The registry walk (~100k wrappers in a world run) runs at most once per frame, and only while an
        // entry still waits for list Resets: this runs under draw_lock at every submit (run 99: draw_lock is
        // the frame time).
        bool unsnapped = false;
        for (const MemcutRetiring& r : s.retiring) unsnapped |= !r.snapped;
        if (unsnapped && frame != s.lowest_frame) {
            u64 live = 0;
            s.lowest = memcut_min_list_epoch(s, &live);
            s.lowest_frame = frame;
        }
        const u64 lowest = s.lowest;
        std::vector<std::pair<ID3D12Fence*, UINT64>> now;
        bool now_taken = false;
        u64 blocked = 0;
        for (auto it = s.retiring.begin(); it != s.retiring.end();) {
            MemcutRetiring& r = *it;
            if (!r.snapped) {
                if (lowest < r.epoch) { ++blocked; ++it; continue; }
                if (!now_taken) {
                    AcquireSRWLockShared(&allocator_lock);
                    for (const auto& [queue, submit] : queue_submits)
                        if (submit.first) now.push_back(submit);
                    ReleaseSRWLockShared(&allocator_lock);
                    now.insert(now.end(), s.test_waits.begin(), s.test_waits.end());
                    now_taken = true;
                }
                r.waits = now;
                r.snapped = true;
            }
            bool passed = true;
            for (const auto& [fence, value] : r.waits)
                if (fence->GetCompletedValue() < value) passed = false;
            if (!passed) { ++it; continue; }
            r.object->Release();
            ++s.freed;
            s.freed_bytes += r.bytes;
            it = s.retiring.erase(it);
        }
        s.blocked = blocked;
    }
    if (s.pending.empty() && s.hollow.empty()) InterlockedExchange(&s.active, 0);
    else InterlockedExchange(&s.active, 1);
    // Cost under draw_lock (this call, including the lock wait): max and total since the last report.
    LARGE_INTEGER ended{}, hz{};
    QueryPerformanceCounter(&ended);
    QueryPerformanceFrequency(&hz);
    const u64 us = static_cast<u64>((ended.QuadPart - started.QuadPart) * 1000000 / (hz.QuadPart ? hz.QuadPart : 1));
    s.window_us += us;
    if (us > s.window_max_us) s.window_max_us = us;
    // Every 300 frames, and at each of the first 4 frees (once per new free, not at every submit).
    const bool early = s.freed && s.freed <= 4 && s.freed != s.reported_freed;
    const bool report = frame - s.last_report_frame >= 300 || early;
    const u64 window_us = s.window_us, window_max_us = s.window_max_us;
    if (report) s.last_report_frame = frame, s.reported_freed = s.freed, s.window_us = 0, s.window_max_us = 0;
    const u64 pending = s.pending.size(), hollow = s.hollow.size(), retiring = s.retiring.size();
    const u64 hollowed = s.hollowed, freed = s.freed, freed_mib = s.freed_bytes >> 20, revived = s.revived, refused = s.refused_count, blocked = s.blocked;
    ReleaseSRWLockExclusive(&s.lock);
    static LONG reports;
    if (report && first_calls(reports, 400))
        log("x2w-memcut release (frame %ld): pending %llu, hollow %llu, retiring %llu (%llu waiting for a list Reset), hollowed %llu, freed %llu = %llu MiB, "
            "revived %llu, refused %llu; reaper under draw_lock since the last line: total %llu.%03llu ms, max %llu.%03llu ms",
            frame, pending, hollow, retiring, blocked, hollowed, freed, freed_mib, revived, refused, window_us / 1000, window_us % 1000,
            window_max_us / 1000, window_max_us % 1000);
}
}  // namespace

// Fixtures (memcut_release_check): op 0 the wrapper's desktop object; 1 add fence (a, value b) to the waits a
// snapshot takes; 2 advance frame_number by b and run the reaper as a submit would; 3 bind wrapper a (revive);
// 4 state of wrapper a: 0 none, 1 pending, 2 hollow, 3 refused; 5 freed count; 6 Reset epoch hook for list a.
extern "C" __declspec(dllexport) u64 WINAPI x2w_memcut_release_test(int op, void* a, u64 b)
{
    MemcutRelease& s = memcut_release_state();
    Wrapper* w = as_wrapper(a);
    switch (op) {
    case 0: return w ? reinterpret_cast<u64>(w->own().real) : 0;
    case 1:
        AcquireSRWLockExclusive(&s.lock);
        s.test_waits.emplace_back(static_cast<ID3D12Fence*>(a), b);
        ReleaseSRWLockExclusive(&s.lock);
        return 1;
    case 2:
        lock_draw(900000 + __LINE__);
        frame_number += static_cast<LONG>(b);
        memcut_reap();
        ReleaseSRWLockExclusive(&draw_lock);
        return 1;
    case 3:
        lock_draw(900000 + __LINE__);
        memcut_revive(w);
        ReleaseSRWLockExclusive(&draw_lock);
        return w ? reinterpret_cast<u64>(w->own().real) : 0;
    case 4: {
        AcquireSRWLockShared(&s.lock);
        const u64 state = s.pending.count(w) ? 1 : s.hollow.count(w) ? 2 : s.refused.count(w) ? 3 : 0;
        ReleaseSRWLockShared(&s.lock);
        return state;
    }
    case 5: return s.freed;
    case 6: memcut_list_reset(w); return 1;
    default: return 0;
    }
}
