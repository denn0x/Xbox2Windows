// LOCAL experiment (never upstream): container compute shaders that dereference raw 64-bit title GPU
// addresses (design: out/gamerun/dxil_work/RAWADDR_DESIGN.md). Included inside d3d12_xs_device.cpp's
// anonymous namespace, after d3d12_compute.inl (uses desktop_range, draw_heap, pass_of).
//
// Inert unless the shader cache holds <key>.rawaddr.txt (sidecar) and <key>.rawaddr.dxbc for a slot-62
// pipeline key. Such a shader reads title memory through an 8-entry region table (root constants b0
// space100) and the raw UAVs u0..u9 space100 (rawaddr_table.h). The pipeline gets the title root plus
// those two parameters; each dispatch fills the table from the sidecar's seeds (root-constant addresses,
// root descriptors) and from addresses the GPU missed in earlier dispatches of the same key.
// On only with X2W_RAWADDR=1; off: X2W_RAWADDR_OFF=1 (wins).

namespace ra = x2w_startup::rawaddr;

struct RawKey {
    u64 key = 0;
    std::vector<ra::Seed> seeds;
    std::vector<u64> learned;  // addresses missed by earlier dispatches (page start), at most 4
    UINT slot = 0;             // statistics slot in rawaddr_stats
    LONG dispatches = 0, resolved = 0, unresolved = 0, overflow = 0, skipped = 0, shown = 0;
    LONG reported_dispatches = 0;
    std::uint32_t seen_misses = 0;
};
struct RawPipeline {
    RawKey* key;
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    UINT first_extra;
};
SRWLOCK rawaddr_lock = SRWLOCK_INIT;
std::unordered_map<u64, RawKey*>& rawaddr_keys() { static std::unordered_map<u64, RawKey*> m; return m; }
std::unordered_map<Wrapper*, RawPipeline>& rawaddr_pipelines() { static std::unordered_map<Wrapper*, RawPipeline> m; return m; }
constexpr UINT rawaddr_stats_slots = 256;
ID3D12Resource* rawaddr_stats;
volatile std::uint32_t* rawaddr_stats_words;  // CPU view of the custom-heap statistics buffer
LONG rawaddr_stats_failed;

// Opt-in: the raw-address shaders have not run on title data yet, so a published sidecar stays inert
// unless X2W_RAWADDR=1 (X2W_RAWADDR_OFF=1 still wins).
bool rawaddr_enabled()
{
    static const bool enabled = x2w_local::env_on("X2W_RAWADDR") && !x2w_local::env_on("X2W_RAWADDR_OFF");
    return enabled;
}

// One CPU-readable buffer (custom heap, write-back, L0) with a 128-byte statistics slot per key.
void rawaddr_ensure_stats()
{
    if (rawaddr_stats || rawaddr_stats_failed) return;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = u64{rawaddr_stats_slots} * ra::kStatsBytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device_of(the_device)->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rawaddr_stats));
    void* words = nullptr;
    if (SUCCEEDED(hr)) hr = rawaddr_stats->Map(0, nullptr, &words);
    if (FAILED(hr)) {
        rawaddr_stats_failed = 1;
        if (rawaddr_stats) rawaddr_stats->Release();
        rawaddr_stats = nullptr;
        log("x2w-rawaddr statistics buffer failed 0x%08lx: GPU misses are not counted", static_cast<unsigned long>(hr));
        return;
    }
    std::memset(words, 0, static_cast<std::size_t>(rd.Width));
    rawaddr_stats_words = static_cast<volatile std::uint32_t*>(words);
}

