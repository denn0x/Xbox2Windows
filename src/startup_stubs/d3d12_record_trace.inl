// LOCAL (scene): X2W_MESH_RECORD_TRACE=1, traced frame only (trace_frame_active). Included after d3d12_mesh.inl.
//
// Who writes the indirect argument records, and what they hold when the frame's GPU work has finished:
//  (a) every ExecuteIndirect of the traced frame is noted with its argument buffer (title address + offset), the
//      call's key (mesh: the mesh binding's key, dispatch: the compute key) and depth target, and its first words
//      in title memory at record time;
//  (b) at the end of the traced frame (after a fence wait on the presenting queue when one is given) the same words
//      are read again: one "x2w-frame record" line per call (at record / at end), then totals per type and depth
//      target;
//  (c) every dispatch of the traced frame, run or skipped (direct Dispatch and indirect Dispatch), logs its key,
//      outcome and the buffer ranges it binds: "x2w-frame cx-uav" lines. Translated keys: the manifest's buffer
//      resources (U uav / S srv). Other keys: every buffer-shaped entry of each bound descriptor table (first 64
//      32-byte entries; '?' = may be an srv). Root views: R (the readable window from the root address, at most
//      1 MiB). The report then names, per argument buffer, the dispatches whose ranges cover the records
//      ("x2w-frame record writers").
// Nothing here runs outside a traced frame or without the switch. Owned files are only read: mesh_trace_key
// (d3d12_mesh.inl), indirect_current (d3d12_indirect.inl), compute_translations() (d3d12_compute.inl, under
// draw_lock, try-acquired) and view_records() (under note_lock).

bool record_trace_on()
{
    static const bool on = x2w_local::env_on("X2W_MESH_RECORD_TRACE");
    return on;
}

struct TracedRecord {
    LONG seq = 0;
    UINT type = 0;  // D3D12_INDIRECT_ARGUMENT_TYPE of the signature's last argument (10 = DispatchMesh)
    UINT stride = 0, max_count = 0;
    u64 key = 0;
    const void* list = nullptr;
    const void* dsv = nullptr;
    const void* wrapper = nullptr;
    const unsigned char* base = nullptr;  // title memory of the argument buffer
    u64 offset = 0;
    UINT words = 0;
    std::uint32_t at_record[4] = {};
};
struct TracedRange {
    LONG seq = 0;  // the dispatch's number in the traced frame
    u64 key = 0;
    const void* pipeline = nullptr;
    char label = '?';
    int param = 0, offset = 0;
    u64 lo = 0, hi = 0;
    const char* state = "";
};
SRWLOCK record_trace_lock = SRWLOCK_INIT;
std::vector<TracedRecord>& traced_records() { static std::vector<TracedRecord> v; return v; }
std::vector<TracedRange>& traced_ranges() { static std::vector<TracedRange> v; return v; }
LONG record_trace_seq, dispatch_trace_seq;
thread_local bool record_trace_dispatch_noted;  // set by record_trace_dispatch inside an indirect call

const char* record_trace_outcome(int outcome)
{
    static const char* const names[] = {"run (c62)", "skipped: c62 table argument", "skipped: c62 unmapped", "run",
                                        "skipped: X2W_CX_OFF", "skipped: not a slot-67 key", "skipped: no translation",
                                        "skipped: heap", "skipped: not run", "skipped: X2W_CX_SKIP", "run (rawaddr)",
                                        "skipped: entry value unresolved", "skipped: compiling (X2W_CX_ASYNC)"};
    static_assert(sizeof(names) / sizeof(names[0]) == co_count, "one name per ComputeOutcome");
    return outcome >= 0 && outcome < co_count ? names[outcome] : "skipped: ?";
}

