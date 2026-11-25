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
// Run 115 additions (same switch, same frame):
//  (d) image entries: each table entry that is a texture view token (slot 18 / 19, not a buffer) or a raw gfx10 T#
//      (word 3 TYPE 8-13) gives one "x2w-frame cx-img" line: the view or T# fields, the resource (wrapper, title
//      memory, desktop resource, its size / mips / format / flags) and what this frame did to it before the dispatch
//      ("content": drawn as a render / depth target N times so far (traced_targets), bound as a texture UAV by an
//      earlier dispatch of the frame (key, #, state), or no writer seen in this frame). A raw T# is matched to a
//      resource by its base address = the resource's title memory.
//  (e) heap reads: each 32-bit root constant of the dispatch whose value is below the title view heap's descriptor
//      count (or has bit 28 set with an index below it in bits 0-9, the mesh rule) is taken as a heap index
//      (ASSUMED: a root constant that is a plain number gives an extra line); the descriptor there gives one
//      "x2w-frame cx-heap" line (buffer range, image, or other) and a buffer range is kept as an 'H' range, so the
//      writers and mesh-read lines see it. At most 8 per dispatch.
//  (f) mesh reads: every buffer a traced mesh call binds (table / root srv) and every heap buffer it names through a
//      bit-28 root constant (the X2W_MESH_DUMP_INPUTS rule) is noted; the report gives, per distinct range, the
//      dispatch ranges of the frame that overlap it, skipped ones first ("x2w-frame mesh-read"), and totals.
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
    LONG order = 0;  // record order shared with the mesh reads
    bool executed = false;
};
// (f) one buffer a traced mesh call reads: kind 'S' srv i (source a b), 'H' heap descriptor `slot` (root constant p w).
struct TracedMeshRead {
    u64 key = 0;
    char kind = 'S';
    int slot = 0, source = 0, a = 0, b = 0;
    u64 lo = 0, hi = 0;
    LONG order = 0;
};
SRWLOCK record_trace_lock = SRWLOCK_INIT;
std::vector<TracedRecord>& traced_records() { static std::vector<TracedRecord> v; return v; }
std::vector<TracedRange>& traced_ranges() { static std::vector<TracedRange> v; return v; }
std::vector<TracedMeshRead>& traced_mesh_reads() { static std::vector<TracedMeshRead> v; return v; }
// (d) texture UAVs bound by this frame's dispatches so far: title wrapper -> "key K #n state".
std::map<const void*, std::string>& traced_image_writers() { static std::map<const void*, std::string> m; return m; }
// (d) raw T# base -> (first resource starting there, how many): one resource walk per base per traced frame.
std::map<u64, std::pair<Wrapper*, int>>& traced_tsharp_bases() { static std::map<u64, std::pair<Wrapper*, int>> m; return m; }
LONG record_trace_seq, dispatch_trace_seq, record_trace_order;
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

