// Local experiment (never upstream): translated MESH pipelines issued through ExecuteIndirect (agent meshrt).
// Included inside d3d12_xs_device.cpp's anonymous namespace, after d3d12_indirect.inl.
//
// Off unless X2W_MESH=1. When on, indirect_mesh_hook is set and every DispatchMesh ExecuteIndirect call goes:
//   indirect_run -> mesh_prepare (this file, before the record is staged; no draw_lock):
//       the bound pipeline's mesh key (computed at slot-57 creation by mesh_note57; slot 55: its base's key),
//       the translation <key>.txt / .ms.cso / [.ps.cso] (loaded once per key, off draw_lock),
//       the desktop PSO (ID3D12Device2::CreatePipelineState stream, once per key and target/state set, off draw_lock);
//     a call without all three is skipped BEFORE indirect stages its copy and guard pass;
//   -> indirect_stage (copy + guard) -> mesh_hook -> draw_translated (d3d12_draw.inl) with the mesh translation:
//       the same binding code as a translated draw (entry constants, srv / uav / sampler tables, heap, default
//       viewport and scissor), pipeline_for returns the pre-built mesh PSO, and the issue point
//       (indirect_pending -> indirect_draw_issue(self, list, false)) records the ExecuteIndirect.
//
// Key rule (board/msgs/195600_meshrt_mesh-key-rule.md, mesh's 183500 proposal): a slot-57 record without vertex
// code at [9]/[10] and with mesh code at [13]/[14] has the key pipeline_key(a = [5]/[6] bytes or empty, b = [13]/[14]
// bytes, root blob). Slot-55 pipelines take their slot-57 base's key.
//
// Cache files (mesh_work/RUNTIME_NEEDS.md): <key>.txt (draw manifest + `stage mesh` + `ms <threads> <verts> <prims>`,
// published last), <key>.ms.cso (DXIL ms_6_5, required), <key>.ps.cso (optional: none = depth-only pipeline).
// Requests req_<key>.{b,root,a} are written by regblob at creation (not here).
//
// Per 300 frames: "x2w-mesh N frames: calls X issued Y; skipped: ..." (report_mesh), then the bound keys with their
// call counts (report_mesh_keys, top 64). Per key: "x2w-mesh key K first bound frame F (cache: ok|fail|missing)"
// (first 4096 keys), "no translation" / "translation failed (...)" once, "call refused (no targets | rtv variant |
// no PSO | bind refused ...)" once per reason, "first call issued" once. Manifest `fma exact|precise` (RUNTIME_NEEDS 8):
// exact or unstated needs DoublePrecisionFloatShaderOps; a cache mixing rules is logged once, not refused.
// Fixed-function state: the default state (opaque blend; depth-stencil only from a stream pipeline, else off; cull
// none). The decoded mesh state (+0x54 from the vertex-record offsets) replaces mesh_state_desc's body.

bool mesh_enabled()
{
    static const bool on = x2w_local::env_on("X2W_MESH");
    return on;
}

enum MeshSkip { mesh_no_translation, mesh_failed, mesh_no_pso, mesh_no_targets, mesh_tier, mesh_no_record, mesh_rtv_variant, mesh_bind, mesh_compiling, mesh_skips };
LONG mesh_calls, mesh_issued, mesh_skipped[mesh_skips];
thread_local int mesh_last_skip = -1;  // LOCAL meshrt: this thread's last skip reason (traced-frame mesh lines)
void mesh_skip(MeshSkip why) { InterlockedIncrement(&mesh_skipped[why]); mesh_last_skip = why; }
constexpr const char* mesh_skip_names[mesh_skips] = {"no translation", "translation failed", "no PSO", "no targets", "tier",
                                                     "no mesh record", "rtv variant", "bind refused", "PSO compiling"};

// ---- Keys, at slot-57 creation --------------------------------------------------------------------------------

struct MeshRecordKey {
    u64 key = 0;
    bool keyed = false;  // false: a mesh record without a key (no root blob, unreadable pixel code)
};
SRWLOCK mesh_key_lock = SRWLOCK_INIT;
std::unordered_map<const Wrapper*, MeshRecordKey>& mesh_record_keys() { static std::unordered_map<const Wrapper*, MeshRecordKey> m; return m; }
LONG mesh_records_keyed, mesh_records_unkeyed;

// Device slot 57, after note_pipeline (X2W_MESH=1 only). The code blobs are hashed while the title still holds them.
void mesh_note57(const void* desc, const Wrapper* self)
{
    if (!mesh_enabled() || !desc || !self || readable_window(reinterpret_cast<u64>(desc), 0x80) < 0x80) return;
    u64 w[16];
    std::memcpy(w, desc, sizeof(w));
    if (w[9] && w[10]) return;  // vertex code: the draw key (translation_for)
    const auto* b = reinterpret_cast<const void*>(w[13]);
    const std::size_t b_bytes = static_cast<std::size_t>(w[14]);
    if (!b || !b_bytes || b_bytes > (1u << 20) || !x2w_local::readable(b, b_bytes)) return;
    MeshRecordKey k;
    std::vector<unsigned char> root_blob;
    if (Wrapper* root = as_wrapper(reinterpret_cast<const void*>(w[0]))) {
        AcquireSRWLockShared(&note_lock);
        const auto found = root_blobs().find(root);
        if (found != root_blobs().end()) root_blob = found->second;
        ReleaseSRWLockShared(&note_lock);
    }
    const auto* a = reinterpret_cast<const void*>(w[5]);
    std::size_t a_bytes = static_cast<std::size_t>(w[6]);
    bool a_ok = true;
    if (!a || !a_bytes) a = nullptr, a_bytes = 0;  // depth-only: empty a
    else a_ok = a_bytes <= (1u << 20) && x2w_local::readable(a, a_bytes);
    if (!root_blob.empty() && a_ok) {
        k.key = pipeline_key(a, a_bytes, b, b_bytes, root_blob);
        k.keyed = true;
    }
    InterlockedIncrement(k.keyed ? &mesh_records_keyed : &mesh_records_unkeyed);
    static LONG shown;
    if (first_calls(shown, 4))
        log("x2w-mesh slot 57 pipeline %p: mesh record, pixel code %zu bytes, mesh code %zu bytes, root %zu bytes -> %s %016llx",
            static_cast<const void*>(self), a_bytes, b_bytes, root_blob.size(), k.keyed ? "key" : "NO KEY", k.key);
    AcquireSRWLockExclusive(&mesh_key_lock);
    mesh_record_keys()[self] = k;
    ReleaseSRWLockExclusive(&mesh_key_lock);
}

// The mesh key of a bound pipeline: its own slot-57 record, else the base chain of slot-55 pipelines (depth 8).
bool mesh_key_of(const Wrapper* pipeline, MeshRecordKey* out)
{
    for (int depth = 0; pipeline && depth < 8; ++depth) {
        bool found = false;
        AcquireSRWLockShared(&mesh_key_lock);
        const auto it = mesh_record_keys().find(pipeline);
        if (it != mesh_record_keys().end()) *out = it->second, found = true;
        ReleaseSRWLockShared(&mesh_key_lock);
        if (found) return true;
        const PipelineInfo* info = pipeline_info(pipeline);
        pipeline = info && info->path == 55 ? info->base : nullptr;
    }
    return false;
}

// ---- Translations, per key (off draw_lock) ------------------------------------------------------------------

// The multiply-add rule a mesh translation was built with (mesh_work/RUNTIME_NEEDS.md 8): manifest `fma exact` (fused
// console ops as binary64 fma: needs DoublePrecisionFloatShaderOps) or `fma precise` (float only). No line: unstated,
// treated as exact (the translator's default). A cache that mixes rules is logged once, never refused.
enum MeshFma { mesh_fma_exact, mesh_fma_precise, mesh_fma_unstated, mesh_fma_rules };
LONG mesh_fma_loaded[mesh_fma_rules];
const char* const mesh_fma_names[mesh_fma_rules] = {"exact", "precise", "unstated"};

bool mesh_double_ops()
{
    static const bool ok = [] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const HRESULT hr = device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        const bool doubles = SUCCEEDED(hr) && options.DoublePrecisionFloatShaderOps;
        log("x2w-mesh DoublePrecisionFloatShaderOps %d (0x%08lx): `fma exact` translations %s", doubles ? 1 : 0, static_cast<unsigned long>(hr),
            doubles ? "allowed" : "refused");
        return doubles;
    }();
    return ok;
}