// A 32-byte table entry as a buffer range: a runtime view token (slot 17 CBV, 18/19 buffer SRV / UAV) or a raw V#.
// False for empty entries, refused-view markers, image descriptors and anything else.
bool record_trace_entry(const std::uint32_t* w, u64* address, u64* bytes)
{
    *address = 0, *bytes = 0;
    if (w[0] == view_token_magic) {
        ViewRecord record{};
        AcquireSRWLockShared(&note_lock);
        const bool known = w[1] < view_records().size();
        if (known) record = view_records()[w[1]];
        ReleaseSRWLockShared(&note_lock);
        if (!known) return false;
        if (record.slot == 17 && record.desc_bytes >= 12) {
            std::uint32_t size = 0;
            std::memcpy(address, record.desc, 8);
            std::memcpy(&size, record.desc + 8, 4);
            *bytes = size;
        } else if ((record.slot == 18 || record.slot == 19) && record.desc_bytes >= 24) {
            Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
            x2w_startup::BufferViewLayout l;
            if (!resource || !resource->own().title_memory ||
                !x2w_startup::buffer_view_layout_from_desc(record.slot == 19, record.desc, record.desc_bytes, l, typed_views_off()))
                return false;
            *address = reinterpret_cast<u64>(resource->own().title_memory) + l.offset;
            *bytes = l.bytes;
        }
    } else if (w[0] != 0x52463258u /* refused-view marker */ && (w[3] >> 30) == 0) {
        *address = w[0] | (static_cast<u64>(w[1] & 0xffff) << 32);
        const std::uint32_t stride = (w[1] >> 16) & 0x3fff;
        *bytes = u64{stride ? stride : 1u} * w[2];
    }
    if (*bytes > (256ull << 20)) *bytes = 256ull << 20;
    return *address && *bytes;
}

// (c) execute_dispatch, every dispatch on this thread (direct, and indirect through indirect_run).
void record_trace_dispatch(Wrapper* self, u64 key, UINT x, UINT y, UINT z, bool executed, int outcome)
{
    if (!trace_frame_active || !record_trace_on()) return;
    const bool indirect = indirect_current != nullptr;
    if (indirect) record_trace_dispatch_noted = true;
    const LONG seq = InterlockedIncrement(&dispatch_trace_seq);
    const char* state = executed ? "run" : record_trace_outcome(outcome);
    const Pass& pass = pass_of(self);
    // The manifest's buffer resources, when the key is translated (state 1).
    struct Res { bool uav; int source, a, b; };
    std::vector<Res> manifest;
    bool have_manifest = false;
    if ((key >> 56) == 0x67) {
        bool locked = false;
        for (int i = 0; i < 200 && !(locked = TryAcquireSRWLockExclusive(&draw_lock)); ++i) Sleep(0);
        if (locked) {
            const auto found = compute_translations().find(key);
            if (found != compute_translations().end() && found->second.state == 1) {
                have_manifest = true;
                for (const auto& r : found->second.res)
                    if (r.kind == 0) manifest.push_back({r.uav, r.source, r.a, r.b});
            }
            ReleaseSRWLockExclusive(&draw_lock);
        }
    }
    std::vector<TracedRange> ranges;
    const auto add = [&](char label, int param, int offset, u64 address, u64 bytes) {
        TracedRange r;
        r.seq = seq, r.key = key, r.pipeline = pass.pipeline, r.label = label, r.param = param, r.offset = offset, r.lo = address, r.hi = address + bytes, r.state = state;
        ranges.push_back(r);
    };
    for (int p = 0; p < 16; ++p) {
        const int kind = pass.compute_kind[p];
        const u64 address = pass.compute_address[p];
        if ((kind == 3 || kind == 4) && address) {
            add('R', p, 0, address, readable_window(address, 1u << 20));
        } else if (kind == 5 && address) {
            const auto entry_at = [&](char label, int offset) {
                const u64 entry = address + static_cast<u64>(offset);
                std::uint32_t w[8];
                if (!x2w_local::readable(reinterpret_cast<const void*>(entry), 32)) return false;
                std::memcpy(w, reinterpret_cast<const void*>(entry), 32);
                u64 at = 0, bytes = 0;
                if (record_trace_entry(w, &at, &bytes)) add(label, p, offset, at, bytes);
                return true;
            };
            if (have_manifest) {  // translated: exactly the manifest's buffers in this table
                for (const Res& r : manifest)
                    if (r.source == 0 && r.a == p && r.b >= 0) entry_at(r.uav ? 'U' : 'S', r.b);
            } else {  // not translated: every buffer-shaped entry of the first 64
                for (int e = 0; e < 64 && entry_at('?', e * 32); ++e) {
                }
            }
        }
    }
    char text[1400];
    int n = std::snprintf(text, sizeof(text), "x2w-frame cx-uav #%ld key %016llx ", seq, static_cast<unsigned long long>(key));
    if (indirect) n += std::snprintf(text + n, sizeof(text) - n, "indirect");
    else n += std::snprintf(text + n, sizeof(text) - n, "(%u, %u, %u)", x, y, z);
    n += std::snprintf(text + n, sizeof(text) - n, " pipeline %p: %s%s:", static_cast<void*>(pass.pipeline), state, have_manifest || (key >> 56) != 0x67 ? "" : ", no manifest");
    if (ranges.empty()) n += std::snprintf(text + n, sizeof(text) - n, " no buffer bound");
    for (const TracedRange& r : ranges) {
        if (n > static_cast<int>(sizeof(text)) - 64) {
            log("%s ...", text);
            n = std::snprintf(text, sizeof(text), "x2w-frame cx-uav #%ld (cont.):", seq);
        }
        n += std::snprintf(text + n, sizeof(text) - n, " %c p%d+0x%x 0x%llx %llu;", r.label, r.param, r.offset,
                           static_cast<unsigned long long>(r.lo), static_cast<unsigned long long>(r.hi - r.lo));
    }
    log("%s", text);
    AcquireSRWLockExclusive(&record_trace_lock);
    traced_ranges().insert(traced_ranges().end(), ranges.begin(), ranges.end());
    ReleaseSRWLockExclusive(&record_trace_lock);
}