// (d) The resource half of an image line: wrapper, title memory, desktop resource and what this frame did to it.
// No side effects (no memcut revive, no tiled stand-in made): a released resource says so.
void record_trace_resource_text(Wrapper* resource, char* text, std::size_t room)
{
    if (!resource || resource->own().kind != resource_kind) {
        std::snprintf(text, room, "resource unknown");
        return;
    }
    const bool live = InterlockedCompareExchange(&resource->own().refs, 0, 0) > 0;
    auto* desktop = static_cast<ID3D12Resource*>(resource->own().real);
    int n = std::snprintf(text, room, "res %p title 0x%llx desktop %p%s", static_cast<void*>(resource),
                          static_cast<unsigned long long>(reinterpret_cast<u64>(resource->own().title_memory)), static_cast<void*>(desktop),
                          resource->own().tiled_address_srv ? " (tiled address srv: sampled through a stand-in)" : "");
    if (desktop && live) {
        const D3D12_RESOURCE_DESC d = desktop->GetDesc();
        n += std::snprintf(text + n, room - n, " %llux%ux%u mips %u fmt %d flags 0x%x", static_cast<unsigned long long>(d.Width), d.Height,
                           d.DepthOrArraySize, d.MipLevels, static_cast<int>(d.Format), static_cast<unsigned>(d.Flags));
    } else {
        n += std::snprintf(text + n, room - n, " (%s)", live ? "no desktop resource" : "released");
    }
    // Content: this frame's writers before now.
    int drawn = -1;
    if (desktop) {
        AcquireSRWLockShared(&traced_lock);
        for (const auto& t : traced_targets())
            if (t.first == desktop) drawn = t.second;
        ReleaseSRWLockShared(&traced_lock);
    }
    std::string uav;
    AcquireSRWLockShared(&record_trace_lock);
    const auto found = traced_image_writers().find(resource);
    if (found != traced_image_writers().end()) uav = found->second;
    ReleaseSRWLockShared(&record_trace_lock);
    if (drawn < 0 && uav.empty()) {
        std::snprintf(text + n, room - n, "; content: no writer seen in this frame before this dispatch%s",
                      desktop && live && !(desktop->GetDesc().Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                          ? " (no GPU-write flags: uploaded from title memory)" : "");
        return;
    }
    n += std::snprintf(text + n, room - n, "; content:");
    if (drawn >= 0) n += std::snprintf(text + n, room - n, " render/depth target of this frame, %d drawn draws so far;", drawn);
    if (!uav.empty()) std::snprintf(text + n, room - n, " texture uav of %s", uav.c_str());
}

// (d) A 32-byte entry as an image: a view token of a texture view, or a raw gfx10 T#. Fills `text` and returns the
// title wrapper (or null); *uav: a UAV view (slot 19). False: not an image entry.
bool record_trace_image(const std::uint32_t* w, char* text, std::size_t room, Wrapper** wrapper_out, bool* uav)
{
    *wrapper_out = nullptr, *uav = false;
    char res[400];
    if (w[0] == view_token_magic) {
        ViewRecord record{};
        AcquireSRWLockShared(&note_lock);
        const bool known = w[1] < view_records().size();
        if (known) record = view_records()[w[1]];
        ReleaseSRWLockShared(&note_lock);
        if (!known || (record.slot != 18 && record.slot != 19) || record.desc_bytes < 12) return false;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT dimension = 0, mip = 0;
        std::memcpy(&format, record.desc, 4);
        std::memcpy(&dimension, record.desc + 4, 4);
        // SRV: ViewDimension 1 = buffer; UAV: 1 = buffer. Both: the first mip at +16 (SRV MostDetailedMip) / +8 (UAV MipSlice).
        if (dimension == 1 || dimension == 0) return false;
        if (record.slot == 18 && record.desc_bytes >= 20) std::memcpy(&mip, record.desc + 16, 4);
        if (record.slot == 19 && record.desc_bytes >= 12) std::memcpy(&mip, record.desc + 8, 4);
        Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
        record_trace_resource_text(resource, res, sizeof(res));
        std::snprintf(text, room, "view %u (%s) dim %u fmt %d mip %u; %s", w[1], record.slot == 19 ? "uav" : "srv", dimension,
                      static_cast<int>(format), mip, res);
        *wrapper_out = resource;
        *uav = record.slot == 19;
        return true;
    }
    const std::uint32_t type = w[3] >> 28;
    if (w[0] == 0x52463258u || type < 8 || type > 13) return false;
    const u64 address = (static_cast<u64>(w[0]) | (static_cast<u64>(w[1] & 0xff) << 32)) << 8;
    const std::uint32_t width = (((w[1] >> 30) & 3) | ((w[2] & 0xfff) << 2)) + 1, height = ((w[2] >> 14) & 0x3fff) + 1;
    // The resource whose title memory starts at the T# base (a texture; several: the first, counted).
    // One walk of the resources per distinct base in the traced frame (the cache is emptied at the report).
    Wrapper* resource = nullptr;
    int matches = 0;
    bool cached = false;
    AcquireSRWLockShared(&record_trace_lock);
    if (const auto hit = traced_tsharp_bases().find(address); hit != traced_tsharp_bases().end())
        cached = true, resource = hit->second.first, matches = hit->second.second;
    ReleaseSRWLockShared(&record_trace_lock);
    if (address && !cached) {
        AcquireSRWLockShared(&note_lock);
        for (const auto& [wrapper, info] : resource_infos())
            if (reinterpret_cast<u64>(wrapper->own().title_memory) == address && info.console.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                InterlockedCompareExchange(&wrapper->own().refs, 0, 0) > 0) {
                if (!resource) resource = const_cast<Wrapper*>(wrapper);
                ++matches;
            }
        ReleaseSRWLockShared(&note_lock);
        AcquireSRWLockExclusive(&record_trace_lock);
        traced_tsharp_bases()[address] = {resource, matches};
        ReleaseSRWLockExclusive(&record_trace_lock);
    }
    if (resource) record_trace_resource_text(resource, res, sizeof(res));
    else std::snprintf(res, sizeof(res), "no resource starts at the base");
    std::snprintf(text, room, "T# 0x%llx %ux%u depth %u fmt %u levels %u-%u type %u sw %u; %s%s", static_cast<unsigned long long>(address), width,
                  height, (w[4] & 0x1fff) + 1, (w[1] >> 20) & 0x1ff, (w[3] >> 12) & 15, (w[3] >> 16) & 15, type, (w[3] >> 20) & 0x1f, res,
                  matches > 1 ? " (several resources start there: the first)" : "");
    *wrapper_out = resource;
    return true;
}

// (e) The title CBV_SRV_UAV heap of a list: the bound one (SetDescriptorHeaps), else the one holding a bound compute
// table, else the newest (title_view_heap's rule).
struct TraceHeap {
    u64 base = 0, bytes = 0;
    UINT increment = 32, count = 0;
};
TraceHeap record_trace_heap(const Pass& pass)
{
    TraceHeap out;
    AcquireSRWLockShared(&note_lock);
    const HeapInfo* best = nullptr;
    for (const HeapInfo& h : heap_infos()) {
        if (h.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !h.title_cpu || !h.increment) continue;
        if (pass.source_view_heap && h.wrapper == pass.source_view_heap) {
            best = &h;
            break;
        }
        for (UINT i = 0; i < 16; ++i)
            if (pass.compute_kind[i] == 5 && pass.compute_address[i] >= h.title_cpu && pass.compute_address[i] < h.title_cpu + u64{h.count} * h.increment)
                best = &h;
        if (!best) best = &h;
    }
    if (best) out.base = best->title_cpu, out.increment = best->increment, out.count = best->logical_count ? best->logical_count : best->count,
              out.bytes = u64{out.count} * best->increment;
    ReleaseSRWLockShared(&note_lock);
    return out;
}
// (e) The descriptor at heap index `index`: a buffer range (true, *address / *bytes) or text for anything else.
bool record_trace_heap_slot(const TraceHeap& heap, std::uint32_t index, u64* address, u64* bytes, char* text, std::size_t room, Wrapper** image)
{
    *address = 0, *bytes = 0, *image = nullptr;
    text[0] = 0;
    const u64 slot = heap.base + u64{index} * heap.increment;
    std::uint32_t w[8] = {};
    if (!heap.base || index >= heap.count || !x2w_local::readable(reinterpret_cast<const void*>(slot), 32)) {
        std::snprintf(text, room, "slot 0x%llx not readable", static_cast<unsigned long long>(slot));
        return false;
    }
    std::memcpy(w, reinterpret_cast<const void*>(slot), 32);
    if (record_trace_entry(w, address, bytes)) return true;
    bool uav = false;
    char image_text[700];
    if (record_trace_image(w, image_text, sizeof(image_text), image, &uav)) {
        std::snprintf(text, room, "image: %s", image_text);
        return false;
    }
    std::snprintf(text, room, "other: words %08x %08x %08x %08x", w[0], w[1], w[2], w[3]);
    return false;
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
    struct Res { bool uav; int kind, source, a, b; };
    std::vector<Res> manifest;
    bool have_manifest = false;
    if ((key >> 56) == 0x67) {
        bool locked = false;
        for (int i = 0; i < 200 && !(locked = TryAcquireSRWLockExclusive(&draw_lock)); ++i) Sleep(0);
        if (locked) {
            const auto found = compute_translations().find(key);
            if (found != compute_translations().end() && found->second.state == 1) {
                have_manifest = true;
                for (const auto& r : found->second.res) manifest.push_back({r.uav, r.kind, r.source, r.a, r.b});
            }
            ReleaseSRWLockExclusive(&draw_lock);
        }
    }
    const LONG order = InterlockedIncrement(&record_trace_order);
    std::vector<TracedRange> ranges;
    const auto add = [&](char label, int param, int offset, u64 address, u64 bytes) {
        TracedRange r;
        r.seq = seq, r.key = key, r.pipeline = pass.pipeline, r.label = label, r.param = param, r.offset = offset, r.lo = address, r.hi = address + bytes, r.state = state;
        r.order = order, r.executed = executed;
        ranges.push_back(r);
    };
    // (d) image entries: logged after the cx-uav line; texture UAVs become writers once this dispatch is logged.
    std::vector<std::string> image_lines;
    std::vector<const void*> uav_images;
    const auto image_at = [&](char label, int param, int offset, const std::uint32_t* w) {
        if (image_lines.size() >= 16) return;
        Wrapper* resource = nullptr;
        bool uav_view = false;
        char text[900];
        if (!record_trace_image(w, text, sizeof(text), &resource, &uav_view)) {
            // Neither buffer nor image (refused-view marker, a view token without a usable desc, an "integer + 7 zero
            // words" heap-index entry, a sampler, a neighbouring table's bytes): the raw words, so no entry is silent.
            bool any = false;
            for (int k = 0; k < 8; ++k) any |= w[k] != 0;
            if (!any) return;
            int n = std::snprintf(text, sizeof(text), "other: words %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            if (w[0] == view_token_magic) {
                ViewRecord record{};
                AcquireSRWLockShared(&note_lock);
                const bool known = w[1] < view_records().size();
                if (known) record = view_records()[w[1]];
                ReleaseSRWLockShared(&note_lock);
                if (known) std::snprintf(text + n, sizeof(text) - n, " (view token: slot %u, desc %u bytes, resource 0x%llx)", record.slot, record.desc_bytes,
                                         static_cast<unsigned long long>(record.resource));
                else std::snprintf(text + n, sizeof(text) - n, " (view token: unknown view)");
            } else if (w[0] == 0x52463258u) {
                std::snprintf(text + n, sizeof(text) - n, " (refused-view marker)");
            } else if (w[0] && !(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
                std::snprintf(text + n, sizeof(text) - n, " (integer + 7 zero words: a heap index?)");
            }
        }
        const bool uav = uav_view || label == 'U';
        char line[1100];
        std::snprintf(line, sizeof(line), "x2w-frame cx-img #%ld key %016llx %c p%d+0x%x%s: %s", seq, static_cast<unsigned long long>(key), label, param,
                      offset, uav ? " (uav)" : "", text);
        image_lines.push_back(line);
        if (uav && resource) uav_images.push_back(resource);
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
                else image_at(label == 'S' ? 'T' : label, p, offset, w);  // (d)
                return true;
            };
            if (have_manifest) {  // translated: exactly the manifest's buffers (and, d, images) in this table
                for (const Res& r : manifest)
                    if (r.source == 0 && r.a == p && r.b >= 0) entry_at(r.uav ? 'U' : 'S', r.b);
            } else {  // not translated: every buffer-shaped (and image) entry of the first 64
                for (int e = 0; e < 64 && entry_at('?', e * 32); ++e) {
                }
            }
        }
    }
    // (e) heap descriptors named by root constants (ASSUMED rule, see the head of this file).
    std::vector<std::string> heap_lines;
    {
        const TraceHeap heap = record_trace_heap(pass);
        // Skipped dispatches: the raw root arguments and the heap used, so the index / base mapping can be checked
        // offline (a constant equal to the heap's low 32 bits, or to a table's, is marked).
        if (!executed) {
            char line[1400];
            int n = std::snprintf(line, sizeof(line), "x2w-frame cx-root #%ld key %016llx: heap 0x%llx (low 0x%08x) count %u increment %u;", seq,
                                  static_cast<unsigned long long>(key), static_cast<unsigned long long>(heap.base), static_cast<std::uint32_t>(heap.base),
                                  heap.count, heap.increment);
            for (int p = 0; p < 16 && n < static_cast<int>(sizeof(line)) - 160; ++p) {
                const int kind = pass.compute_kind[p];
                if (!kind) continue;
                if (kind != 1) {
                    n += std::snprintf(line + n, sizeof(line) - n, " p%d kind %d 0x%llx (low 0x%08x);", p, kind,
                                       static_cast<unsigned long long>(pass.compute_address[p]), static_cast<std::uint32_t>(pass.compute_address[p]));
                    continue;
                }
                n += std::snprintf(line + n, sizeof(line) - n, " p%d constants %u:", p, pass.compute_count[p]);
                for (UINT w = 0; w < pass.compute_count[p] && w < 16 && n < static_cast<int>(sizeof(line)) - 40; ++w) {
                    const std::uint32_t c = pass.compute_constants[p][w];
                    bool table_low = false;
                    for (int q = 0; q < 16; ++q) table_low |= pass.compute_kind[q] == 5 && c && c == static_cast<std::uint32_t>(pass.compute_address[q]);
                    n += std::snprintf(line + n, sizeof(line) - n, " %08x%s", c,
                                       heap.base && c == static_cast<std::uint32_t>(heap.base) ? "(=heap low)" : table_low ? "(=table low)" : "");
                }
                n += std::snprintf(line + n, sizeof(line) - n, ";");
            }
            heap_lines.push_back(line);
        }
        unsigned looked = 0;
        std::uint32_t seen[8];
        for (int p = 0; p < 16 && heap.base && looked < 8; ++p) {
            if (pass.compute_kind[p] != 1) continue;
            for (UINT w = 0; w < pass.compute_count[p] && w < 16 && looked < 8; ++w) {
                const std::uint32_t c = pass.compute_constants[p][w];
                const std::uint32_t index = c < heap.count ? c : (c & (1u << 28)) && (c & 0x3ff) < heap.count ? (c & 0x3ff) : 0xffffffffu;
                if (index == 0xffffffffu) continue;
                bool again = false;
                for (unsigned k = 0; k < looked; ++k) again |= seen[k] == index;
                if (again) continue;
                seen[looked++] = index;
                u64 at = 0, bytes = 0;
                Wrapper* image = nullptr;
                char text[800];
                const bool buffer = record_trace_heap_slot(heap, index, &at, &bytes, text, sizeof(text), &image);
                if (buffer) {
                    add('H', p, static_cast<int>(w * 4), at, bytes);
                    std::snprintf(text, sizeof(text), "buffer 0x%llx %llu bytes", static_cast<unsigned long long>(at), static_cast<unsigned long long>(bytes));
                }
                char line[1100];
                std::snprintf(line, sizeof(line), "x2w-frame cx-heap #%ld key %016llx root constant p%d w%u = 0x%08x -> heap 0x%llx index %u (count %u, increment %u): %s",
                              seq, static_cast<unsigned long long>(key), p, w, c, static_cast<unsigned long long>(heap.base), index, heap.count,
                              heap.increment, text);
                heap_lines.push_back(line);
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
    for (const std::string& line : image_lines) log("%s", line.c_str());
    for (const std::string& line : heap_lines) log("%s", line.c_str());
    char writer[96];
    std::snprintf(writer, sizeof(writer), "key %016llx #%ld (%s)", static_cast<unsigned long long>(key), seq, state);
    AcquireSRWLockExclusive(&record_trace_lock);
    traced_ranges().insert(traced_ranges().end(), ranges.begin(), ranges.end());
    for (const void* image : uav_images) traced_image_writers()[image] = writer;
    ReleaseSRWLockExclusive(&record_trace_lock);
}

// (f) d3d12_draw.inl draw_translated, a traced mesh call: one buffer srv it binds (title address and full size).
void record_trace_mesh_read(u64 key, UINT srv, int source, int a, int b, u64 address, u64 bytes)
{
    if (!record_trace_on() || !address || !bytes) return;
    TracedMeshRead r;
    r.key = key, r.kind = 'S', r.slot = static_cast<int>(srv), r.source = source, r.a = a, r.b = b, r.lo = address, r.hi = address + bytes;
    r.order = InterlockedIncrement(&record_trace_order);
    AcquireSRWLockExclusive(&record_trace_lock);
    traced_mesh_reads().push_back(r);
    ReleaseSRWLockExclusive(&record_trace_lock);
}
// (f) the heap buffers a traced mesh call names through root constants the translation reads with bit 28 set
// (index in bits 0-9: the X2W_MESH_DUMP_INPUTS rule, ASSUMED).
void record_trace_mesh_heap(Wrapper* self, const Translation& tr)
{
    if (!record_trace_on()) return;
    const Pass& pass = pass_of(self);
    const TraceHeap heap = record_trace_heap(pass);
    if (!heap.base) return;
    std::vector<TracedMeshRead> reads;
    for (const Translation::Ud& ud : tr.uds) {
        if (ud.kind != Translation::root_constant || ud.a < 0 || ud.a >= 16 || ud.b < 0 || ud.b >= 8) continue;
        const std::uint32_t c = pass.constants[ud.a][ud.b];
        if (!(c & (1u << 28)) || (c & 0x3ff) >= heap.count) continue;
        u64 at = 0, bytes = 0;
        Wrapper* image = nullptr;
        char text[800];
        if (!record_trace_heap_slot(heap, c & 0x3ff, &at, &bytes, text, sizeof(text), &image)) continue;
        bool again = false;
        for (const TracedMeshRead& r : reads) again |= r.lo == at && r.hi == at + bytes;
        if (again) continue;
        TracedMeshRead r;
        r.key = tr.key, r.kind = 'H', r.slot = static_cast<int>(c & 0x3ff), r.a = ud.a, r.b = ud.b, r.lo = at, r.hi = at + bytes;
        r.order = InterlockedIncrement(&record_trace_order);
        reads.push_back(r);
    }
    if (reads.empty()) return;
    AcquireSRWLockExclusive(&record_trace_lock);
    traced_mesh_reads().insert(traced_mesh_reads().end(), reads.begin(), reads.end());
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
    std::vector<TracedMeshRead> mesh_reads;
    records.swap(traced_records());
    ranges.swap(traced_ranges());
    mesh_reads.swap(traced_mesh_reads());
    traced_image_writers().clear();
    traced_tsharp_bases().clear();
    ReleaseSRWLockExclusive(&record_trace_lock);
    InterlockedExchange(&record_trace_seq, 0);
    InterlockedExchange(&dispatch_trace_seq, 0);
    InterlockedExchange(&record_trace_order, 0);
    if (records.empty() && ranges.empty() && mesh_reads.empty()) return;
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
    // (f) mesh reads: per distinct (key, kind, slot, range), the dispatch ranges of the frame that overlap it.
    struct Read { TracedMeshRead r; LONG calls = 0, first_order = 0; };
    std::vector<Read> distinct;
    for (const TracedMeshRead& m : mesh_reads) {
        Read* d = nullptr;
        for (Read& e : distinct)
            if (e.r.key == m.key && e.r.kind == m.kind && e.r.slot == m.slot && e.r.lo == m.lo && e.r.hi == m.hi) d = &e;
        if (!d) {
            distinct.push_back({m, 0, m.order});
            d = &distinct.back();
        }
        ++d->calls;
    }
    std::size_t in_skipped = 0, in_run_only = 0, in_none = 0, shown = 0, in_skipped_entry = 0;
    std::vector<std::string> later;  // lines without a skipped overlap, shown after the ones with
    // Reverse view: per skipped dispatch range, the mesh reads that overlap it (not capped by the per-read lines).
    struct ByRange { const TracedRange* r; LONG dispatches = 0; std::size_t reads = 0; LONG calls = 0; u64 keys[4] = {}; unsigned nkeys = 0; u64 overlap = 0; };
    std::vector<ByRange> by_range;
    // Skipped ranges that overlap any mesh read, merged per (key, label, param, offset, range); dispatches = how many
    // skipped dispatches bound that same range.
    for (const TracedRange& r : ranges) {
        if (r.executed) continue;
        bool overlaps = false;
        for (const Read& d : distinct) overlaps |= !(r.hi <= d.r.lo || r.lo >= d.r.hi);
        if (!overlaps) continue;
        ByRange* b = nullptr;
        for (ByRange& e : by_range)
            if (e.r->key == r.key && e.r->label == r.label && e.r->param == r.param && e.r->offset == r.offset && e.r->lo == r.lo && e.r->hi == r.hi) b = &e;
        if (!b) {
            by_range.push_back({&r});
            b = &by_range.back();
        }
        ++b->dispatches;
    }
    for (const Read& d : distinct) {
        bool entry_hit = false;
        for (const ByRange& b : by_range) entry_hit |= b.r->label != 'R' && !(b.r->hi <= d.r.lo || b.r->lo >= d.r.hi);
        in_skipped_entry += entry_hit;
    }
    // Reads and calls per range.
    for (ByRange& b : by_range) {
        for (const Read& d : distinct) {
            if (b.r->hi <= d.r.lo || b.r->lo >= d.r.hi) continue;
            ++b.reads;
            b.calls += d.calls;
            const u64 lo = b.r->lo > d.r.lo ? b.r->lo : d.r.lo, hi = b.r->hi < d.r.hi ? b.r->hi : d.r.hi;
            b.overlap += hi - lo;
            bool known = false;
            for (unsigned k = 0; k < b.nkeys; ++k) known |= b.keys[k] == d.r.key;
            if (!known && b.nkeys < 4) b.keys[b.nkeys++] = d.r.key;
        }
    }
    std::stable_sort(by_range.begin(), by_range.end(), [](const ByRange& x, const ByRange& y) { return x.r->label != 'R' && y.r->label == 'R'; });
    for (const Read& d : distinct) {
        // Overlapping dispatch ranges, merged per (key, label, param, offset, range, state): skipped first.
        struct Hit { const TracedRange* r; LONG dispatches; bool before; };
        std::vector<Hit> hits;
        bool skipped = false;
        for (const TracedRange& r : ranges) {
            if (r.hi <= d.r.lo || r.lo >= d.r.hi) continue;
            Hit* h = nullptr;
            for (Hit& e : hits)
                if (e.r->key == r.key && e.r->label == r.label && e.r->param == r.param && e.r->offset == r.offset && e.r->lo == r.lo && e.r->hi == r.hi &&
                    e.r->executed == r.executed)
                    h = &e;
            if (!h) {
                hits.push_back({&r, 0, false});
                h = &hits.back();
            }
            ++h->dispatches;
            h->before |= r.order < d.first_order;
            skipped |= !r.executed;
        }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return !x.r->executed && y.r->executed; });
        if (skipped) ++in_skipped;
        else if (!hits.empty()) ++in_run_only;
        else ++in_none;
        char text[1400];
        int n = std::snprintf(text, sizeof(text), "x2w-frame mesh-read key %016llx %c%d", static_cast<unsigned long long>(d.r.key), d.r.kind, d.r.slot);
        if (d.r.kind == 'S') n += std::snprintf(text + n, sizeof(text) - n, " (source %d %d+0x%x)", d.r.source, d.r.a, d.r.b);
        else n += std::snprintf(text + n, sizeof(text) - n, " (heap index %d, root constant p%d w%d)", d.r.slot, d.r.a, d.r.b);
        n += std::snprintf(text + n, sizeof(text) - n, " 0x%llx %llu bytes, %ld calls: %s", static_cast<unsigned long long>(d.r.lo),
                           static_cast<unsigned long long>(d.r.hi - d.r.lo), d.calls,
                           skipped ? "INSIDE a skipped dispatch's range" : hits.empty() ? "no dispatch of this frame binds it" : "run dispatches only");
        for (std::size_t k = 0; k < hits.size() && k < 6 && n < static_cast<int>(sizeof(text)) - 200; ++k) {
            const TracedRange& r = *hits[k].r;
            const u64 lo = r.lo > d.r.lo ? r.lo : d.r.lo, hi = r.hi < d.r.hi ? r.hi : d.r.hi;
            n += std::snprintf(text + n, sizeof(text) - n, "; key %016llx %c p%d+0x%x 0x%llx %llu bytes, %s, %ld dispatches (first #%ld)%s, overlap %llu bytes",
                               static_cast<unsigned long long>(r.key), r.label, r.param, r.offset, static_cast<unsigned long long>(r.lo),
                               static_cast<unsigned long long>(r.hi - r.lo), r.state, hits[k].dispatches, r.seq,
                               hits[k].before ? " recorded before the first read" : " recorded after the first read",
                               static_cast<unsigned long long>(hi - lo));
        }
        if (hits.size() > 6) std::snprintf(text + n, sizeof(text) - n, "; %zu more", hits.size() - 6);
        if (skipped && shown < 64) {
            log("%s", text);
            ++shown;
        } else if (!skipped && later.size() < 64) {
            later.push_back(text);
        }
    }
    for (std::size_t k = 0; k < later.size() && shown < 64; ++k, ++shown) log("%s", later[k].c_str());
    // The reverse view, table / heap entries first, then root windows (R: a readable window, not a size).
    for (std::size_t k = 0; k < by_range.size() && k < 256; ++k) {
        const ByRange& b = by_range[k];
        char keys[96] = "";
        int n = 0;
        for (unsigned j = 0; j < b.nkeys; ++j) n += std::snprintf(keys + n, sizeof(keys) - n, "%s%016llx", j ? " " : "", static_cast<unsigned long long>(b.keys[j]));
        log("x2w-frame mesh-read by-range key %016llx %c p%d+0x%x 0x%llx %llu bytes, %s, %ld dispatches (first #%ld)%s: %zu distinct mesh reads, %ld calls, "
            "overlap %llu bytes, mesh keys %s%s",
            static_cast<unsigned long long>(b.r->key), b.r->label, b.r->param, b.r->offset, static_cast<unsigned long long>(b.r->lo),
            static_cast<unsigned long long>(b.r->hi - b.r->lo), b.r->state, b.dispatches, b.r->seq, b.r->label == 'R' ? " (root window)" : "", b.reads,
            b.calls, static_cast<unsigned long long>(b.overlap), keys, b.nkeys == 4 ? " ..." : "");
    }
    if (by_range.size() > 256) log("x2w-frame mesh-read by-range: %zu more not shown", by_range.size() - 256);
    if (!mesh_reads.empty())
        log("x2w-frame mesh-read totals: %zu reads, %zu distinct ranges: %zu inside a skipped dispatch's range (%zu inside a skipped table / heap entry, "
            "%zu only inside a skipped root window), %zu run dispatches only, %zu bound by no dispatch of this frame (%zu lines shown, %zu skipped ranges hit)",
            mesh_reads.size(), distinct.size(), in_skipped, in_skipped_entry, in_skipped - in_skipped_entry, in_run_only, in_none, shown, by_range.size());
}
extern "C" __declspec(dllexport) void x2w_d3d12_record_trace_report() { record_trace_report(nullptr); }  // fixtures, after their fence