// load_translation's stage hook (d3d12_draw.inl): 0 not a mesh manifest, 1 `stage mesh` with .ms.cso (and the
// optional .ps.cso) loaded, -1 a mesh manifest whose stage files cannot be read or whose fma rule cannot run here.
int load_mesh_stages(Translation& tr, const std::string& manifest)
{
    bool mesh = false;
    unsigned threads = 0, vertices = 0, primitives = 0;
    int fma = mesh_fma_unstated;
    std::string fma_text;
    for (std::size_t at = 0; at < manifest.size();) {
        std::size_t end = manifest.find('\n', at);
        if (end == std::string::npos) end = manifest.size();
        std::string line = manifest.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = end + 1;
        if (line == "stage mesh") mesh = true;
        else if (line.compare(0, 4, "fma ") == 0) fma_text = line.substr(4), fma = fma_text == "exact" ? mesh_fma_exact : fma_text == "precise" ? mesh_fma_precise : -1;
        else std::sscanf(line.c_str(), "ms %u %u %u", &threads, &vertices, &primitives);
    }
    if (!mesh) return 0;
    if (fma < 0) {
        log("x2w-mesh pipeline %016llx: translation failed (unknown fma rule \"%s\")", tr.key, fma_text.c_str());
        return -1;
    }
    if (fma != mesh_fma_precise && !mesh_double_ops()) {
        log("x2w-mesh pipeline %016llx: translation failed (fma %s needs DoublePrecisionFloatShaderOps)", tr.key, mesh_fma_names[fma]);
        return -1;
    }
    wchar_t path[MAX_PATH];
    if (!cache_path(path, L"", tr.key, L".ms.cso") || FAILED(D3DReadFileToBlob(path, &tr.ms))) {
        log("x2w-mesh pipeline %016llx: translation failed (no readable .ms.cso)", tr.key);
        return -1;
    }
    if (cache_path(path, L"", tr.key, L".ps.cso") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && FAILED(D3DReadFileToBlob(path, &tr.ps))) {
        log("x2w-mesh pipeline %016llx: translation failed (.ps.cso present but unreadable)", tr.key);
        return -1;
    }
    log("x2w-mesh pipeline %016llx: mesh stage %zu bytes (ms %u threads, %u vertices, %u primitives), pixel stage %s, fma %s", tr.key,
        tr.ms->GetBufferSize(), threads, vertices, primitives, tr.ps ? "yes" : "none (depth-only)", mesh_fma_names[fma]);
    InterlockedIncrement(&mesh_fma_loaded[fma]);
    int rules = 0;
    for (LONG& n : mesh_fma_loaded) rules += InterlockedCompareExchange(&n, 0, 0) != 0;
    static LONG mixed;
    if (rules > 1 && InterlockedExchange(&mixed, 1) == 0)
        log("x2w-mesh shader cache mixes fma rules (from %016llx): exact %ld, precise %ld, unstated %ld loaded so far; positions may differ "
            "between pipelines (not refused)", tr.key, mesh_fma_loaded[mesh_fma_exact], mesh_fma_loaded[mesh_fma_precise], mesh_fma_loaded[mesh_fma_unstated]);
    return 1;
}

struct MeshPso {
    int state = 0;  // 0 new, 3 compiling, 1 ready, 2 failed (never retried)
    ID3D12PipelineState* pso = nullptr;
};
struct MeshEntry {
    volatile int state = 0;  // 0 new, 3 loading, 1 ready, 2 failed (never retried), 4 no manifest (checked again after 5 s)
    volatile ULONGLONG retry_at = 0;
    Translation* tr = nullptr;
    std::unordered_map<u64, MeshPso> psos;  // by target formats, DSV format and fixed-function state
    bool issued_once = false;
    unsigned refusals_logged = 0;  // bit per MeshSkip: its "call refused" line was written (mesh_lock)
    volatile LONG uses = 0;        // keyed calls since the last report (report_mesh_keys), counted without a lock
    volatile LONG seen = 0;        // 1 once the "first bound" line was written
};
SRWLOCK mesh_lock = SRWLOCK_INIT;
CONDITION_VARIABLE mesh_loaded = CONDITION_VARIABLE_INIT;
std::unordered_map<u64, MeshEntry>& mesh_entries() { static std::unordered_map<u64, MeshEntry> m; return m; }  // node-based: references stay valid

// The entry of `key` in its final state for now (1, 2 or 4). One thread loads a key; others wait for that key only.
MeshEntry& loaded_mesh(u64 key)
{
    AcquireSRWLockExclusive(&mesh_lock);
    MeshEntry& e = mesh_entries()[key];
    while (e.state == 3) SleepConditionVariableSRW(&mesh_loaded, &mesh_lock, INFINITE, 0);
    if (e.state == 1 || e.state == 2 || (e.state == 4 && GetTickCount64() < e.retry_at)) {
        ReleaseSRWLockExclusive(&mesh_lock);
        return e;
    }
    const bool first = e.state == 0;
    e.state = 3;
    ReleaseSRWLockExclusive(&mesh_lock);
    int state = 4;
    Translation* tr = nullptr;
    wchar_t path[MAX_PATH];
    if (cache_path(path, L"", key, L".txt") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        tr = new Translation;
        tr->key = key;
        load_translation(*tr);
        if (tr->state == 1 && tr->ms) {
            state = 1;
        } else {
            log("x2w-mesh pipeline %016llx: translation failed (%s)", key,
                tr->state == 1 ? "manifest has no `stage mesh`" : "manifest, stage files or root signature rejected");
            state = 2;
        }
    } else {
        static LONG shown;
        if (first && first_calls(shown, 1024)) log("x2w-mesh pipeline %016llx: no translation", key);
    }
    AcquireSRWLockExclusive(&mesh_lock);
    // tr and retry_at before state: mesh_prepare reads a kept entry's state without mesh_lock.
    e.tr = state == 1 ? tr : nullptr;
    e.retry_at = state == 4 ? GetTickCount64() + 5000 : 0;
    MemoryBarrier();
    e.state = state;
    ReleaseSRWLockExclusive(&mesh_lock);
    WakeAllConditionVariable(&mesh_loaded);
    return e;  // a failed Translation is kept (leaked) like draw translations: never retried
}

// Once per key and reason (first 4096 lines PER REASON, so keys refused for one reason cannot use up the lines of
// another): "x2w-mesh pipeline K: call refused (<why>)", for the refusals that have no per-key line of their own
// (no targets, rtv variant, no PSO, translation taken, bind refused). "no translation" and "translation failed (...)"
// are written once per key by loaded_mesh / load_mesh_stages.
// A refused call of a key whose line is out pays no lock (a racy read of the bit: at worst the lock is taken once more).
bool mesh_refusal_logged(const MeshEntry& e, MeshSkip why) { return (e.refusals_logged & (1u << why)) != 0; }
void mesh_refusal_line(MeshEntry& e, u64 key, MeshSkip why, const char* detail)
{
    if (mesh_refusal_logged(e, why)) return;
    AcquireSRWLockExclusive(&mesh_lock);
    const bool first = (e.refusals_logged & (1u << why)) == 0;
    e.refusals_logged |= 1u << why;
    ReleaseSRWLockExclusive(&mesh_lock);
    static LONG shown[mesh_skips];
    if (first && first_calls(shown[why], 4096)) log("x2w-mesh pipeline %016llx: call refused (%s)", key, detail);
}

// ---- Coverage: which mesh keys are bound (board/msgs/210900_mesh) ----------------------------------------------

// Every mesh call with a keyed record, after its translation lookup (state: 1 ok, 2 fail, 4 missing). Counted in the
// key's entry without a lock (the entry map is node-based; report_mesh_keys collects the counts).
void mesh_note_use(MeshEntry& e, u64 key)
{
    InterlockedIncrement(&e.uses);
    if (e.seen || InterlockedExchange(&e.seen, 1)) return;
    static LONG shown;
    if (first_calls(shown, 4096))
        log("x2w-mesh key %016llx first bound frame %ld (cache: %s)", key, frame_number, e.state == 1 ? "ok" : e.state == 2 ? "fail" : "missing");
}

