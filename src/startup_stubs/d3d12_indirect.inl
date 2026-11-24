// Local experiment (never upstream): ExecuteIndirect (list slot 61) on the desktop.
// Included inside d3d12_xs_device.cpp's anonymous namespace, after d3d12_compute.inl, so the window
// lookup (desktop_range), the translated dispatch and draw paths and the Pass state are visible.
//
// The console call is ExecuteIndirect(signature, max count, argument resource, argument offset,
// count resource, count offset), the desktop shape. The five signatures the title creates each hold
// one argument: Draw (stride 16), DrawIndexed (20), Dispatch (12), DispatchMesh (12) and a console
// type 0x69 (12) that the desktop cannot create (device_create_command_signature gives it a
// placeholder). See out/gamerun/indirect_work/DESIGN.md.
//
// This part counts every call by signature type and the reason it was not executed; the counts are
// logged every 300 frames by report_indirect (no per-call log lines; the first calls are described
// once).

// Signature type index for counters: 0 Draw, 1 DrawIndexed, 2 Dispatch, 3 DispatchMesh, 4 other.
enum IndirectType { indirect_draw, indirect_indexed, indirect_dispatch, indirect_mesh, indirect_other, indirect_types };
constexpr const char* indirect_type_names[indirect_types] = {"draw", "indexed", "dispatch", "mesh", "other"};
int indirect_type_of(UINT argument_type)
{
    switch (argument_type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: return indirect_draw;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: return indirect_indexed;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: return indirect_dispatch;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: return indirect_mesh;
    default: return indirect_other;
    }
}

// Why a call was not executed. "not-implemented": no execution path exists yet for the type.
enum IndirectSkip {
    skip_not_recording,      // the list is not recording a desktop list (not render mode, or closed)
    skip_unknown_signature,  // the signature was not created through device slot 41
    skip_not_implemented,    // no execution path for this type (yet)
    skip_switched_off,       // the type's env switch is off
    skip_no_pipeline,        // the bound pipeline has no desktop translation
    skip_unresolved_buffer,  // argument or count buffer has no in-place desktop window
    skip_zero_count,         // max count 0
    skip_reasons
};
constexpr const char* indirect_skip_names[skip_reasons] = {"not-recording", "unknown-signature", "not-implemented", "off",
                                                           "no-pipeline", "unresolved-buffer", "zero-count"};

struct IndirectCounters {
    LONG calls[indirect_types];
    LONG executed[indirect_types];
    LONG skipped[skip_reasons][indirect_types];
    LONG with_count_buffer;    // calls that pass a count resource
    LONG max_count_above_one;  // calls with max count > 1
    LONG max_count_max;        // largest max count seen
};
IndirectCounters indirect_counts;

void indirect_skip(int type, IndirectSkip why) { InterlockedIncrement(&indirect_counts.skipped[why][type]); }

// The signature's stride and last argument type, through a one-entry per-thread cache in front of the
// shared signature table (about 5000 calls a frame use five signatures).
bool indirect_signature(const void* signature, UINT* stride, UINT* argument_type)
{
    thread_local const void* last = nullptr;
    thread_local UINT last_stride = 0, last_type = 0;
    if (signature && signature == last) {
        *stride = last_stride;
        *argument_type = last_type;
        return true;
    }
    bool found = false;
    AcquireSRWLockShared(&signature_lock);
    const auto it = signature_infos().find(signature);
    if (it != signature_infos().end() && !it->second.args.empty()) {
        *stride = it->second.stride;
        *argument_type = static_cast<UINT>(it->second.args.back().Type);
        found = true;
    }
    ReleaseSRWLockShared(&signature_lock);
    if (found) {
        last = signature;
        last_stride = *stride;
        last_type = *argument_type;
    }
    return found;
}

void max_count_seen(UINT max_count)
{
    if (max_count > 1) InterlockedIncrement(&indirect_counts.max_count_above_one);
    LONG seen = indirect_counts.max_count_max;
    while (static_cast<LONG>(max_count) > seen) {
        const LONG previous = InterlockedCompareExchange(&indirect_counts.max_count_max, static_cast<LONG>(max_count), seen);
        if (previous == seen) break;
        seen = previous;
    }
}