// (a) list_execute_indirect, after indirect_execute: the call's record at record time.
void record_trace_indirect(Wrapper* self, UINT type, UINT stride, UINT max_count, Wrapper* arguments, u64 offset)
{
    if (!trace_frame_active || !record_trace_on()) return;
    const bool dispatch_noted = record_trace_dispatch_noted;
    record_trace_dispatch_noted = false;
    TracedRecord t;
    t.seq = InterlockedIncrement(&record_trace_seq);
    t.type = type, t.stride = stride, t.max_count = max_count, t.list = self, t.wrapper = arguments, t.offset = offset;
    const Pass& pass = pass_of(self);
    t.dsv = pass.dsv_resource;
    t.key = type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH ? mesh_trace_key
          : type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH        ? compute_key_of(self)
                                                                 : 0;
    t.base = arguments && arguments->own().kind == resource_kind ? static_cast<const unsigned char*>(arguments->own().title_memory) : nullptr;
    const UINT words = stride / 4 < 4 ? stride / 4 : 4;
    if (t.base && words && x2w_local::readable(t.base + offset, words * 4)) {
        t.words = words;
        std::memcpy(t.at_record, t.base + offset, words * 4);
    }
    // An indirect Dispatch that never reached execute_dispatch (refused before staging) still gets its cx-uav line.
    if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH && !dispatch_noted) {
        IndirectPending* const saved = indirect_current;
        IndirectPending marker{};
        indirect_current = &marker;  // only read as "indirect" by record_trace_dispatch
        record_trace_dispatch(self, t.key, 0, 0, 0, false, co_not_run);
        record_trace_dispatch_noted = false;
        indirect_current = saved;
    }
    AcquireSRWLockExclusive(&record_trace_lock);
    traced_records().push_back(t);
    ReleaseSRWLockExclusive(&record_trace_lock);
}