// "x2w-mesh N frames: keys n distinct, calls c; top: key:calls ..." (up to 64 keys, 32 per line; log lines are 1536 bytes).
void report_mesh_keys(LONG frames)
{
    std::vector<std::pair<u64, LONG>> uses;
    std::size_t seen = 0;
    AcquireSRWLockShared(&mesh_lock);
    for (auto& [key, e] : mesh_entries()) {
        if (const LONG n = InterlockedExchange(&e.uses, 0)) uses.emplace_back(key, n);
        if (e.seen) ++seen;
    }
    ReleaseSRWLockShared(&mesh_lock);
    LONG calls = 0;
    for (const auto& u : uses) calls += u.second;
    std::sort(uses.begin(), uses.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
    const std::size_t top = uses.size() < 64 ? uses.size() : 64;
    if (top <= 32) {
        char line[1100];
        int n = 0;
        for (std::size_t i = 0; i < top; ++i) n += std::snprintf(line + n, sizeof(line) - n, " %016llx:%ld", uses[i].first, uses[i].second);
        log("x2w-mesh %ld frames: keys %zu distinct, calls %ld; top:%s (keys seen since start %zu)", frames, uses.size(), calls, top ? line : " none", seen);
        return;
    }
    log("x2w-mesh %ld frames: keys %zu distinct, calls %ld; top: 64 keys on the next 2 lines (keys seen since start %zu)", frames, uses.size(), calls, seen);
    for (std::size_t from = 0; from < top; from += 32) {
        const std::size_t to = top < from + 32 ? top : from + 32;
        char line[1100];
        int n = 0;
        for (std::size_t i = from; i < to; ++i) n += std::snprintf(line + n, sizeof(line) - n, " %016llx:%ld", uses[i].first, uses[i].second);
        log("x2w-mesh %ld frames top %zu-%zu:%s", frames, from + 1, to, line);
    }
}

// ---- Desktop pipeline ---------------------------------------------------------------------------------------

// The fixed-function state of the mesh PSO: the default state (lead, 2026-11-24). Opaque blend, all channels
// written: translation_for's blend decode (bound.blend) reads slot-57 state at the vertex-record offsets, and mesh
// records keep their state 0x54 bytes later, so it is NOT used here. Depth-stencil only from a stream pipeline
// (never a slot-57 record), else off; cull none. The key covers all of it.
// LOCAL meshrt: X2W_MESH_STATE=1 puts the pipeline's own decoded state over that default: depth-stencil, rasterizer
// and masked blend from state block A in the mesh layout (renderstate's decode_draw_state / apply_draw_state,
// pipeline_state_decode.h) or the nearest slot 55 part. Unset (default): the default state as before, same PSO key.
// The state is per title pipeline (BoundPipeline), so two pipelines of one key with different state get two PSOs.
// *fallback (when not null) receives the default desc if the decoded state changed it, for a failed creation.
// LOCAL meshrt (X2W_MESH_DUMP_INPUTS, declared in d3d12_draw.inl): the first record's X, Y, Z in title memory, as
// the traced-frame mesh line reads it (d3d12_indirect.inl sets mesh_trace_record for this call).
bool mesh_dump_record_groups(std::uint32_t (&xyz)[3])
{
    if (!mesh_trace_record || mesh_trace_stride < 12 || !x2w_local::readable(mesh_trace_record, 12)) return false;
    std::memcpy(xyz, mesh_trace_record, 12);
    return true;
}
bool mesh_cull_flip()
{
    static const bool flip = x2w_local::env_on("X2W_MESH_STATE_CULL_FLIP");
    return flip;
}
const x2w_pipeline_state::Switches& mesh_switches()
{
    static const x2w_pipeline_state::Switches s = [] {
        x2w_pipeline_state::Switches v;
        if (x2w_local::env_on("X2W_MESH_STATE")) v.depth = v.cull = v.blend = true;
        // Without a rebuild: X2W_MESH_STATE_NOCULL=1 keeps the default rasterizer (cull none);
        // X2W_MESH_STATE_CULL_FLIP=1 inverts the decoded front face (apply_draw_state's flip covers slot-55 patches only).
        if (v.cull && x2w_local::env_on("X2W_MESH_STATE_NOCULL")) v.cull = false;
        // X2W_DRAW_STATE_DEPTHCLIP=1: the decoded depth clip alone (also without X2W_MESH_STATE); cull includes it.
        v.depthclip = draw_state::switches().depthclip;  // parsed with the others: X2W_DRAW_STATE=0 turns it off too
        if (v.depthclip) log("x2w-mesh X2W_DRAW_STATE_DEPTHCLIP=1: mesh PSOs take depth clip from the pipeline's rasterizer state");
        if (v.depth)
            log("x2w-mesh X2W_MESH_STATE=1: mesh PSOs take depth-stencil, %sblend from the pipeline's state block%s", v.cull ? "rasterizer and " : "",
                !v.cull ? " (X2W_MESH_STATE_NOCULL: rasterizer default)" : mesh_cull_flip() ? " (X2W_MESH_STATE_CULL_FLIP: front face inverted)" : "");
        return v;
    }();
    return s;
}
LONG mesh_state_changed, mesh_state_fallback;  // PSOs whose decoded state differs from the default; created with the default after a failure
void mesh_state_desc(const BoundPipeline& bound, const Pass& pass, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, u64& key,
                     bool* decoded = nullptr, D3D12_GRAPHICS_PIPELINE_STATE_DESC* fallback = nullptr)
{
    const DXGI_FORMAT dsv_format = pass.dsv_format != DXGI_FORMAT_UNKNOWN ? pass.dsv_format : bound.dsv_format;
    desc = {};
    for (auto& target : desc.BlendState.RenderTarget)
        target = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
                  D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    static const bool default_blend = x2w_local::env_on("X2W_DRAW_DEFAULT_BLEND");
    if (default_blend)
        for (auto& target : desc.BlendState.RenderTarget)
            target = {TRUE, FALSE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                      D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    if (bound.has_depth_stencil) desc.DepthStencilState = bound.depth_stencil;
    desc.SampleMask = ~0u;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.NumRenderTargets = pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) desc.RTVFormats[i] = pass.target_formats[i];
    desc.DSVFormat = dsv_format;
    desc.SampleDesc.Count = 1;
    const auto& s = mesh_switches();
    if (s.depth || s.cull || s.blend || s.depthclip) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC before = desc;
        const bool changed = apply_draw_state(bound, pass, desc, s);
        if (s.cull && mesh_cull_flip() && std::memcmp(&before.RasterizerState, &desc.RasterizerState, sizeof(desc.RasterizerState)) != 0)
            desc.RasterizerState.FrontCounterClockwise = !desc.RasterizerState.FrontCounterClockwise;
        if (decoded) *decoded = changed;
        if (changed && fallback) *fallback = before;
    }
    key = pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) key = key * 131 + static_cast<u64>(pass.target_formats[i]);
    key = fnv_bytes(&dsv_format, sizeof(dsv_format), key);
    // Field by field: D3D12_DEPTH_STENCIL_DESC and D3D12_RENDER_TARGET_BLEND_DESC have padding after their UINT8 masks,
    // and a struct copied from apply_draw_state's locals carries whatever was there, so hashing the bytes gave a new key
    // for the same state (run 102: two PSO lines with the same state for 5ce5567f1f9b3707 and 7f8e1488ac1e336a).
    const auto mix = [&key](u64 v) { key = fnv_bytes(&v, sizeof(v), key); };
    const auto& b = desc.BlendState;
    mix(b.AlphaToCoverageEnable), mix(b.IndependentBlendEnable);
    for (const auto& t : b.RenderTarget) {
        mix(t.BlendEnable), mix(t.LogicOpEnable), mix(t.SrcBlend), mix(t.DestBlend), mix(t.BlendOp), mix(t.SrcBlendAlpha);
        mix(t.DestBlendAlpha), mix(t.BlendOpAlpha), mix(t.LogicOp), mix(t.RenderTargetWriteMask);
    }
    const auto& d = desc.DepthStencilState;
    mix(d.DepthEnable), mix(d.DepthWriteMask), mix(d.DepthFunc), mix(d.StencilEnable), mix(d.StencilReadMask), mix(d.StencilWriteMask);
    for (const auto* f : {&d.FrontFace, &d.BackFace})
        mix(f->StencilFailOp), mix(f->StencilDepthFailOp), mix(f->StencilPassOp), mix(f->StencilFunc);
    const auto& r = desc.RasterizerState;
    mix(r.FillMode), mix(r.CullMode), mix(r.FrontCounterClockwise), mix(static_cast<u64>(static_cast<std::int64_t>(r.DepthBias)));
    std::uint32_t bits;
    std::memcpy(&bits, &r.DepthBiasClamp, 4), mix(bits);
    std::memcpy(&bits, &r.SlopeScaledDepthBias, 4), mix(bits);
    mix(r.DepthClipEnable), mix(r.MultisampleEnable), mix(r.AntialiasedLineEnable), mix(r.ForcedSampleCount), mix(r.ConservativeRaster);
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class Value>
struct alignas(void*) MeshSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    Value value{};
};
struct MeshStream {
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv_format;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv_formats;
    MeshSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
};

ID3D12PipelineState* create_mesh_pso(const Translation& tr, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, HRESULT* result)
{
    static ID3D12Device2* device2 = [] {
        ID3D12Device2* d2 = nullptr;
        device_of(the_device)->QueryInterface(IID_PPV_ARGS(&d2));
        return d2;
    }();
    *result = E_NOINTERFACE;
    if (!device2) return nullptr;
    MeshStream s;
    s.root.value = tr.root;
    s.ms.value = {tr.ms->GetBufferPointer(), tr.ms->GetBufferSize()};
    if (tr.ps) s.ps.value = {tr.ps->GetBufferPointer(), tr.ps->GetBufferSize()};
    s.blend.value = d.BlendState;
    s.sample_mask.value = d.SampleMask;
    s.rasterizer.value = d.RasterizerState;
    s.depth_stencil.value = d.DepthStencilState;
    s.dsv_format.value = d.DSVFormat;
    s.rtv_formats.value.NumRenderTargets = d.NumRenderTargets;
    for (UINT i = 0; i < 8; ++i) s.rtv_formats.value.RTFormats[i] = d.RTVFormats[i];
    s.sample.value = d.SampleDesc;
    const D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(s), &s};
    ID3D12PipelineState* pso = nullptr;
    *result = device2->CreatePipelineState(&stream, IID_PPV_ARGS(&pso));
    return SUCCEEDED(*result) ? pso : nullptr;
}