// Device slot 62 hook: builds the raw-address pipeline for key when its sidecar exists. True when the
// key has a sidecar (whether or not the pipeline could be built): the plain .cs.dxbc path is then skipped.
bool rawaddr_pipeline(u64 key, Wrapper* root, Wrapper* pipeline)
{
    if (!key || !pipeline) return false;
    wchar_t path[MAX_PATH];
    std::string sidecar;
    if (!cache_path(path, L"", key, L".rawaddr.txt") || !read_text(path, sidecar)) return false;
    if (!rawaddr_enabled()) {
        static LONG logged;
        if (first_calls(logged, 4)) log("x2w-rawaddr %016llx: sidecar present, path off (needs X2W_RAWADDR=1, no X2W_RAWADDR_OFF): no pipeline", key);
        return true;
    }
    std::vector<ra::Seed> seeds;
    if (!ra::parse_sidecar(sidecar, seeds)) {
        log("x2w-rawaddr %016llx: sidecar not understood (version / table shape): no pipeline", key);
        return true;
    }
    std::vector<unsigned char> blob;
    AcquireSRWLockShared(&note_lock);
    if (const auto found = root_blobs().find(root); found != root_blobs().end()) blob = found->second;
    ReleaseSRWLockShared(&note_lock);
    ID3DBlob* code = nullptr;
    const char* error = nullptr;
    UINT first = 0;
    ID3DBlob* augmented = blob.empty() ? nullptr : ra::augmented_root(blob.data(), blob.size(), &first, &error);
    ID3D12RootSignature* signature = nullptr;
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = augmented ? S_OK : E_FAIL;
    if (blob.empty()) error = "no title root blob";
    if (SUCCEEDED(hr)) hr = device_of(the_device)->CreateRootSignature(0, augmented->GetBufferPointer(), augmented->GetBufferSize(), IID_PPV_ARGS(&signature));
    if (augmented) augmented->Release();
    if (SUCCEEDED(hr) && (!cache_path(path, L"", key, L".rawaddr.dxbc") || FAILED(D3DReadFileToBlob(path, &code)))) {
        hr = E_FAIL;
        error = "no .rawaddr.dxbc";
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = signature;
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        hr = device_of(the_device)->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) error = "CreateComputePipelineState";
    }
    if (code) code->Release();
    log("x2w-rawaddr %016llx pipeline: title root params %u + 2, %zu seeds -> 0x%08lx%s%s", key, first, seeds.size(),
        static_cast<unsigned long>(hr), error ? " " : "", error ? error : "");
    if (FAILED(hr)) {
        if (signature) signature->Release();
        return true;
    }
    AcquireSRWLockExclusive(&rawaddr_lock);
    RawKey*& k = rawaddr_keys()[key];
    if (!k) {
        k = new RawKey;
        k->key = key;
        k->seeds = seeds;
        k->slot = static_cast<UINT>(rawaddr_keys().size() - 1) % rawaddr_stats_slots;
    }
    rawaddr_pipelines()[pipeline] = {k, signature, pso, first};
    ReleaseSRWLockExclusive(&rawaddr_lock);
    return true;
}

// The largest span [address, address + span) contiguous in one desktop window, up to X2W_RAWADDR_SPAN_MIB
// (default 16 MiB), to 64 KiB granularity: the cap, then quarters until one fits, then a binary search
// between that span and the failing one above it. At least the 4 KiB page, else unresolved.
bool rawaddr_region(u64 address, ra::Region* region, ID3D12Resource** window)
{
    static const u64 cap = [] {
        char text[16] = "";
        const DWORD n = GetEnvironmentVariableA("X2W_RAWADDR_SPAN_MIB", text, sizeof(text));
        const unsigned long mib = n && n < sizeof(text) ? std::strtoul(text, nullptr, 10) : 0;
        return u64{mib >= 1 && mib <= 192 ? mib : 16} << 20;
    }();
    u64 offset = 0, good = 0, bad = 0;
    ID3D12Resource* w = nullptr;
    for (u64 span = cap; span >= 0x10000; span >>= 2) {
        if (desktop_range(address, span, &w, &offset)) {
            good = span;
            break;
        }
        bad = span;
    }
    if (!good) {
        if (!desktop_range(address, 0x1000, &w, &offset)) return false;  // at least the page
        *window = w;
        *region = ra::make_region(address, 0x1000, offset);
        return true;
    }
    u64 good_offset = offset;
    ID3D12Resource* good_window = w;
    while (bad && bad - good > 0x10000) {
        const u64 mid = (good + (bad - good) / 2) & ~0xffffull;
        if (mid <= good) break;
        if (desktop_range(address, mid, &w, &offset)) {
            good = mid;
            good_offset = offset;
            good_window = w;
        } else {
            bad = mid;
        }
    }
    *window = good_window;
    *region = ra::make_region(address, good, good_offset);
    return true;
}