// List slot 61 (called from list_execute_indirect before its frame trace).
void indirect_execute(Wrapper* self, void* signature, UINT max_count, Wrapper* arguments, u64 offset, Wrapper* count_resource,
                      u64 count_offset)
{
    UINT stride = 0, argument_type = ~0u;
    const bool known = indirect_signature(signature, &stride, &argument_type);
    const int type = known ? indirect_type_of(argument_type) : indirect_other;
    InterlockedIncrement(&indirect_counts.calls[type]);
    if (count_resource) InterlockedIncrement(&indirect_counts.with_count_buffer);
    max_count_seen(max_count);
    static LONG described;
    if (first_calls(described, 16)) {
        const void* args_memory = arguments && arguments->own().kind == resource_kind ? arguments->own().title_memory : nullptr;
        const void* count_memory = count_resource && count_resource->own().kind == resource_kind ? count_resource->own().title_memory : nullptr;
        log("x2w-indirect call: signature %p type %u stride %u, max count %u, arguments %p (title %p) + 0x%llx, count %p (title %p) + 0x%llx",
            signature, argument_type, stride, max_count, static_cast<void*>(arguments), args_memory, offset,
            static_cast<void*>(count_resource), count_memory, count_offset);
    }
    if (!recording(self)) return indirect_skip(type, skip_not_recording);
    if (!known) return indirect_skip(type, skip_unknown_signature);
    if (!max_count) return indirect_skip(type, skip_zero_count);
    indirect_skip(type, skip_not_implemented);
}

// Every 300 frames (queue_present): one line with the calls by type, the executed calls and the
// skipped calls by reason (each reason with its non-zero types). Counters restart after each line.
void report_indirect(LONG frames)
{
    IndirectCounters c{};
    for (int t = 0; t < indirect_types; ++t) {
        c.calls[t] = InterlockedExchange(&indirect_counts.calls[t], 0);
        c.executed[t] = InterlockedExchange(&indirect_counts.executed[t], 0);
        for (int r = 0; r < skip_reasons; ++r) c.skipped[r][t] = InterlockedExchange(&indirect_counts.skipped[r][t], 0);
    }
    c.with_count_buffer = InterlockedExchange(&indirect_counts.with_count_buffer, 0);
    c.max_count_above_one = InterlockedExchange(&indirect_counts.max_count_above_one, 0);
    c.max_count_max = InterlockedExchange(&indirect_counts.max_count_max, 0);
    LONG calls = 0, executed = 0;
    for (int t = 0; t < indirect_types; ++t) calls += c.calls[t], executed += c.executed[t];
    char skipped[768] = "";
    int at = 0;
    for (int r = 0; r < skip_reasons; ++r) {
        LONG total = 0;
        for (int t = 0; t < indirect_types; ++t) total += c.skipped[r][t];
        if (!total || at >= static_cast<int>(sizeof(skipped)) - 96) continue;
        at += std::snprintf(skipped + at, sizeof(skipped) - at, "%s%s %ld [", at ? ", " : "", indirect_skip_names[r], total);
        bool first = true;
        for (int t = 0; t < indirect_types; ++t)
            if (c.skipped[r][t]) {
                at += std::snprintf(skipped + at, sizeof(skipped) - at, "%s%s %ld", first ? "" : " ", indirect_type_names[t], c.skipped[r][t]);
                first = false;
            }
        at += std::snprintf(skipped + at, sizeof(skipped) - at, "]");
    }
    log("x2w-indirect %ld frames: calls %ld by type (draw %ld, indexed %ld, dispatch %ld, mesh %ld, other %ld); executed %ld "
        "(draw %ld, indexed %ld, dispatch %ld, mesh %ld, other %ld); skipped by reason (%s); count buffer %ld, max count > 1 %ld, largest %ld",
        frames, calls, c.calls[0], c.calls[1], c.calls[2], c.calls[3], c.calls[4], executed, c.executed[0], c.executed[1], c.executed[2],
        c.executed[3], c.executed[4], at ? skipped : "none", c.with_count_buffer, c.max_count_above_one, c.max_count_max);
}