// Compiles the slot's PSO (state 3 on entry) and publishes it: state 1 (ready) or 2 (failed, kept). The line gives the
// driver compile time (run 102: compute compiles of up to 43 s in the frame path stalled the frame).
void mesh_pso_compile(const Translation& tr, MeshPso& slot, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                      const D3D12_GRAPHICS_PIPELINE_STATE_DESC& fallback, bool decoded, UINT target_count, DXGI_FORMAT format0, bool async)
{
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    HRESULT hr = E_FAIL;
    ID3D12PipelineState* pso = create_mesh_pso(tr, desc, &hr);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const auto& d = desc.DepthStencilState;
    const auto& ms = mesh_switches();
    log("x2w-mesh desktop pipeline for %016llx: %u targets (format %d), dsv %d, depth %d, pixel stage %s -> 0x%08lx; state %s: depth func %d "
        "write %d stencil %d, cull %d ccw %d depthclip %d, blend rt0 %d mask 0x%x; compiled in %lld ms%s", tr.key, target_count, static_cast<int>(format0),
        static_cast<int>(desc.DSVFormat), d.DepthEnable ? 1 : 0, tr.ps ? "yes" : "none", static_cast<unsigned long>(hr),
        !decoded ? "default" : (ms.depth || ms.cull || ms.blend) ? "decoded (X2W_MESH_STATE)" : "default + depth clip decoded (X2W_DRAW_STATE_DEPTHCLIP)",
        d.DepthFunc, d.DepthWriteMask, d.StencilEnable, desc.RasterizerState.CullMode, desc.RasterizerState.FrontCounterClockwise,
        desc.RasterizerState.DepthClipEnable, desc.BlendState.RenderTarget[0].BlendEnable, desc.BlendState.RenderTarget[0].RenderTargetWriteMask,
        static_cast<long long>((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart), async ? " (X2W_MESH_ASYNC worker)" : "");
    if (decoded) InterlockedIncrement(&mesh_state_changed);
    if (!pso && decoded) {
        // The decoded state was refused by the runtime: the default state instead (NOT exercised by a test).
        pso = create_mesh_pso(tr, fallback, &hr);
        InterlockedIncrement(&mesh_state_fallback);
        log("x2w-mesh desktop pipeline for %016llx: decoded state refused, default state -> 0x%08lx", tr.key, static_cast<unsigned long>(hr));
    }
    check_device("translated mesh CreatePipelineState", 0);
    AcquireSRWLockExclusive(&mesh_lock);
    slot.pso = pso;
    slot.state = pso ? 1 : 2;
    ReleaseSRWLockExclusive(&mesh_lock);
    WakeAllConditionVariable(&mesh_loaded);
}

// X2W_MESH_ASYNC=1: a call whose PSO is not compiled yet hands the compile to a thread-pool worker and is skipped
// ("compiling"); the recording thread never compiles and never waits for a compile. Default off: compile in the call.
bool mesh_async()
{
    static const bool on = [] {
        // Also on with stall's X2W_CX_ASYNC=1 (compute and draws), unless X2W_MESH_ASYNC_OFF=1.
        const bool v = x2w_local::env_on("X2W_MESH_ASYNC") || (x2w_local::env_on("X2W_CX_ASYNC") && !x2w_local::env_on("X2W_MESH_ASYNC_OFF"));
        if (v) log("x2w-mesh X2W_MESH_ASYNC=1: mesh PSOs compile on a worker; calls are skipped until their PSO is ready");
        return v;
    }();
    return on;
}
struct MeshPsoJob {
    const Translation* tr;
    MeshPso* slot;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, fallback;
    bool decoded;
    UINT target_count;
    DXGI_FORMAT format0;
};
DWORD WINAPI mesh_pso_worker(void* p)
{
    MeshPsoJob* job = static_cast<MeshPsoJob*>(p);
    mesh_pso_compile(*job->tr, *job->slot, job->desc, job->fallback, job->decoded, job->target_count, job->format0, true);
    delete job;
    return 0;
}

// The PSO for (entry, translation, state of this call), compiled once per key without draw_lock (failures are kept).
// tr is the entry's translation or its RTV variant (translation_for_targets). With X2W_MESH_ASYNC, null + *compiling
// while a worker compiles it.
ID3D12PipelineState* mesh_pso(MeshEntry& e, const Translation& tr, const BoundPipeline& bound, const Pass& pass, bool* compiling = nullptr)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, fallback;
    u64 key = 0;
    bool decoded = false;
    mesh_state_desc(bound, pass, desc, key, &decoded, &fallback);
    key = fnv_bytes(&tr.key, sizeof(tr.key), key);  // a variant's PSOs never share a slot with the base's
    const bool async = mesh_async();
    AcquireSRWLockExclusive(&mesh_lock);
    MeshPso& slot = e.psos[key];
    if (async && slot.state == 3) {
        ReleaseSRWLockExclusive(&mesh_lock);
        if (compiling) *compiling = true;
        return nullptr;
    }
    while (slot.state == 3) SleepConditionVariableSRW(&mesh_loaded, &mesh_lock, INFINITE, 0);
    if (slot.state == 1 || slot.state == 2) {
        ID3D12PipelineState* pso = slot.pso;
        ReleaseSRWLockExclusive(&mesh_lock);
        return pso;
    }
    slot.state = 3;
    ReleaseSRWLockExclusive(&mesh_lock);
    const DXGI_FORMAT format0 = pass.target_count ? pass.target_formats[0] : DXGI_FORMAT_UNKNOWN;
    if (async) {
        auto* job = new MeshPsoJob{&tr, &slot, desc, fallback, decoded, pass.target_count, format0};
        if (QueueUserWorkItem(mesh_pso_worker, job, WT_EXECUTELONGFUNCTION)) {
            if (compiling) *compiling = true;
            return nullptr;
        }
        delete job;  // no worker: compile here
    }
    mesh_pso_compile(tr, slot, desc, fallback, decoded, pass.target_count, format0, false);
    AcquireSRWLockShared(&mesh_lock);
    ID3D12PipelineState* pso = slot.pso;
    ReleaseSRWLockShared(&mesh_lock);
    return pso;
}

// ---- Per call --------------------------------------------------------------------------------------------------

// Bound title pipeline -> its BoundPipeline (translation_for) with the mesh translation attached.
struct MeshBinding {
    MeshRecordKey key;
    bool mesh = false;
    BoundPipeline* bound = nullptr;
    Translation* attached = nullptr;
    MeshEntry* entry = nullptr;               // the key's entry once it is final (ready or failed): no loaded_mesh per call
    u64 pso_signature = 0;                    // mesh_pass_signature of the pass pso was made for
    ID3D12PipelineState* pso = nullptr;       // the last PSO of this pipeline: no state desc, hash or mesh_lock per call
    Translation* pso_tr = nullptr;            // the translation pso was made from (the entry's, or its RTV variant)
    u64 variant_wait_signature = 0;           // RTV variant not ready for this pass signature: no draw_lock lookup ...
    ULONGLONG variant_wait_until = 0;         // ... until this tick (1 s)
};

// Everything of the pass that mesh_state_desc / apply_draw_state read: targets, DSV format, read-only DSV, rect
// topology. Equal signatures give the same PSO key for the same title pipeline (its BoundPipeline state is fixed).
u64 mesh_pass_signature(const Pass& pass)
{
    u64 s = 0x9e3779b97f4a7c15ull ^ pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) s = s * 1099511628211ull + static_cast<u64>(pass.target_formats[i]);
    s = s * 1099511628211ull + static_cast<u64>(pass.dsv_format);
    s = s * 1099511628211ull + (pass.dsv_read_only ? 1u : 0u) + (pass.topology == 0x11 ? 2u : 0u);
    // renderstate 7c0d4cae: apply_draw_state keeps depth / stencil off on a depth target no forwarded slot 51 clear
    // has initialised, so whether the bound target is cleared is part of the PSO (one shared-lock lookup, X2W_MESH_STATE only).
    if (mesh_switches().depth && pass.dsv_resource && depth_target_cleared(pass.dsv_resource)) s = s * 1099511628211ull + 4u;
    return s | 1;  // never 0 (0 = no PSO cached)
}

// LOCAL meshrt: stand-in CPU per mesh call (report_mesh): mesh_prepare and the bind through draw_translated (which
// includes its draw_lock wait), QueryPerformanceCounter ticks.
volatile LONG64 mesh_prepare_ticks, mesh_bind_ticks, mesh_bind_calls;
LONG64 mesh_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
std::unordered_map<const Wrapper*, MeshBinding>& mesh_bindings() { static std::unordered_map<const Wrapper*, MeshBinding> m; return m; }

bool mesh_tier_ok()
{
    static const bool ok = [] {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        const HRESULT hr7 = device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        const HRESULT hrm = device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
        const bool tier = SUCCEEDED(hr7) && options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        const bool sm = SUCCEEDED(hrm) && model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
        log("x2w-mesh X2W_MESH=1: MeshShaderTier %d (0x%08lx), highest shader model 0x%x (0x%08lx) -> mesh path %s",
            SUCCEEDED(hr7) ? static_cast<int>(options7.MeshShaderTier) : -1, static_cast<unsigned long>(hr7),
            SUCCEEDED(hrm) ? static_cast<unsigned>(model.HighestShaderModel) : 0u, static_cast<unsigned long>(hrm),
            tier && sm ? "ON" : "OFF (needs tier 1 and shader model 6.5)");
        return tier && sm;
    }();
    return ok;
}

// What mesh_prepare found for the call indirect is about to stage on this thread.
struct MeshReady {
    Wrapper* list = nullptr;
    Translation* tr = nullptr;
    ID3D12PipelineState* pso = nullptr;
    MeshEntry* entry = nullptr;
};
thread_local MeshReady mesh_ready;
thread_local bool mesh_binding_now;  // inside draw_translated for the mesh call

// LOCAL meshrt: in a traced frame (trace_frame_active), every mesh call takes part like an ordinary draw: its render
// targets and depth target are noted for the end-of-frame read-back (X2W_D3D12_TRACE_TARGETS snapshots) and one
// "x2w-frame mesh" line gives key, every target with size and format, depth target, the list's first viewport and
// scissor, the decoded depth state of the PSO, the indirect record's first words in title memory at record time (the
// GPU may write them later) and, when not drawn, the skip reason. Nothing of this runs outside a traced frame.
thread_local u64 mesh_trace_key;              // the binding's key for this call (0: none yet)
thread_local const BoundPipeline* mesh_trace_bound;
thread_local LONG mesh_trace_query = -1;      // this call's pipeline statistics slot, or -1