// execute_dispatch hook: runs the dispatch when the bound pipeline is a raw-address pipeline. False when
// it is not one (or a title argument cannot be bound: the dispatch is then skipped by the caller).
bool rawaddr_dispatch(Wrapper* self, ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
    Pass& pass = pass_of(self);
    AcquireSRWLockShared(&rawaddr_lock);
    const auto found = rawaddr_pipelines().find(pass.pipeline);
    const RawPipeline rp = found != rawaddr_pipelines().end() ? found->second : RawPipeline{};
    ReleaseSRWLockShared(&rawaddr_lock);
    if (!rp.pso || !pass.compute_root) return false;
    RawKey& key = *rp.key;
    // the title's own parameters, replayed as execute_dispatch does
    D3D12_GPU_VIRTUAL_ADDRESS addresses[16] = {};
    for (UINT i = 0; i < 16; ++i) {
        const int kind = pass.compute_kind[i];
        if (kind == 5 || (kind >= 2 && kind <= 4 && pass.compute_address[i] && !(addresses[i] = desktop_va(pass.compute_address[i])))) {
            if (x2w_local::first_of(InterlockedIncrement(&key.skipped), 4)) log("x2w-rawaddr %016llx: title parameter %u (kind %d) not bindable: dispatch skipped", key.key, i, kind);
            return false;
        }
    }
    // learned seeds: addresses this key missed before (statistics slot, written by the GPU)
    AcquireSRWLockExclusive(&rawaddr_lock);
    rawaddr_ensure_stats();  // lists record on several threads: created under the lock
    if (rawaddr_stats_words) {
        const volatile std::uint32_t* s = rawaddr_stats_words + key.slot * (ra::kStatsBytes / 4);
        const std::uint32_t misses = s[0];
        if (misses != key.seen_misses) {
            for (UINT i = 0; i < ra::kStatsRing && i < misses - key.seen_misses && key.learned.size() < 4; ++i) {
                const u64 a = (s[4 + 2 * i] | (u64{s[5 + 2 * i]} << 32)) & ~0xffffull;
                if (a && std::find(key.learned.begin(), key.learned.end(), a) == key.learned.end()) key.learned.push_back(a);
            }
            key.seen_misses = misses;
        }
    }
    std::vector<u64> learned = key.learned;
    ReleaseSRWLockExclusive(&rawaddr_lock);
    // seeds -> regions: root constants first, then learned, then root descriptors
    ra::Region regions[ra::kRegions]{};
    ID3D12Resource* windows[ra::kRegions] = {};
    UINT count = 0, resolved = 0, unresolved = 0, overflow = 0;
    std::vector<u64> forget;  // learned addresses that do not resolve: dropped so they free their place
    const bool show = x2w_local::first_calls(key.shown, 2);
    auto add = [&](u64 address, const char* what) {
        if (!address) return;
        for (UINT k = 0; k < count; ++k)
            if (ra::region_contains(regions[k], address)) return;
        if (count >= ra::kRegions) {
            ++overflow;
            if (show) log("x2w-rawaddr   %016llx seed %s 0x%llx -> DROPPED (table full)", key.key, what, address);
            return;
        }
        if (rawaddr_region(address, &regions[count], &windows[count])) {
            ++resolved;
            if (show) log("x2w-rawaddr   %016llx seed %s 0x%llx -> region %u, %u bytes", key.key, what, address, count, regions[count].size);
            ++count;
        } else {
            ++unresolved;
            if (what[0] == 'l') forget.push_back(address);
            if (show) log("x2w-rawaddr   %016llx seed %s 0x%llx -> UNRESOLVED", key.key, what, address);
        }
    };
    for (const ra::Seed& s : key.seeds)
        if (s.kind != ra::SeedKind::rootdesc) add(ra::seed_address(s, pass.compute_kind, pass.compute_constants, pass.compute_address), "rootconst");
    for (u64 a : learned) add(a, "learned");
    for (const ra::Seed& s : key.seeds)
        if (s.kind == ra::SeedKind::rootdesc) add(ra::seed_address(s, pass.compute_kind, pass.compute_constants, pass.compute_address), "rootdesc");
    InterlockedExchangeAdd(&key.resolved, static_cast<LONG>(resolved));
    InterlockedExchangeAdd(&key.unresolved, static_cast<LONG>(unresolved));
    InterlockedExchangeAdd(&key.overflow, static_cast<LONG>(overflow));
    if (!forget.empty()) {
        AcquireSRWLockExclusive(&rawaddr_lock);
        for (u64 a : forget) key.learned.erase(std::remove(key.learned.begin(), key.learned.end(), a), key.learned.end());
        ReleaseSRWLockExclusive(&rawaddr_lock);
    }
    std::uint32_t table[ra::kTableDwords];
    ra::fill_table(regions, count, table);

    lock_draw(400000 + __LINE__);
    if (!ensure_draw_heap(the_device)) {
        ReleaseSRWLockExclusive(&draw_lock);
        return false;
    }
    UINT first = static_cast<UINT>(InterlockedExchangeAdd(&draw_heap_cursor, static_cast<LONG>(ra::kSlots))) % draw_heap_size;
    if (first + ra::kSlots > draw_heap_size) {
        first = 0;
        InterlockedExchange(&draw_heap_cursor, static_cast<LONG>(ra::kSlots));
    }
    auto raw_uav = [&](ID3D12Resource* buffer, u64 offset, UINT bytes, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = offset / 4;
        d.Buffer.NumElements = buffer ? (bytes + 3) / 4 : 1;
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        const D3D12_CPU_DESCRIPTOR_HANDLE dest{draw_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{first + slot} * draw_heap_increment};
        device_of(the_device)->CreateUnorderedAccessView(buffer, nullptr, &d, dest);
    };
    for (UINT k = 0; k < ra::kRegions; ++k) raw_uav(k < count ? windows[k] : nullptr, regions[k].view_offset, regions[k].size, k);
    raw_uav(nullptr, 0, 0, ra::kNullSlot);
    raw_uav(rawaddr_stats, u64{key.slot} * ra::kStatsBytes, ra::kStatsBytes, ra::kStatsSlot);
    list->SetPipelineState(rp.pso);
    if (!pass.heap_set) {
        list->SetDescriptorHeaps(1, &draw_heap);
        pass.heap_set = true;
        pass.bound_sampler_heap = nullptr;  // this call unbinds any sampler heap a draw bound
    }
    list->SetComputeRootSignature(rp.root);
    for (UINT i = 0; i < 16 && i < rp.first_extra; ++i) {
        switch (pass.compute_kind[i]) {
        case 1: list->SetComputeRoot32BitConstants(i, pass.compute_count[i], pass.compute_constants[i], 0); break;
        case 2: list->SetComputeRootConstantBufferView(i, addresses[i]); break;
        case 3: list->SetComputeRootShaderResourceView(i, addresses[i]); break;
        case 4: list->SetComputeRootUnorderedAccessView(i, addresses[i]); break;
        default: break;
        }
    }
    list->SetComputeRoot32BitConstants(rp.first_extra, ra::kTableDwords, table, 0);
    list->SetComputeRootDescriptorTable(rp.first_extra + 1, D3D12_GPU_DESCRIPTOR_HANDLE{draw_heap->GetGPUDescriptorHandleForHeapStart().ptr + u64{first} * draw_heap_increment});
    ReleaseSRWLockExclusive(&draw_lock);
    indirect_dispatch(self, list, x, y, z);  // LOCAL: Dispatch, or the pending ExecuteIndirect (d3d12_indirect.inl)
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    const LONG done = InterlockedIncrement(&key.dispatches);
    if (x2w_local::first_of(done, 2)) log("x2w-rawaddr %016llx dispatch %ld (%u, %u, %u): regions %u, seeds resolved %u unresolved %u", key.key, done, x, y, z, count, resolved, unresolved);
    if ((done & 63) == 1) check_device("raw-address dispatch", 0);
    return true;
}

// Heartbeat: one line per key dispatched since the last report.
void rawaddr_report()
{
    AcquireSRWLockShared(&rawaddr_lock);
    for (const auto& [id, k] : rawaddr_keys()) {
        const LONG d = k->dispatches;
        if (d == k->reported_dispatches) continue;
        const std::uint32_t misses = rawaddr_stats_words ? rawaddr_stats_words[k->slot * (ra::kStatsBytes / 4)] : 0;
        log("x2w-rawaddr %016llx: dispatches %ld (+%ld), seeds resolved %ld unresolved %ld dropped %ld, gpu misses %u%s, learned %zu, skipped %ld",
            id, d, d - k->reported_dispatches, k->resolved, k->unresolved, k->overflow, misses, rawaddr_stats_words ? "" : " (not counted)",
            k->learned.size(), k->skipped);
        k->reported_dispatches = d;
    }
    ReleaseSRWLockShared(&rawaddr_lock);
}
