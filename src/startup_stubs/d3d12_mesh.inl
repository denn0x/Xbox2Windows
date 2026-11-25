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

enum MeshSkip { mesh_no_translation, mesh_failed, mesh_no_pso, mesh_no_targets, mesh_tier, mesh_no_record, mesh_rtv_variant, mesh_bind, mesh_skips };
LONG mesh_calls, mesh_issued, mesh_skipped[mesh_skips];
void mesh_skip(MeshSkip why) { InterlockedIncrement(&mesh_skipped[why]); }

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
    if (s.depth || s.cull || s.blend) {
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
    key = fnv_bytes(&desc.BlendState, sizeof(desc.BlendState), key);
    key = fnv_bytes(&desc.DepthStencilState, sizeof(desc.DepthStencilState), key);
    key = fnv_bytes(&desc.RasterizerState, sizeof(desc.RasterizerState), key);
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

// The PSO for (entry, state of this call), compiled once per key without draw_lock (failures are kept).
ID3D12PipelineState* mesh_pso(MeshEntry& e, const BoundPipeline& bound, const Pass& pass)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, fallback;
    u64 key = 0;
    bool decoded = false;
    mesh_state_desc(bound, pass, desc, key, &decoded, &fallback);
    AcquireSRWLockExclusive(&mesh_lock);
    MeshPso& slot = e.psos[key];
    while (slot.state == 3) SleepConditionVariableSRW(&mesh_loaded, &mesh_lock, INFINITE, 0);
    if (slot.state == 1 || slot.state == 2) {
        ID3D12PipelineState* pso = slot.pso;
        ReleaseSRWLockExclusive(&mesh_lock);
        return pso;
    }
    slot.state = 3;
    ReleaseSRWLockExclusive(&mesh_lock);
    HRESULT hr = E_FAIL;
    ID3D12PipelineState* pso = create_mesh_pso(*e.tr, desc, &hr);
    const auto& d = desc.DepthStencilState;
    log("x2w-mesh desktop pipeline for %016llx: %u targets (format %d), dsv %d, depth %d, pixel stage %s -> 0x%08lx; state %s: depth func %d "
        "write %d stencil %d, cull %d ccw %d, blend rt0 %d mask 0x%x", e.tr->key, pass.target_count, pass.target_count ? pass.target_formats[0] : 0,
        static_cast<int>(desc.DSVFormat), d.DepthEnable ? 1 : 0, e.tr->ps ? "yes" : "none", static_cast<unsigned long>(hr),
        decoded ? "decoded (X2W_MESH_STATE)" : "default", d.DepthFunc, d.DepthWriteMask, d.StencilEnable, desc.RasterizerState.CullMode,
        desc.RasterizerState.FrontCounterClockwise, desc.BlendState.RenderTarget[0].BlendEnable, desc.BlendState.RenderTarget[0].RenderTargetWriteMask);
    if (decoded) InterlockedIncrement(&mesh_state_changed);
    if (!pso && decoded) {
        // The decoded state was refused by the runtime: the default state instead (NOT exercised by a test).
        pso = create_mesh_pso(*e.tr, fallback, &hr);
        InterlockedIncrement(&mesh_state_fallback);
        log("x2w-mesh desktop pipeline for %016llx: decoded state refused, default state -> 0x%08lx", e.tr->key, static_cast<unsigned long>(hr));
    }
    check_device("translated mesh CreatePipelineState", 0);
    AcquireSRWLockExclusive(&mesh_lock);
    slot.pso = pso;
    slot.state = pso ? 1 : 2;
    ReleaseSRWLockExclusive(&mesh_lock);
    WakeAllConditionVariable(&mesh_loaded);
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
};

// Everything of the pass that mesh_state_desc / apply_draw_state read: targets, DSV format, read-only DSV, rect
// topology. Equal signatures give the same PSO key for the same title pipeline (its BoundPipeline state is fixed).
u64 mesh_pass_signature(const Pass& pass)
{
    u64 s = 0x9e3779b97f4a7c15ull ^ pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) s = s * 1099511628211ull + static_cast<u64>(pass.target_formats[i]);
    s = s * 1099511628211ull + static_cast<u64>(pass.dsv_format);
    s = s * 1099511628211ull + (pass.dsv_read_only ? 1u : 0u) + (pass.topology == 0x11 ? 2u : 0u);
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