// LOCAL meshrt: pipeline statistics of the traced frame's issued mesh calls. One query per call (BeginQuery before the
// ExecuteIndirect, EndQuery + ResolveQueryData into a readback buffer after it), PIPELINE_STATISTICS1 when the device
// reports MeshShaderPipelineStatsSupported (MS invocations / primitives), else PIPELINE_STATISTICS (no MS counts). The
// results are read 8 presents after the traced frame ends (no GPU wait; mesh_stats_present), or by the fixture export.
// "x2w-frame mesh stats #slot key K: ..." per slot. Capacity 4096 calls per traced frame; later calls get no query.
struct MeshStats {
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    bool v1 = false;     // PIPELINE_STATISTICS1
    bool failed = false;
    UINT capacity = 0;
};
SRWLOCK mesh_stats_lock = SRWLOCK_INIT;
MeshStats mesh_stats;
LONG mesh_stats_next;      // slots handed out since the last report
LONG mesh_stats_due;       // frame at which to report (0: none pending)
u64 mesh_stats_keys[4096];
bool mesh_stats_ready()
{
    AcquireSRWLockShared(&mesh_stats_lock);
    const bool ready = mesh_stats.heap != nullptr, failed = mesh_stats.failed;
    ReleaseSRWLockShared(&mesh_stats_lock);
    if (ready || failed) return ready;
    AcquireSRWLockExclusive(&mesh_stats_lock);
    if (!mesh_stats.heap && !mesh_stats.failed) {
        ID3D12Device* device = device_of(the_device);
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
        mesh_stats.v1 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9))) &&
                        options9.MeshShaderPipelineStatsSupported;
        mesh_stats.capacity = 4096;
        const D3D12_QUERY_HEAP_DESC desc{mesh_stats.v1 ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 : D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,
                                         mesh_stats.capacity, 0};
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, u64{mesh_stats.capacity} * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1),
                                   1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const HRESULT hq = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&mesh_stats.heap));
        const HRESULT hb = SUCCEEDED(hq) ? device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                          nullptr, IID_PPV_ARGS(&mesh_stats.readback)) : hq;
        if (FAILED(hb)) {
            if (mesh_stats.heap) mesh_stats.heap->Release();
            mesh_stats.heap = nullptr;
            mesh_stats.failed = true;
        }
        log("x2w-mesh pipeline statistics for traced mesh calls: %s (MeshShaderPipelineStatsSupported %d) -> 0x%08lx / 0x%08lx",
            mesh_stats.failed ? "unavailable" : mesh_stats.v1 ? "PIPELINE_STATISTICS1" : "PIPELINE_STATISTICS (no MS counts)", mesh_stats.v1 ? 1 : 0,
            static_cast<unsigned long>(hq), static_cast<unsigned long>(hb));
    }
    const bool made = mesh_stats.heap != nullptr;
    ReleaseSRWLockExclusive(&mesh_stats_lock);
    return made;
}
D3D12_QUERY_TYPE mesh_stats_type() { return mesh_stats.v1 ? D3D12_QUERY_TYPE_PIPELINE_STATISTICS1 : D3D12_QUERY_TYPE_PIPELINE_STATISTICS; }
UINT mesh_stats_size() { return mesh_stats.v1 ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1) : sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS); }
// Prints every slot's numbers and frees the slots. The GPU work of those calls must be complete.
void mesh_stats_report()
{
    const LONG used = InterlockedExchange(&mesh_stats_next, 0);
    InterlockedExchange(&mesh_stats_due, 0);
    if (!mesh_stats.readback || used <= 0) return;
    const LONG count = used < static_cast<LONG>(mesh_stats.capacity) ? used : static_cast<LONG>(mesh_stats.capacity);
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(count) * mesh_stats_size()};
    if (FAILED(mesh_stats.readback->Map(0, &range, &mapped))) return;
    std::uint64_t sums[5] = {}, zero_ps = 0, zero_clip = 0;
    for (LONG i = 0; i < count; ++i) {
        D3D12_QUERY_DATA_PIPELINE_STATISTICS1 s{};
        std::memcpy(&s, static_cast<const unsigned char*>(mapped) + std::size_t(i) * mesh_stats_size(), mesh_stats_size());
        if (mesh_stats.v1)
            log("x2w-frame mesh stats #%ld key %016llx: MS invocations %llu primitives %llu, clipper invocations %llu primitives %llu, PS invocations %llu",
                i, static_cast<unsigned long long>(mesh_stats_keys[i]), s.MSInvocations, s.MSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations);
        else
            log("x2w-frame mesh stats #%ld key %016llx: (no MS counts) clipper invocations %llu primitives %llu, PS invocations %llu",
                i, static_cast<unsigned long long>(mesh_stats_keys[i]), s.CInvocations, s.CPrimitives, s.PSInvocations);
        const std::uint64_t v[5] = {s.MSInvocations, s.MSPrimitives, s.CInvocations, s.CPrimitives, s.PSInvocations};
        for (int k = 0; k < 5; ++k) sums[k] += v[k];
        zero_clip += s.CPrimitives == 0;
        zero_ps += s.PSInvocations == 0;
    }
    const D3D12_RANGE none{0, 0};
    mesh_stats.readback->Unmap(0, &none);
    log("x2w-frame mesh stats total over %ld calls (%ld asked): MS invocations %llu primitives %llu, clipper invocations %llu primitives %llu, "
        "PS invocations %llu; calls with 0 clipper primitives %llu, with 0 PS invocations %llu", count, used, sums[0], sums[1], sums[2], sums[3],
        sums[4], zero_clip, zero_ps);
}
// queue_present, every present: the traced frame has ended (trace_frame_active 0) -> report 8 presents later.
void mesh_stats_present(LONG frame)
{
    if (trace_frame_active || !mesh_stats_next) return;
    const LONG due = mesh_stats_due;
    if (!due) InterlockedCompareExchange(&mesh_stats_due, frame + 8, 0);
    else if (frame >= due) mesh_stats_report();
}
extern "C" __declspec(dllexport) void x2w_d3d12_mesh_stats_report() { mesh_stats_report(); }  // fixtures, after their fence