// (b) + writers. queue: wait for its work first (the busy-frame end); null: read at once (no GPU wait).
void record_trace_report(ID3D12CommandQueue* queue)
{
    if (!record_trace_on()) return;
    AcquireSRWLockExclusive(&record_trace_lock);
    std::vector<TracedRecord> records;
    std::vector<TracedRange> ranges;
    records.swap(traced_records());
    ranges.swap(traced_ranges());
    ReleaseSRWLockExclusive(&record_trace_lock);
    InterlockedExchange(&record_trace_seq, 0);
    InterlockedExchange(&dispatch_trace_seq, 0);
    if (records.empty() && ranges.empty()) return;
    const char* when = "no GPU wait";
    if (queue && the_device) {
        ID3D12Fence* fence = nullptr;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event && SUCCEEDED(device_of(the_device)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
            SUCCEEDED(queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event)))
            when = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 ? "after GPU wait on the presenting queue" : "GPU wait timed out";
        if (fence) fence->Release();
        if (event) CloseHandle(event);
    }
    struct Total { LONG calls = 0, x_record = 0, x_end = 0, unreadable = 0; };
    std::map<std::pair<UINT, const void*>, Total> totals;
    std::map<const unsigned char*, std::vector<std::size_t>> by_buffer;
    std::vector<std::uint32_t> end_x(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const TracedRecord& t = records[i];
        std::uint32_t at_end[4] = {};
        const bool readable = t.words && x2w_local::readable(t.base + t.offset, t.words * 4);
        if (readable) std::memcpy(at_end, t.base + t.offset, t.words * 4);
        end_x[i] = at_end[0];
        Total& total = totals[{t.type, t.dsv}];
        ++total.calls;
        if (!readable) ++total.unreadable;
        if (readable && t.at_record[0]) ++total.x_record;
        if (readable && at_end[0]) ++total.x_end;
        if (t.base) by_buffer[t.base].push_back(i);
        log("x2w-frame record #%ld type %u key %016llx list %p dsv %p args %p+0x%llx (wrapper %p, stride %u, max %u): at record %08x %08x %08x %08x, "
            "at end %08x %08x %08x %08x%s",
            t.seq, t.type, static_cast<unsigned long long>(t.key), t.list, t.dsv, static_cast<const void*>(t.base),
            static_cast<unsigned long long>(t.offset), t.wrapper, t.stride, t.max_count, t.at_record[0], t.at_record[1], t.at_record[2],
            t.at_record[3], at_end[0], at_end[1], at_end[2], at_end[3], readable ? "" : " (unreadable)");
    }
    for (const auto& [k, total] : totals)
        log("x2w-frame record totals type %u dsv %p: calls %ld, X > 0 at record %ld, at end %ld, unreadable %ld (end read %s)", k.first, k.second,
            total.calls, total.x_record, total.x_end, total.unreadable, when);
    // Writers: per argument buffer, the dispatch ranges that cover at least one of its records.
    for (const auto& [base, indices] : by_buffer) {
        struct Writer { u64 key; const void* pipeline; char label; int param, offset; u64 lo, hi; const char* state; LONG dispatches = 0, first = 0; std::size_t covered = 0; };
        std::vector<Writer> writers;
        for (const TracedRange& r : ranges) {
            std::size_t covered = 0;
            for (const std::size_t i : indices) {
                const u64 at = reinterpret_cast<u64>(base) + records[i].offset;
                if (at >= r.lo && at + 4 <= r.hi) ++covered;
            }
            if (!covered) continue;
            Writer* w = nullptr;
            for (Writer& existing : writers)
                if (existing.key == r.key && existing.pipeline == r.pipeline && existing.label == r.label && existing.param == r.param && existing.offset == r.offset &&
                    existing.lo == r.lo && existing.hi == r.hi && std::strcmp(existing.state, r.state) == 0)
                    w = &existing;
            if (!w) {
                writers.push_back({r.key, r.pipeline, r.label, r.param, r.offset, r.lo, r.hi, r.state, 0, r.seq, 0});
                w = &writers.back();
            }
            ++w->dispatches;
            if (covered > w->covered) w->covered = covered;
        }
        std::size_t x_end = 0;
        for (const std::size_t i : indices) x_end += end_x[i] != 0;
        log("x2w-frame record writers args %p: %zu records (X > 0 at end %zu), %zu covering dispatch ranges%s", static_cast<const void*>(base),
            indices.size(), x_end, writers.size(), writers.empty() ? " (none of this frame's dispatches binds it)" : "");
        for (std::size_t n = 0; n < writers.size() && n < 32; ++n) {
            const Writer& w = writers[n];
            log("x2w-frame record writers args %p: key %016llx pipeline %p %c p%d+0x%x 0x%llx %llu bytes, %s, %ld dispatches (first #%ld), covers %zu records",
                static_cast<const void*>(base), static_cast<unsigned long long>(w.key), w.pipeline, w.label, w.param, w.offset,
                static_cast<unsigned long long>(w.lo), static_cast<unsigned long long>(w.hi - w.lo), w.state, w.dispatches, w.first, w.covered);
        }
        if (writers.size() > 32) log("x2w-frame record writers args %p: %zu more not shown", static_cast<const void*>(base), writers.size() - 32);
    }
}
extern "C" __declspec(dllexport) void x2w_d3d12_record_trace_report() { record_trace_report(nullptr); }  // fixtures, after their fence