// indirect_run (d3d12_indirect.inl), type DispatchMesh, before the record is staged. False: skip the call (counted
// here by reason; indirect counts it as no-pipeline). No draw_lock held on entry.
bool mesh_prepare_body(Wrapper* self, ID3D12GraphicsCommandList* list);
bool mesh_prepare(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    const LONG64 t0 = mesh_now();
    const bool ok = mesh_prepare_body(self, list);
    InterlockedAdd64(&mesh_prepare_ticks, mesh_now() - t0);
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
    // Integer / 32-bit float targets need an RTV variant of the pixel stage (translation_for_targets); none exists
    // for mesh translations yet.
    if (std::any_of(pass.target_formats, pass.target_formats + pass.target_count, needs_rtv_variant)) {
        if (mesh_refusal_logged(e, mesh_rtv_variant)) return mesh_skip(mesh_rtv_variant), false;
        char detail[160];
        int n = std::snprintf(detail, sizeof(detail), "rtv variant: target formats");
        for (UINT i = 0; i < pass.target_count && n < 120; ++i) n += std::snprintf(detail + n, sizeof(detail) - n, " %d", static_cast<int>(pass.target_formats[i]));
        std::snprintf(detail + n, sizeof(detail) - n, "; integer / 32-bit float targets need a pixel-stage variant, none for mesh");
        mesh_refusal_line(e, binding.key.key, mesh_rtv_variant, detail);
        return mesh_skip(mesh_rtv_variant), false;
    }
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
    static volatile LONG reuse_logged;  // once per process: the first call served by the PSO kept with the binding
    if (pso && !reuse_logged && InterlockedExchange(&reuse_logged, 1) == 0)
        log("x2w-mesh pipeline %016llx: PSO kept with the binding reused (no state desc, hash or mesh_lock for this call; logged once)",
            static_cast<unsigned long long>(binding.key.key));
    if (!pso) {
        pso = mesh_pso(e, *binding.bound, pass);
        if (pso) {
            AcquireSRWLockExclusive(&mesh_key_lock);
            MeshBinding& kept = mesh_bindings()[pass.pipeline];
            kept.pso = pso;
            kept.pso_signature = signature;
            ReleaseSRWLockExclusive(&mesh_key_lock);
        }
    }
    if (!pso) {
        mesh_refusal_line(e, binding.key.key, mesh_no_pso, "no PSO: CreatePipelineState failed, see its desktop pipeline line");
        return mesh_skip(mesh_no_pso), false;
    }
    mesh_ready = {self, e.tr, pso, &e};
    return true;
}

// indirect_mesh_hook: the call is staged (copy + guard). Binds through draw_translated, whose issue point calls
// indirect_issue. True only when the ExecuteIndirect was recorded.
bool mesh_hook(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    if (mesh_ready.list != self || !mesh_ready.pso || !list) return false;
    mesh_binding_now = true;
    const LONG64 t0 = mesh_now();
    const bool issued = draw_translated(self, false, 0, 0, 0, 0, 0);
    InterlockedAdd64(&mesh_bind_ticks, mesh_now() - t0);
    InterlockedIncrement64(&mesh_bind_calls);
    mesh_binding_now = false;
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
        "tier %ld; also no mesh record %ld, rtv variant %ld, bind refused %ld, refused by indirect after prepare %ld; "
        "records keyed %ld unkeyed %ld (total)",
        frames, calls, issued, s[mesh_no_translation], s[mesh_failed], s[mesh_no_pso], s[mesh_no_targets], s[mesh_tier], s[mesh_no_record],
        s[mesh_rtv_variant], s[mesh_bind], calls - accounted, mesh_records_keyed, mesh_records_unkeyed);
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