// LOCAL meshrt: X2W_MESH_STATS_FRAMES=<n> (default 0: off). Outside the traced frame, every issued mesh call gets a
// pipeline statistics query (its own query heap, 16 frame slots of 2048 calls) until n frames have been COUNTED: a frame
// counts when at least one of its mesh calls had a record with X, Y, Z > 0 in title memory at record time. 8 presents
// after a frame ends its results are read (no GPU wait); a counted frame prints one line per target set (render target
// formats / sizes + depth format / size; the resources at the end of the line) with the call count, the calls with
// groups and the summed statistics, then a frame line; a frame that does not count prints one "not counted" line (the
// first 8 only). The traced frame keeps its per-call queries (mesh_stats) and is not part of this.
struct MeshFrameSlot {
    LONG frame = -1;      // the frame id collected in this slot (-1: free)
    LONG used = 0;        // calls handed a query
    LONG groups = 0;      // calls with a record with groups (record time)
    LONG due = 0;         // present count at which to read
    bool closed = false;  // the frame has ended (due is set)
};
constexpr UINT mesh_fs_slots = 16, mesh_fs_calls = 2048;
struct MeshFrameCall {
    u64 set = 0;          // target-set hash
    bool groups = false;  // the record had X, Y, Z > 0 at record time
};
SRWLOCK mesh_fs_lock = SRWLOCK_INIT;
ID3D12QueryHeap* mesh_fs_heap;
ID3D12Resource* mesh_fs_readback;
bool mesh_fs_failed;
MeshFrameSlot mesh_fs_slot[mesh_fs_slots];
std::vector<MeshFrameCall>& mesh_fs_calls_of() { static std::vector<MeshFrameCall> v(mesh_fs_slots * mesh_fs_calls); return v; }
std::map<u64, std::string>& mesh_fs_sets() { static std::map<u64, std::string> m; return m; }  // under mesh_fs_lock
LONG mesh_fs_current;     // the frame id being collected
LONG mesh_fs_presents;    // presents seen
LONG mesh_fs_counted;     // frames counted so far
LONG mesh_fs_reported;    // counted frames reported
LONG mesh_fs_skipped_lines;
LONG mesh_fs_wanted()
{
    static const LONG n = [] {
        char v[16]{};
        const LONG k = GetEnvironmentVariableA("X2W_MESH_STATS_FRAMES", v, sizeof(v)) ? std::atol(v) : 0;
        if (k > 0) log("x2w-mesh X2W_MESH_STATS_FRAMES=%ld: mesh calls carry statistics queries until %ld frames with groups are counted", k, k);
        return k > 0 ? k : 0;
    }();
    return n;
}
bool mesh_fs_collecting() { return mesh_fs_wanted() && mesh_fs_counted < mesh_fs_wanted() && !mesh_fs_failed; }
bool mesh_fs_ready()
{
    if (!mesh_stats_ready()) return false;  // the query type (v1 or not) is chosen there
    AcquireSRWLockExclusive(&mesh_fs_lock);
    if (!mesh_fs_heap && !mesh_fs_failed) {
        ID3D12Device* device = device_of(the_device);
        const UINT count = mesh_fs_slots * mesh_fs_calls;
        const D3D12_QUERY_HEAP_DESC desc{mesh_stats.v1 ? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 : D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, count, 0};
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, u64{count} * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1), 1, 1, 1,
                                   DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const HRESULT hq = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&mesh_fs_heap));
        const HRESULT hb = SUCCEEDED(hq) ? device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                          nullptr, IID_PPV_ARGS(&mesh_fs_readback)) : hq;
        if (FAILED(hb)) {
            if (mesh_fs_heap) mesh_fs_heap->Release();
            mesh_fs_heap = nullptr;
            mesh_fs_failed = true;
        }
        mesh_fs_calls_of();
        mesh_fs_slot[0].frame = mesh_fs_current;
        log("x2w-mesh X2W_MESH_STATS_FRAMES query heap (%u queries) -> 0x%08lx / 0x%08lx", count, static_cast<unsigned long>(hq),
            static_cast<unsigned long>(hb));
    }
    const bool made = mesh_fs_heap != nullptr;
    ReleaseSRWLockExclusive(&mesh_fs_lock);
    return made;
}
// The target set of this call: hash, and its text the first time it is seen.
u64 mesh_fs_set(const Pass& pass)
{
    u64 h = fnv_bytes(&pass.target_count, sizeof(pass.target_count), 1469598103934665603ull);
    for (UINT i = 0; i < pass.target_count; ++i) h = fnv_bytes(&pass.targets[i], sizeof(pass.targets[i]), h);
    h = fnv_bytes(&pass.dsv_resource, sizeof(pass.dsv_resource), h);
    AcquireSRWLockShared(&mesh_fs_lock);
    const bool known = mesh_fs_sets().count(h) != 0;
    ReleaseSRWLockShared(&mesh_fs_lock);
    if (known) return h;
    char text[600];
    int n = std::snprintf(text, sizeof(text), "targets %u", pass.target_count);
    std::string res = "; resources";
    char part[64];
    for (UINT i = 0; i < pass.target_count && n > 0 && n < 480; ++i) {
        const D3D12_RESOURCE_DESC rd = pass.targets[i] ? pass.targets[i]->GetDesc() : D3D12_RESOURCE_DESC{};
        n += std::snprintf(text + n, sizeof(text) - n, " [fmt %d %llux%u]", static_cast<int>(pass.target_formats[i]),
                           static_cast<unsigned long long>(rd.Width), rd.Height);
        std::snprintf(part, sizeof(part), " rt%u %p", i, static_cast<void*>(pass.targets[i]));
        res += part;
    }
    if (pass.dsv_resource && n > 0 && n < 540) {
        const D3D12_RESOURCE_DESC dd = pass.dsv_resource->GetDesc();
        std::snprintf(text + n, sizeof(text) - n, " dsv fmt %d %llux%u", static_cast<int>(pass.dsv_format), static_cast<unsigned long long>(dd.Width),
                      dd.Height);
        std::snprintf(part, sizeof(part), " dsv %p", static_cast<void*>(pass.dsv_resource));
        res += part;
    } else if (n > 0 && n < 540) {
        std::snprintf(text + n, sizeof(text) - n, " dsv none");
    }
    AcquireSRWLockExclusive(&mesh_fs_lock);
    mesh_fs_sets().emplace(h, std::string(text) + '\x1f' + res);
    ReleaseSRWLockExclusive(&mesh_fs_lock);
    return h;
}
// mesh_hook: a query index for this call, or -1. The current frame's slot.
LONG mesh_fs_begin(const Pass& pass, bool groups)
{
    if (!mesh_fs_collecting() || !mesh_fs_ready()) return -1;
    const LONG frame = mesh_fs_current;
    MeshFrameSlot& slot = mesh_fs_slot[static_cast<UINT>(frame) % mesh_fs_slots];
    if (slot.frame != frame) return -1;  // the slot is still being read (cannot happen with 16 slots and 8 presents)
    const LONG i = InterlockedIncrement(&slot.used) - 1;
    if (i >= static_cast<LONG>(mesh_fs_calls)) return -1;
    if (groups) InterlockedIncrement(&slot.groups);
    MeshFrameCall& call = mesh_fs_calls_of()[(static_cast<UINT>(frame) % mesh_fs_slots) * mesh_fs_calls + static_cast<UINT>(i)];
    call.set = mesh_fs_set(pass);
    call.groups = groups;
    return static_cast<LONG>((static_cast<UINT>(frame) % mesh_fs_slots) * mesh_fs_calls) + i;
}
void mesh_fs_read(UINT s)
{
    MeshFrameSlot& slot = mesh_fs_slot[s];
    const LONG used = slot.used < static_cast<LONG>(mesh_fs_calls) ? slot.used : static_cast<LONG>(mesh_fs_calls);
    const bool counted = slot.groups > 0;
    if (counted && used > 0 && mesh_fs_readback) {
        struct Sum {
            LONG calls = 0, groups = 0;
            std::uint64_t v[5] = {};
        };
        std::map<u64, Sum> sums;
        Sum all;
        void* mapped = nullptr;
        const SIZE_T base = SIZE_T{s} * mesh_fs_calls * mesh_stats_size();
        const D3D12_RANGE range{base, base + static_cast<SIZE_T>(used) * mesh_stats_size()};
        if (SUCCEEDED(mesh_fs_readback->Map(0, &range, &mapped))) {
            for (LONG i = 0; i < used; ++i) {
                D3D12_QUERY_DATA_PIPELINE_STATISTICS1 q{};
                std::memcpy(&q, static_cast<const unsigned char*>(mapped) + base + std::size_t(i) * mesh_stats_size(), mesh_stats_size());
                const MeshFrameCall& call = mesh_fs_calls_of()[s * mesh_fs_calls + static_cast<UINT>(i)];
                const std::uint64_t v[5] = {q.MSInvocations, q.MSPrimitives, q.CInvocations, q.CPrimitives, q.PSInvocations};
                for (Sum* sum : {&sums[call.set], &all}) {
                    ++sum->calls;
                    sum->groups += call.groups;
                    for (int k = 0; k < 5; ++k) sum->v[k] += v[k];
                }
            }
            const D3D12_RANGE none{0, 0};
            mesh_fs_readback->Unmap(0, &none);
        }
        const LONG number = InterlockedIncrement(&mesh_fs_reported);
        for (const auto& [set, sum] : sums) {
            AcquireSRWLockShared(&mesh_fs_lock);
            const auto found = mesh_fs_sets().find(set);
            const std::string text = found == mesh_fs_sets().end() ? std::string("?\x1f") : found->second;
            ReleaseSRWLockShared(&mesh_fs_lock);
            const std::size_t cut = text.find('\x1f');
            if (mesh_stats.v1)
                log("x2w-mesh stats frame %ld set %s: calls %ld (%ld with groups), MS invocations %llu primitives %llu, clipper invocations %llu "
                    "primitives %llu, PS invocations %llu%s", slot.frame, text.substr(0, cut).c_str(), sum.calls, sum.groups, sum.v[0], sum.v[1],
                    sum.v[2], sum.v[3], sum.v[4], text.substr(cut + 1).c_str());
            else
                log("x2w-mesh stats frame %ld set %s: calls %ld (%ld with groups), (no MS counts) clipper invocations %llu primitives %llu, PS "
                    "invocations %llu%s", slot.frame, text.substr(0, cut).c_str(), sum.calls, sum.groups, sum.v[2], sum.v[3], sum.v[4],
                    text.substr(cut + 1).c_str());
        }
        log("x2w-mesh stats frames total %ld of %ld frames (frame %ld): %zu target sets, calls %ld (%ld with groups%s), MS invocations %llu "
            "primitives %llu, clipper invocations %llu primitives %llu, PS invocations %llu", number, mesh_fs_wanted(), slot.frame, sums.size(),
            all.calls, all.groups, slot.used > static_cast<LONG>(mesh_fs_calls) ? "; calls past 2048 had no query" : "", all.v[0], all.v[1], all.v[2],
            all.v[3], all.v[4]);
    } else if (first_calls(mesh_fs_skipped_lines, 8)) {
        log("x2w-mesh stats frame %ld not counted (no record with X > 0 at record time; %ld mesh calls)", slot.frame, slot.used);
    }
    slot.used = slot.groups = 0;
    slot.due = 0;
    slot.closed = false;
    slot.frame = -1;
}
// Ends the frame being collected: it counts if a call had groups; its results are read `delay` presents later.
void mesh_fs_close(LONG delay)
{
    AcquireSRWLockExclusive(&mesh_fs_lock);
    const LONG frame = mesh_fs_current;
    MeshFrameSlot& slot = mesh_fs_slot[static_cast<UINT>(frame) % mesh_fs_slots];
    if (slot.frame == frame) {
        if (slot.groups > 0) ++mesh_fs_counted;
        slot.due = mesh_fs_presents + delay;
        slot.closed = true;
    }
    mesh_fs_current = frame + 1;
    MeshFrameSlot& next = mesh_fs_slot[static_cast<UINT>(frame + 1) % mesh_fs_slots];
    if (next.frame == -1 && mesh_fs_counted < mesh_fs_wanted()) next.frame = frame + 1;
    ReleaseSRWLockExclusive(&mesh_fs_lock);
}
void mesh_fs_present()
{
    if (!mesh_fs_wanted() || !mesh_fs_heap) return;
    const LONG now = InterlockedIncrement(&mesh_fs_presents);
    mesh_fs_close(8);
    for (UINT s = 0; s < mesh_fs_slots; ++s)
        if (mesh_fs_slot[s].frame >= 0 && mesh_fs_slot[s].closed && now >= mesh_fs_slot[s].due) mesh_fs_read(s);
}
// Fixtures (after their fence): ends the current frame and reads every closed slot now.
extern "C" __declspec(dllexport) void x2w_d3d12_mesh_frame_stats_report()
{
    if (!mesh_fs_heap) return;
    mesh_fs_close(0);
    for (UINT s = 0; s < mesh_fs_slots; ++s)
        if (mesh_fs_slot[s].frame >= 0 && mesh_fs_slot[s].closed) mesh_fs_read(s);
}
void mesh_trace_call(Wrapper* self, bool drawn, const Translation* tr, const char* why)
{
    const Pass& pass = pass_of(self);
    for (UINT i = 0; i < pass.target_count; ++i) note_traced_target(self, pass.targets[i], drawn);
    if (pass.dsv_resource)
        note_traced_target(self, pass.dsv_resource, drawn && !pass.dsv_read_only,
                           pass.dsv_read_only ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    char text[900];
    int n = 0;
    const auto add = [&](const char* format, auto... args) {
        if (n >= 0 && n < static_cast<int>(sizeof(text))) n += std::snprintf(text + n, sizeof(text) - n, format, args...);
    };
    add("x2w-frame mesh %s key %016llx", drawn ? "drawn" : "NOT drawn", static_cast<unsigned long long>(mesh_trace_key));
    if (tr && tr->key != mesh_trace_key) add(" variant %016llx", static_cast<unsigned long long>(tr->key));
    add(" targets %u", pass.target_count);
    for (UINT i = 0; i < pass.target_count; ++i) {
        const D3D12_RESOURCE_DESC rd = pass.targets[i] ? pass.targets[i]->GetDesc() : D3D12_RESOURCE_DESC{};
        add(" [%u %p %llux%u fmt %d]", i, static_cast<void*>(pass.targets[i]), static_cast<unsigned long long>(rd.Width), rd.Height,
            static_cast<int>(pass.target_formats[i]));
    }
    add(" dsv %p fmt %d%s", static_cast<void*>(pass.dsv_resource), static_cast<int>(pass.dsv_format), pass.dsv_read_only ? " read-only" : "");
    if (pass.dsv_resource) {
        DepthClearValue clear;
        const D3D12_RESOURCE_DESC dd = pass.dsv_resource->GetDesc();
        add(" (%llux%u res fmt %d", static_cast<unsigned long long>(dd.Width), dd.Height, static_cast<int>(dd.Format));
        if (last_depth_clear(pass.dsv_resource, &clear))
            add(", last clear depth %g stencil %u flags 0x%x frame %ld)", clear.depth, clear.stencil, clear.flags, clear.frame);
        else
            add(", no forwarded clear)");
    }
    if (mesh_trace_query >= 0) add(" query #%ld", mesh_trace_query);
    if (pass.viewport_set)
        add(" viewport %.1f %.1f %.1f %.1f depth %.2f-%.2f", pass.viewport0.TopLeftX, pass.viewport0.TopLeftY, pass.viewport0.Width,
            pass.viewport0.Height, pass.viewport0.MinDepth, pass.viewport0.MaxDepth);
    else
        add(" viewport none");
    if (pass.scissor_set)
        add(" scissor %ld %ld %ld %ld", pass.scissor0.left, pass.scissor0.top, pass.scissor0.right, pass.scissor0.bottom);
    else
        add(" scissor none");
    if (mesh_trace_bound) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
        u64 key = 0;
        bool decoded = false;
        mesh_state_desc(*mesh_trace_bound, pass, desc, key, &decoded);
        const auto& d = desc.DepthStencilState;
        add(" depth %d func %d write %d stencil %d", d.DepthEnable, static_cast<int>(d.DepthFunc), d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL,
            d.StencilEnable);
        // The PSO's rasterizer (the desc mesh_pso creates; a failed creation falls back to the default state, see the
        // desktop-pipeline line) and the title's words it is decoded from. D3D12 cull: 1 none, 2 front, 3 back.
        const auto& r = desc.RasterizerState;
        const BoundPipeline& b = *mesh_trace_bound;
        const bool title_rs = b.derived_rs || (b.has_words && b.rs.ok);
        add(" raster cull %d ccw %d depthclip %d bias %d/%g/%g fill %d (%s", static_cast<int>(r.CullMode), r.FrontCounterClockwise, r.DepthClipEnable,
            r.DepthBias, r.DepthBiasClamp, r.SlopeScaledDepthBias, static_cast<int>(r.FillMode),
            mesh_switches().cull ? (title_rs ? "from the title" : "default: title rasterizer not decoded")
            : mesh_switches().depthclip && title_rs ? "default, depth clip from the title (X2W_DRAW_STATE_DEPTHCLIP)"
            : "default: X2W_MESH_STATE off or NOCULL");
        (void)decoded;
        if (b.derived_rs)
            add("; title: slot 55 rasterizer part");
        else if (b.has_words)
            add("; title PA_SU_SC_MODE_CNTL %08x PA_CL_CLIP_CNTL %08x%s%s%s", b.raw_mode, b.raw_clip, b.mesh_layout ? " mesh layout" : " vertex layout",
                b.rs.ok ? "" : ", refused: ", b.rs.ok ? "" : (b.rs.reason ? b.rs.reason : "?"));
        else
            add("; no title state words");
        if (title_rs) {  // what the title's words decode to, whether applied or not (FACE 0 = counter-clockwise front)
            const D3D12_RASTERIZER_DESC& t = b.derived_rs ? b.patch_rs : b.rs.desc;
            add(" -> decoded cull %d ccw %d depthclip %d bias %d", static_cast<int>(t.CullMode), t.FrontCounterClockwise, t.DepthClipEnable, t.DepthBias);
        }
        add("%s)", mesh_cull_flip() ? "; X2W_MESH_STATE_CULL_FLIP" : "");
    }
    if (mesh_trace_record) {
        const UINT words = mesh_trace_stride / 4 < 8 ? mesh_trace_stride / 4 : 8;
        if (words && x2w_local::readable(mesh_trace_record, words * 4)) {
            add(" record (max count %u, stride %u):", mesh_trace_max, mesh_trace_stride);
            for (UINT i = 0; i < words; ++i) {
                std::uint32_t w;
                std::memcpy(&w, mesh_trace_record + 4 * i, 4);
                add(" %08x", w);
            }
        } else {
            add(" record unreadable");
        }
    }
    if (!drawn) add(": %s", why ? why : "not issued");
    log("%s", text);
}

// indirect_run (d3d12_indirect.inl), type DispatchMesh, before the record is staged. False: skip the call (counted
// here by reason; indirect counts it as no-pipeline). No draw_lock held on entry.
bool mesh_prepare_body(Wrapper* self, ID3D12GraphicsCommandList* list);
bool mesh_prepare(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    const LONG64 t0 = mesh_now();
    if (trace_frame_active) mesh_trace_key = 0, mesh_trace_bound = nullptr, mesh_last_skip = -1;
    const bool ok = mesh_prepare_body(self, list);
    InterlockedAdd64(&mesh_prepare_ticks, mesh_now() - t0);
    if (!ok && trace_frame_active) mesh_trace_call(self, false, nullptr, mesh_last_skip >= 0 ? mesh_skip_names[mesh_last_skip] : "refused");
    return ok;
}
bool mesh_prepare_body(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    mesh_ready = {};
    InterlockedIncrement(&mesh_calls);
    if (!list || !mesh_tier_ok()) return mesh_skip(mesh_tier), false;
    Pass& pass = pass_of(self);
    if (!pass.pipeline) return mesh_skip(mesh_no_record), false;
    AcquireSRWLockShared(&mesh_key_lock);
    const auto found = mesh_bindings().find(pass.pipeline);
    MeshBinding binding = found != mesh_bindings().end() ? found->second : MeshBinding{};
    const bool known = found != mesh_bindings().end();
    ReleaseSRWLockShared(&mesh_key_lock);
    if (!known) {
        binding.mesh = mesh_key_of(pass.pipeline, &binding.key);
        lock_draw(300000 + __LINE__);
        binding.bound = translation_for(pass.pipeline);
        ReleaseSRWLockExclusive(&draw_lock);
        AcquireSRWLockExclusive(&mesh_key_lock);
        mesh_bindings()[pass.pipeline] = binding;
        ReleaseSRWLockExclusive(&mesh_key_lock);
    }
    if (!binding.mesh || !binding.key.keyed || !binding.bound) return mesh_skip(mesh_no_record), false;
    if (trace_frame_active) mesh_trace_key = binding.key.key, mesh_trace_bound = binding.bound;
    // The key's entry is kept with the binding (map nodes do not move): loaded_mesh (mesh_lock + lookup) only for the
    // first call and, for a key without a translation, when its 5 s retry is due (racy reads of state / retry_at: at
    // worst one extra loaded_mesh).
    MeshEntry* entry = binding.entry;
    if (!entry || (entry->state != 1 && entry->state != 2 && GetTickCount64() >= entry->retry_at)) {
        entry = &loaded_mesh(binding.key.key);
        if (!binding.entry) {
            AcquireSRWLockExclusive(&mesh_key_lock);
            mesh_bindings()[pass.pipeline].entry = entry;
            ReleaseSRWLockExclusive(&mesh_key_lock);
        }
    }
    MeshEntry& e = *entry;
    mesh_note_use(e, binding.key.key);  // every keyed call, whatever is refused below (coverage)
    if (!pass.targets_bound) {
        mesh_refusal_line(e, binding.key.key, mesh_no_targets, "no targets: no render target or depth view bound by the list");
        return mesh_skip(mesh_no_targets), false;
    }
    // A depth-only pipeline (no pixel stage) needs no variant (translation_for_targets takes the base for it).
    const bool rtv_variant = e.tr && e.tr->ps && std::any_of(pass.target_formats, pass.target_formats + pass.target_count, needs_rtv_variant);
    if (e.state == 4) return mesh_skip(mesh_no_translation), false;
    if (e.state != 1) return mesh_skip(mesh_failed), false;
    if (binding.attached != e.tr) {
        // The mesh translation becomes this pipeline's translation, so draw_translated finds it (translation_for leaves
        // it null for a record without vertex code). Only a ready translation is attached.
        lock_draw(300000 + __LINE__);
        const bool free = !binding.bound->translation || binding.bound->translation == e.tr;
        if (free) binding.bound->translation = e.tr;
        ReleaseSRWLockExclusive(&draw_lock);
        if (!free) {
            mesh_refusal_line(e, binding.key.key, mesh_failed, "translation taken: the bound pipeline already has another translation");
            return mesh_skip(mesh_failed), false;
        }
        AcquireSRWLockExclusive(&mesh_key_lock);
        mesh_bindings()[pass.pipeline].attached = e.tr;
        ReleaseSRWLockExclusive(&mesh_key_lock);
    }
    const u64 signature = mesh_pass_signature(pass);
    ID3D12PipelineState* pso = binding.pso && binding.pso_signature == signature ? binding.pso : nullptr;
    Translation* tr = pso ? binding.pso_tr : e.tr;
    if (!pso && rtv_variant) {
        // Integer / 32-bit float targets: the pixel stage's RTV variant, the same contract as ordinary draws
        // (translation_for_targets: key fnv("rtv1 <base key> <formats>"), request req_<variant>.rtv, manifest with the
        // rtv1 line). draw_translated resolves the same variant entry for this call. A variant that is not ready is
        // looked up again (under draw_lock) at most once a second per binding and pass signature.
        if (binding.variant_wait_signature == signature && GetTickCount64() < binding.variant_wait_until) return mesh_skip(mesh_rtv_variant), false;
        lock_draw(300000 + __LINE__);
        tr = translation_for_targets(e.tr, pass);
        const bool ready = tr && tr->state == 1 && tr->ms;
        ReleaseSRWLockExclusive(&draw_lock);
        if (!ready) {
            AcquireSRWLockExclusive(&mesh_key_lock);
            MeshBinding& kept = mesh_bindings()[pass.pipeline];
            kept.variant_wait_signature = signature;
            kept.variant_wait_until = GetTickCount64() + 1000;
            ReleaseSRWLockExclusive(&mesh_key_lock);
            if (!mesh_refusal_logged(e, mesh_rtv_variant)) {
                char detail[200];
                int n = std::snprintf(detail, sizeof(detail), "rtv variant: target formats");
                for (UINT i = 0; i < pass.target_count && n < 120; ++i)
                    n += std::snprintf(detail + n, sizeof(detail) - n, " %d", static_cast<int>(pass.target_formats[i]));
                std::snprintf(detail + n, sizeof(detail) - n, "; variant %016llx %s", tr ? static_cast<unsigned long long>(tr->key) : 0ull,
                              tr && tr->state == 1 ? "has no mesh stage" : "not translated yet (req_<variant>.rtv published)");
                mesh_refusal_line(e, binding.key.key, mesh_rtv_variant, detail);
            }
            return mesh_skip(mesh_rtv_variant), false;
        }
        static LONG variants_logged;
        if (first_calls(variants_logged, 256))
            log("x2w-mesh pipeline %016llx: RTV variant %016llx for target formats %d%s (pixel stage %s)", static_cast<unsigned long long>(binding.key.key),
                static_cast<unsigned long long>(tr->key), static_cast<int>(pass.target_formats[0]), pass.target_count > 1 ? " ..." : "", tr->ps ? "yes" : "none");
    }
    static volatile LONG reuse_logged;  // once per process: the first call served by the PSO kept with the binding
    if (pso && !reuse_logged && InterlockedExchange(&reuse_logged, 1) == 0)
        log("x2w-mesh pipeline %016llx: PSO kept with the binding reused (no state desc, hash or mesh_lock for this call; logged once)",
            static_cast<unsigned long long>(binding.key.key));
    if (!pso) {
        bool compiling = false;
        pso = mesh_pso(e, *tr, *binding.bound, pass, &compiling);
        if (compiling) return mesh_skip(mesh_compiling), false;
        if (pso) {
            AcquireSRWLockExclusive(&mesh_key_lock);
            MeshBinding& kept = mesh_bindings()[pass.pipeline];
            kept.pso = pso;
            kept.pso_tr = tr;
            kept.pso_signature = signature;
            ReleaseSRWLockExclusive(&mesh_key_lock);
        }
    }
    if (!pso) {
        mesh_refusal_line(e, binding.key.key, mesh_no_pso, "no PSO: CreatePipelineState failed, see its desktop pipeline line");
        return mesh_skip(mesh_no_pso), false;
    }
    mesh_ready = {self, tr, pso, &e};
    return true;
}

// indirect_mesh_hook: the call is staged (copy + guard). Binds through draw_translated, whose issue point calls
// indirect_issue. True only when the ExecuteIndirect was recorded.
bool mesh_hook(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    if (mesh_ready.list != self || !mesh_ready.pso || !list) return false;
    mesh_binding_now = true;
    const LONG64 t0 = mesh_now();
    // LOCAL meshrt: traced frame: a pipeline statistics query around the call (mesh_stats_ready).
    mesh_trace_query = -1;
    if (trace_frame_active && mesh_stats_ready()) {
        const LONG slot = InterlockedIncrement(&mesh_stats_next) - 1;
        if (slot < static_cast<LONG>(mesh_stats.capacity)) {
            mesh_trace_query = slot;
            mesh_stats_keys[slot] = mesh_trace_key;
            list->BeginQuery(mesh_stats.heap, mesh_stats_type(), static_cast<UINT>(slot));
        }
    }
    // LOCAL meshrt: X2W_MESH_STATS_FRAMES: outside the traced frame, a query from the frame slots (mesh_fs_begin).
    LONG fs_query = -1;
    if (!trace_frame_active && mesh_fs_collecting()) {
        std::uint32_t xyz[3] = {};
        const bool groups = mesh_dump_record_groups(xyz) && xyz[0] && xyz[1] && xyz[2];
        fs_query = mesh_fs_begin(pass_of(self), groups);
        if (fs_query >= 0) list->BeginQuery(mesh_fs_heap, mesh_stats_type(), static_cast<UINT>(fs_query));
    }
    const bool issued = draw_translated(self, false, 0, 0, 0, 0, 0);
    if (fs_query >= 0) {
        list->EndQuery(mesh_fs_heap, mesh_stats_type(), static_cast<UINT>(fs_query));
        list->ResolveQueryData(mesh_fs_heap, mesh_stats_type(), static_cast<UINT>(fs_query), 1, mesh_fs_readback,
                               u64{static_cast<UINT>(fs_query)} * mesh_stats_size());
    }
    if (mesh_trace_query >= 0) {
        list->EndQuery(mesh_stats.heap, mesh_stats_type(), static_cast<UINT>(mesh_trace_query));
        list->ResolveQueryData(mesh_stats.heap, mesh_stats_type(), static_cast<UINT>(mesh_trace_query), 1, mesh_stats.readback,
                               u64{static_cast<UINT>(mesh_trace_query)} * mesh_stats_size());
    }
    InterlockedAdd64(&mesh_bind_ticks, mesh_now() - t0);
    InterlockedIncrement64(&mesh_bind_calls);
    mesh_binding_now = false;
    if (trace_frame_active) mesh_trace_call(self, issued, mesh_ready.tr, "bind refused by draw_translated");
    if (!mesh_dump_dir.empty()) {  // LOCAL meshrt: X2W_MESH_DUMP_INPUTS: the record of the dumped call, then done
        const UINT bytes = mesh_trace_stride * (mesh_trace_max ? mesh_trace_max : 1);
        const bool readable_record = mesh_trace_record && bytes && bytes <= 65536 && x2w_local::readable(mesh_trace_record, bytes);
        char note[160];
        std::snprintf(note, sizeof(note), "indirect record(s) in title memory at record time: max count %u, stride %u; call %s", mesh_trace_max,
                      mesh_trace_stride, issued ? "issued" : "NOT issued (bind refused)");
        mesh_dump_file("record.bin", readable_record ? mesh_trace_record : nullptr, readable_record ? bytes : 0, note);
        mesh_dump_dir.clear();
    }
    MeshEntry* e = mesh_ready.entry;
    mesh_ready = {};
    if (!issued) {
        if (e) mesh_refusal_line(*e, e->tr->key, mesh_bind, "bind refused by draw_translated; heap / vinfo / vdesc refusals also have an x2w-draw-rt line, others none");
        return mesh_skip(mesh_bind), false;
    }
    InterlockedIncrement(&mesh_issued);
    if (e && !e->issued_once) {
        e->issued_once = true;  // a benign race: at worst two lines
        static LONG shown;
        if (first_calls(shown, 1024)) log("x2w-mesh pipeline %016llx: first call issued", e->tr->key);
    }
    return true;
}

// d3d12_draw.inl hooks: draw_translated binds a mesh call (topology gate off, no rect entry value) ...
bool mesh_issue_active(Wrapper* self) { return mesh_binding_now && mesh_ready.list == self && indirect_pending(self); }
// ... and pipeline_for returns the pre-built mesh PSO for a mesh translation (null outside a mesh call: a plain
// draw with a mesh pipeline bound draws nothing).
ID3D12PipelineState* mesh_pso_for(const Translation& tr) { return mesh_binding_now && mesh_ready.tr == &tr ? mesh_ready.pso : nullptr; }

const bool mesh_hook_installed = [] {
    if (mesh_enabled()) indirect_mesh_hook = &mesh_hook;
    return mesh_enabled();
}();

// Every 300 frames (queue_present), X2W_MESH=1 only. Counters restart after each line.
void report_mesh(LONG frames)
{
    if (!mesh_enabled()) return;
    LONG s[mesh_skips];
    for (int i = 0; i < mesh_skips; ++i) s[i] = InterlockedExchange(&mesh_skipped[i], 0);
    const LONG calls = InterlockedExchange(&mesh_calls, 0), issued = InterlockedExchange(&mesh_issued, 0);
    LONG accounted = issued;
    for (LONG v : s) accounted += v;
    log("x2w-mesh %ld frames: calls %ld issued %ld; skipped: no translation %ld, translation failed %ld, no PSO %ld, no targets %ld, "
        "tier %ld; also no mesh record %ld, rtv variant %ld, bind refused %ld, refused by indirect after prepare %ld, PSO compiling "
        "(X2W_MESH_ASYNC) %ld; records keyed %ld unkeyed %ld (total)",
        frames, calls, issued, s[mesh_no_translation], s[mesh_failed], s[mesh_no_pso], s[mesh_no_targets], s[mesh_tier], s[mesh_no_record],
        s[mesh_rtv_variant], s[mesh_bind], calls - accounted, s[mesh_compiling], mesh_records_keyed, mesh_records_unkeyed);
    // LOCAL meshrt: stand-in CPU per call (QueryPerformanceCounter around mesh_prepare and the bind).
    static const double us_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / static_cast<double>(f.QuadPart);
    }();
    const LONG64 prepare = InterlockedExchange64(&mesh_prepare_ticks, 0), bind = InterlockedExchange64(&mesh_bind_ticks, 0);
    const LONG64 binds = InterlockedExchange64(&mesh_bind_calls, 0);
    log("x2w-mesh %ld frames cpu: prepare %.1f us/call over %ld calls (%.0f ms), bind %.1f us/call over %lld binds (%.0f ms; draw_translated "
        "including its draw_lock wait); PSOs with decoded state %ld, of them on the default state after a refusal %ld (total)", frames,
        calls ? prepare * us_per_tick / calls : 0.0, calls, prepare * us_per_tick / 1000.0, binds ? bind * us_per_tick / binds : 0.0,
        static_cast<long long>(binds), bind * us_per_tick / 1000.0, mesh_state_changed, mesh_state_fallback);
    report_mesh_keys(frames);
}
