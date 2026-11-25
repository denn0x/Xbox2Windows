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
enum IndirectType { itype_draw, itype_indexed, itype_dispatch, itype_mesh, itype_other, itype_types };
constexpr const char* indirect_type_names[itype_types] = {"draw", "indexed", "dispatch", "mesh", "other"};
int indirect_type_of(UINT argument_type)
{
    switch (argument_type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: return itype_draw;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: return itype_indexed;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: return itype_dispatch;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: return itype_mesh;
    default: return itype_other;
    }
}

// Why a call was not executed. "not-implemented": no execution path exists yet for the type.
enum IndirectSkip {
    skip_not_recording,      // the list is not recording a desktop list (not render mode, or closed)
    skip_unknown_signature,  // the signature was not created through device slot 41
    skip_not_implemented,    // no execution path for this type (yet)
    skip_switched_off,       // the type's env switch is off
    skip_no_pipeline,        // the bound pipeline has no desktop translation (or the path refused the call)
    skip_unresolved_buffer,  // argument or count buffer has no in-place desktop window (or no index data)
    skip_zero_count,         // max count 0
    skip_topology,           // a draw with a console-only topology (rect list)
    skip_no_ring,            // the private argument ring could not be created or the record is too large
    skip_no_guard,           // the argument guard pass could not be created (calls are never executed unguarded)
    skip_reasons
};
constexpr const char* indirect_skip_names[skip_reasons] = {"not-recording", "unknown-signature", "not-implemented", "off",
                                                           "no-pipeline", "unresolved-buffer", "zero-count", "unsupported-topology",
                                                           "no-ring", "no-guard"};

struct IndirectCounters {
    LONG calls[itype_types];
    LONG executed[itype_types];
    LONG skipped[skip_reasons][itype_types];
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

// ---- Execution -------------------------------------------------------------------------------
//
// Per call: the argument record (and count dword) is copied from the window over the title's memory
// into a private argument ring, and ExecuteIndirect reads the ring (DESIGN.md section 3):
//   window UAV -> COPY_SOURCE, ring COMMON -> COPY_DEST; copy; window -> UAV, ring -> INDIRECT_ARGUMENT;
//   ExecuteIndirect; [read-back copy]; ring -> COMMON.
// A window rests in COMMON or UAV-promoted (compute binds it as UAV); UAV is a valid BeforeState for both.

// X2W_INDIRECT_OFF=1: nothing executed. X2W_INDIRECT_DISPATCH=0 / X2W_INDIRECT_MESH=0: per type (default on).
// Draw/DrawIndexed: default OFF until a test or run has exercised it; X2W_INDIRECT_DRAW=1 turns it on.
// X2W_INDIRECT_READBACK=<n>: read back the first n executed records per report.
bool indirect_env_zero(const char* name)
{
    char value[8]{};
    return GetEnvironmentVariableA(name, value, sizeof(value)) && value[0] == '0';
}
bool indirect_type_enabled(int type)
{
    static const bool off = x2w_local::env_on("X2W_INDIRECT_OFF");
    static const bool dispatch = !indirect_env_zero("X2W_INDIRECT_DISPATCH");
    static const bool draw = x2w_local::env_on("X2W_INDIRECT_DRAW");
    static const bool mesh = !indirect_env_zero("X2W_INDIRECT_MESH");
    if (off) return false;
    return type == itype_dispatch ? dispatch : type == itype_draw || type == itype_indexed ? draw : type == itype_mesh ? mesh : false;
}
LONG indirect_readback_wanted()
{
    static const LONG wanted = [] {
        char value[16]{};
        const LONG n = GetEnvironmentVariableA("X2W_INDIRECT_READBACK", value, sizeof(value)) ? std::atol(value) : 0L;
        return n < 0 ? 0L : n > 4096 ? 4096L : n;
    }();
    return wanted;
}

// Mesh pipelines (agent mesh): binds the translated mesh pipeline and root state on list `self`, then
// calls indirect_issue(self, list) and returns true when it issued. Null until mesh provides one.
using IndirectMeshHook = bool (*)(Wrapper* self, ID3D12GraphicsCommandList* list);
IndirectMeshHook indirect_mesh_hook = nullptr;
// LOCAL meshrt (d3d12_mesh.inl): a DispatchMesh call that cannot be issued is refused before it is staged.
bool mesh_prepare(Wrapper* self, ID3D12GraphicsCommandList* list);
void mesh_trace_call(Wrapper* self, bool drawn, const Translation* tr, const char* why);  // LOCAL meshrt: traced frame only

// The private argument ring: default heap, rests in COMMON between calls. LOCAL memcut: a chain of chunks recycled
// only after the lists that used them executed and their fences passed (d3d12_upload_ring.inl, indirect instance;
// X2W_INDIRECT_RING=legacy = the old 16 MiB ring). The chunk is per call: IndirectPending::ring.
constexpr u64 indirect_ring_max_take = u64{1} << 20;  // one call's record + count, as before (a sixteenth of 16 MiB)
// The chunk holding `bytes` (whole 256-byte slots) at *offset, or nullptr when refused (too large, or the cap).
ID3D12Resource* indirect_ring_take(u64 bytes, u64* offset)
{
    return bytes && bytes <= indirect_ring_max_take ? reserve_indirect_ring(bytes, offset) : nullptr;
}

// ---- Argument guard ----------------------------------------------------------------------------
//
// GPU-written records may be garbage while the shaders that produce them are wrong or untranslated, and
// a Dispatch with huge group counts hangs the GPU (device removed). Every record copied into the ring is
// checked ON THE GPU by one tiny compute pass before ExecuteIndirect reads it; a record over the bounds
// is zeroed (a zero-group dispatch / zero-count draw does nothing) and counted in a CPU-visible counter
// that report_indirect reads once per report. No CPU read-back per call.
//   Dispatch / DispatchMesh {x, y, z}: each <= X2W_INDIRECT_MAX_GROUPS (default 65535, the desktop limit)
//     and x*y*z <= X2W_INDIRECT_MAX_GROUP_PRODUCT (default 2^24; DispatchMesh also <= 2^22, the desktop limit).
//   Draw {vertices, instances, ..} / DrawIndexed {indices, instances, ..}: count <= X2W_INDIRECT_MAX_VERTICES
//     (default 2^24), instances <= X2W_INDIRECT_MAX_INSTANCES (default 2^16), count*instances <=
//     X2W_INDIRECT_MAX_DRAW_PRODUCT (default 2^26).
// X2W_INDIRECT_GUARD=0 executes unguarded (debugging only). When the guard cannot be created, calls are
// skipped ("no-guard"), never executed unguarded.
// Not covered: a record within the bounds can still make a shader loop or fault (wrong root data,
// out-of-range addresses); start offsets (StartIndex, BaseVertex, StartInstance) are not checked
// (index fetches are bounds-checked by the desktop); the count dword is not checked (ExecuteIndirect
// already takes min(count, max count)).
UINT indirect_env_u32(const char* name, UINT fallback)
{
    char value[16]{};
    if (!GetEnvironmentVariableA(name, value, sizeof(value)) || !value[0]) return fallback;
    const unsigned long long n = std::strtoull(value, nullptr, 0);
    return n && n <= 0xffffffffull ? static_cast<UINT>(n) : fallback;
}
struct IndirectBounds {
    UINT groups, group_product, mesh_product, vertices, instances, draw_product;
};
const IndirectBounds& indirect_bounds()
{
    static const IndirectBounds b = [] {
        IndirectBounds v{};
        v.groups = indirect_env_u32("X2W_INDIRECT_MAX_GROUPS", 65535);
        v.group_product = indirect_env_u32("X2W_INDIRECT_MAX_GROUP_PRODUCT", 1u << 24);
        v.mesh_product = v.group_product < (1u << 22) ? v.group_product : (1u << 22);
        v.vertices = indirect_env_u32("X2W_INDIRECT_MAX_VERTICES", 1u << 24);
        v.instances = indirect_env_u32("X2W_INDIRECT_MAX_INSTANCES", 1u << 16);
        v.draw_product = indirect_env_u32("X2W_INDIRECT_MAX_DRAW_PRODUCT", 1u << 26);
        return v;
    }();
    return b;
}
bool indirect_guard_wanted()
{
    static const bool wanted = !indirect_env_zero("X2W_INDIRECT_GUARD");
    return wanted;
}

constexpr char indirect_guard_source[] = R"(
RWByteAddressBuffer ring : register(u0);
RWByteAddressBuffer zeroed : register(u1);
cbuffer Bounds : register(b0) { uint stride; uint records; uint kind; uint counter; uint each; uint product; uint each2; uint unused; };
[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= records) return;
    const uint at = id.x * stride;
    bool bad = false;
    if (kind == 0) {
        const uint3 g = ring.Load3(at);
        bad = g.x > each || g.y > each || g.z > each;
        if (!bad && g.x != 0 && g.y > product / g.x) bad = true;
        const uint xy = g.x * g.y;
        if (!bad && xy != 0 && g.z > product / xy) bad = true;
        if (bad) ring.Store3(at, uint3(0, 0, 0));
    } else {
        const uint2 d = ring.Load2(at);
        bad = d.x > each || d.y > each2 || (d.x != 0 && d.y > product / d.x);
        if (bad) ring.Store2(at, uint2(0, 0));
    }
    if (bad) {
        uint previous;
        zeroed.InterlockedAdd(counter * 4, 1, previous);
    }
}
)";
ID3D12RootSignature* indirect_guard_root;
ID3D12PipelineState* indirect_guard_pso;
ID3D12Resource* indirect_guard_counter;              // 8 dwords: records zeroed, by counter type
const volatile std::uint32_t* indirect_guard_words;  // mapped (CPU-visible custom heap), or null
std::uint32_t indirect_guard_reported[itype_types];
bool ensure_indirect_guard()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static bool failed;
    if (indirect_guard_pso) return true;
    AcquireSRWLockExclusive(&lock);
    if (!indirect_guard_pso && !failed) {
        ID3D12Device* device = device_of(the_device);
        ID3DBlob *code = nullptr, *errors = nullptr, *serialized = nullptr;
        HRESULT hr = D3DCompile(indirect_guard_source, sizeof(indirect_guard_source) - 1, "indirect_guard", nullptr, nullptr, "cs",
                                "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) && errors) log("x2w-indirect guard compile: %.400s", static_cast<const char*>(errors->GetBufferPointer()));
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.Num32BitValues = 8;
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC root_desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
        ID3D12RootSignature* root = nullptr;
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root;
            desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        }
        // The counter: a CPU-visible custom heap (write-back, system memory) so a report reads it without a
        // copy; if the adapter refuses, a default-heap counter (the guard still runs, counts are not shown).
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 64;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* counter = nullptr;
        void* mapped = nullptr;
        HRESULT counter_hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            const D3D12_HEAP_PROPERTIES visible{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
            counter_hr = device->CreateCommittedResource(&visible, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&counter));
            if (SUCCEEDED(counter_hr) && SUCCEEDED(counter->Map(0, nullptr, &mapped))) {
                std::memset(mapped, 0, 64);
            } else {
                mapped = nullptr;
                if (counter) counter->Release(), counter = nullptr;
                const D3D12_HEAP_PROPERTIES local{D3D12_HEAP_TYPE_DEFAULT};
                hr = device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&counter));
            }
        }
        const IndirectBounds& b = indirect_bounds();
        log("x2w-indirect argument guard -> 0x%08lx (counter %s 0x%08lx): groups <= %u each, product <= %u (mesh %u); draws count <= %u, "
            "instances <= %u, product <= %u",
            static_cast<unsigned long>(hr), mapped ? "visible" : "not visible", static_cast<unsigned long>(counter_hr), b.groups,
            b.group_product, b.mesh_product, b.vertices, b.instances, b.draw_product);
        if (code) code->Release();
        if (errors) errors->Release();
        if (serialized) serialized->Release();
        if (SUCCEEDED(hr) && pso && root && counter) {
            indirect_guard_root = root;
            indirect_guard_counter = counter;
            indirect_guard_words = static_cast<const volatile std::uint32_t*>(mapped);
            indirect_guard_pso = pso;
        } else {
            if (pso) pso->Release();
            if (root) root->Release();
            if (counter) counter->Release();
            failed = true;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return indirect_guard_pso != nullptr;
}
// Records the guard pass over `records` records at ring offset `slot` (ring in UNORDERED_ACCESS).
void indirect_guard(ID3D12GraphicsCommandList* list, ID3D12Resource* ring, int type, u64 slot, UINT stride, UINT records)
{
    const IndirectBounds& b = indirect_bounds();
    const bool groups = type == itype_dispatch || type == itype_mesh;
    const UINT constants[8] = {stride, records, groups ? 0u : 1u, static_cast<UINT>(type),
                               groups ? b.groups : b.vertices, groups ? (type == itype_mesh ? b.mesh_product : b.group_product) : b.draw_product,
                               b.instances, 0};
    list->SetPipelineState(indirect_guard_pso);
    list->SetComputeRootSignature(indirect_guard_root);
    list->SetComputeRootUnorderedAccessView(0, ring->GetGPUVirtualAddress() + slot);
    list->SetComputeRootUnorderedAccessView(1, indirect_guard_counter->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstants(2, 8, constants, 0);
    // LOCAL: the guard pass is a Dispatch op in DRED too; record it so the report's Dispatch op count matches.
    const u64 issue_key = cx_issue_key;
    const char issue_path = cx_issue_path;
    cx_issue_key = 0;
    cx_issue_path = 'g';
    note_issued_dispatch(list, (records + 63) / 64, 1, 1, false);
    cx_issue_key = issue_key;
    cx_issue_path = issue_path;
    list->Dispatch((records + 63) / 64, 1, 1);
}

// Read-back of executed records (X2W_INDIRECT_READBACK): 32-byte slots, [0] marker copied from the upload
// ring after the record, [1..7] the first 28 bytes of the record as ExecuteIndirect read it.
constexpr UINT indirect_readback_slots = 4096;
ID3D12Resource* indirect_readback;
const std::uint32_t* indirect_readback_words;
volatile LONG indirect_readback_used;
LONG indirect_readback_generation = 1;
unsigned char indirect_readback_type[indirect_readback_slots];
bool ensure_indirect_readback()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static bool failed;
    if (indirect_readback_words) return true;
    AcquireSRWLockExclusive(&lock);
    if (!indirect_readback_words && !failed) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = u64{indirect_readback_slots} * 32;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* created = nullptr;
        void* mapped = nullptr;
        HRESULT hr = device_of(the_device)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    nullptr, IID_PPV_ARGS(&created));
        if (SUCCEEDED(hr)) hr = created->Map(0, nullptr, &mapped);
        log("x2w-indirect read-back buffer (%u records) -> 0x%08lx", indirect_readback_slots, static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) {
            indirect_readback = created;
            indirect_readback_words = static_cast<const std::uint32_t*>(mapped);
        } else {
            failed = true;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return indirect_readback_words != nullptr;
}

// The window and window offset of [offset, offset + bytes) of a buffer wrapper's title memory. The
// lookup is keyed on the wrapper's whole range (cached per frame by desktop_range), so the thousands
// of records a frame do not each miss the cache.
bool indirect_window(Wrapper* resource, u64 offset, u64 bytes, ID3D12Resource** window, u64* at)
{
    if (!resource || resource->own().kind != resource_kind || !resource->own().title_memory) return false;
    const u64 base = reinterpret_cast<u64>(resource->own().title_memory);
    u64 width = 0;
    if (auto* real = static_cast<ID3D12Resource*>(resource->own().real)) {
        const D3D12_RESOURCE_DESC desc = real->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) width = desc.Width;
    }
    if (width && offset <= width && bytes <= width - offset && desktop_range(base, width, window, at)) {
        *at += offset;
        return true;
    }
    return desktop_range(base + offset, bytes, window, at);
}

// The call being executed on this thread, handed to the dispatch / draw / mesh path, which binds the
// pipeline and then calls indirect_issue instead of its direct Dispatch / Draw.
struct IndirectPending {
    Wrapper* list;
    int type;
    ID3D12CommandSignature* signature;
    UINT stride, max_count;
    ID3D12Resource* args_window;
    u64 args_at;
    ID3D12Resource* count_window;  // null: no count buffer
    u64 count_at;
    bool issued;
    IndirectSkip refused;          // set by an issue path that refused the call
    // Set by indirect_stage: the record (and count) sit in the ring at `slot`, checked by the guard; the
    // ring is in `ring_state` on this list until indirect_issue / indirect_unstage returns it to COMMON.
    bool staged;
    ID3D12Resource* ring;  // LOCAL memcut: the argument ring chunk holding the record
    u64 slot, ring_count_at;
    D3D12_RESOURCE_STATES ring_state;
};
thread_local IndirectPending* indirect_current;
// LOCAL meshrt: in a traced frame, the DispatchMesh call's record in title memory (CPU view at record time) for the
// x2w-frame mesh line (d3d12_mesh.inl mesh_trace_call); null otherwise.
thread_local const unsigned char* mesh_trace_record;
thread_local UINT mesh_trace_stride, mesh_trace_max;
// Also outside the traced frame while an X2W_MESH_DUMP_INPUTS key is not dumped yet or X2W_MESH_STATS_FRAMES collects
// (d3d12_mesh.inl): both select by the record's group counts.
bool mesh_fs_collecting();
bool mesh_record_wanted() { return trace_frame_active || mesh_dump_pending() || mesh_fs_collecting(); }

bool indirect_pending(Wrapper* self) { return indirect_current && !indirect_current->issued && indirect_current->list == self; }

void indirect_transition(D3D12_RESOURCE_BARRIER& b, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

// Records, BEFORE the dispatch / draw / mesh path binds its pipeline and root state (the guard pass binds
// its own compute pipeline): the copy of the record into the ring, the guard pass over it and the
// barriers that leave the ring readable as indirect arguments. A forwarded title pipeline (pipeline_real)
// is bound again afterwards. Returns false (p->refused set) when nothing was recorded.
bool indirect_stage(Wrapper* self, ID3D12GraphicsCommandList* list, IndirectPending* p)
{
    const bool guard = indirect_guard_wanted();
    if (guard && !ensure_indirect_guard()) {
        p->refused = skip_no_guard;
        return false;
    }
    const u64 record_bytes = u64{p->stride} * p->max_count;
    const u64 count_at = (record_bytes + 15) & ~u64{15};
    u64 slot = 0;
    ID3D12Resource* const indirect_ring = indirect_ring_take(count_at + (p->count_window ? 16 : 0), &slot);
    if (!indirect_ring) {
        p->refused = skip_no_ring;
        return false;
    }
    const bool readback = indirect_readback_wanted() && indirect_readback_used < indirect_readback_wanted() && ensure_indirect_readback();
    D3D12_RESOURCE_BARRIER b[3];
    UINT n = 0;
    indirect_transition(b[n++], p->args_window, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (p->count_window && p->count_window != p->args_window)
        indirect_transition(b[n++], p->count_window, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    indirect_transition(b[n++], indirect_ring, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(n, b);
    list->CopyBufferRegion(indirect_ring, slot, p->args_window, p->args_at, record_bytes);
    if (p->count_window) list->CopyBufferRegion(indirect_ring, slot + count_at, p->count_window, p->count_at, 4);
    const D3D12_RESOURCE_STATES ring_state = readback ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE
                                                      : D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    for (UINT i = 0; i < n; ++i) std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
    b[n - 1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[n - 1].Transition.StateAfter = guard ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : ring_state;
    list->ResourceBarrier(n, b);
    if (guard) {
        indirect_guard(list, indirect_ring, p->type, slot, p->stride, p->max_count);
        D3D12_RESOURCE_BARRIER after;
        indirect_transition(after, indirect_ring, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ring_state);
        list->ResourceBarrier(1, &after);
        const Pass& pass = pass_of(self);
        if (pass.pipeline_real && pass.pipeline) list->SetPipelineState(static_cast<ID3D12PipelineState*>(pass.pipeline->own().real));
    }
    p->staged = true;
    p->ring = indirect_ring;
    p->slot = slot;
    p->ring_count_at = count_at;
    p->ring_state = ring_state;
    return true;
}
// A staged call that no path issued: the ring goes back to COMMON on this list.
void indirect_unstage(ID3D12GraphicsCommandList* list, IndirectPending* p)
{
    if (!p->staged) return;
    D3D12_RESOURCE_BARRIER b;
    indirect_transition(b, p->ring, p->ring_state, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &b);
    p->staged = false;
}

// Records the ExecuteIndirect for the pending (staged) call. The pipeline and root state must already be
// bound on `list`.
bool indirect_issue(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    IndirectPending* p = indirect_current;
    if (!p || p->issued || !p->staged || p->list != self || !list) return false;
    const u64 record_bytes = u64{p->stride} * p->max_count;
    const u64 slot = p->slot, count_at = p->ring_count_at;
    ID3D12Resource* const indirect_ring = p->ring;
    const D3D12_RESOURCE_STATES ring_state = p->ring_state;
    const bool readback = (ring_state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
    list->ExecuteIndirect(p->signature, p->max_count, indirect_ring, slot, p->count_window ? indirect_ring : nullptr,
                          p->count_window ? slot + count_at : 0);
    if (readback) {
        const LONG index = InterlockedIncrement(&indirect_readback_used) - 1;
        u64 marker_at = 0;
        ID3D12Resource* marker_chunk = nullptr;
        auto* marker = index < indirect_readback_wanted() ? reserve_upload_ring(4, &marker_at, &marker_chunk, nullptr) : nullptr;
        if (marker) {
            const std::uint32_t value = 0x1d000000u | (static_cast<std::uint32_t>(indirect_readback_generation & 0xff) << 16) |
                                        static_cast<std::uint32_t>(index);
            std::memcpy(marker, &value, 4);
            indirect_readback_type[index] = static_cast<unsigned char>(p->type);
            const u64 dest = u64{static_cast<UINT>(index)} * 32;
            list->CopyBufferRegion(indirect_readback, dest + 4, indirect_ring, slot, record_bytes < 28 ? record_bytes : 28);
            list->CopyBufferRegion(indirect_readback, dest, marker_chunk, marker_at, 4);
        }
    }
    D3D12_RESOURCE_BARRIER rest;
    indirect_transition(rest, indirect_ring, ring_state, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &rest);
    p->staged = false;
    p->issued = true;
    return true;
}

// Hook for the final Dispatch of both dispatch paths (d3d12_compute.inl execute_translated,
// d3d12_xs_device.cpp execute_dispatch): the indirect call when one is pending on this list.
void indirect_dispatch(Wrapper* self, ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
    if (indirect_pending(self)) {
        note_issued_dispatch(list, x, y, z, true);  // LOCAL: for the DRED report (d3d12_xs_device.cpp)
        indirect_issue(self, list);
        return;
    }
    note_issued_dispatch(list, x, y, z, false);
    list->Dispatch(x, y, z);
}

// Hook for the issue point of draw_translated (d3d12_draw.inl): the whole index buffer view goes to
// the upload ring (as the direct path copies its index range), then the indirect call.
bool indirect_draw_issue(Wrapper* self, ID3D12GraphicsCommandList* list, bool indexed)
{
    if (!indirect_pending(self)) return false;
    if (indexed) {
        const Pass& pass = pass_of(self);
        const std::size_t bytes = pass.ib.SizeInBytes < (16u << 20) ? pass.ib.SizeInBytes : (16u << 20);
        u64 offset = 0;
        ID3D12Resource* chunk = nullptr;
        if (!pass.ib.BufferLocation || !bytes || readable_window(pass.ib.BufferLocation, bytes) < bytes ||
            !frame_copy(pass.ib.BufferLocation, bytes, &offset, &chunk)) {
            indirect_current->refused = skip_unresolved_buffer;
            return false;
        }
        const D3D12_INDEX_BUFFER_VIEW view{chunk->GetGPUVirtualAddress() + offset, static_cast<UINT>(bytes),
                                           pass.ib.Format == DXGI_FORMAT_R16_UINT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&view);
    }
    return indirect_issue(self, list);
}

u64 compute_key_of(Wrapper* self);
bool execute_dispatch(Wrapper* self, UINT x, UINT y, UINT z, u64 key);

void indirect_announce()
{
    static LONG once;
    if (InterlockedExchange(&once, 1)) return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    const HRESULT hr = device_of(the_device)->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    log("x2w-indirect execution: dispatch %s, draw %s, mesh %s (hook %s), guard %s, read-back %ld records per report; MeshShaderTier %d (0x%08lx)",
        indirect_type_enabled(itype_dispatch) ? "on" : "off", indirect_type_enabled(itype_draw) ? "on" : "off",
        indirect_type_enabled(itype_mesh) ? "on" : "off", indirect_mesh_hook ? "set" : "absent",
        indirect_guard_wanted() ? "on" : "OFF (X2W_INDIRECT_GUARD=0)", indirect_readback_wanted(),
        SUCCEEDED(hr) ? static_cast<int>(options7.MeshShaderTier) : -1, static_cast<unsigned long>(hr));
}

// Read-back of a call that is NOT executed (no path or no pipeline): the record as the GPU sees it at
// that point of the list, copied straight from the window. Slot type = counter type | 0x80.
void indirect_probe(ID3D12GraphicsCommandList* list, int type, ID3D12Resource* window, u64 at, u64 bytes)
{
    if (!indirect_readback_wanted() || indirect_readback_used >= indirect_readback_wanted() || !ensure_indirect_readback()) return;
    const LONG index = InterlockedIncrement(&indirect_readback_used) - 1;
    u64 marker_at = 0;
    ID3D12Resource* marker_chunk = nullptr;
    auto* marker = index < indirect_readback_wanted() ? reserve_upload_ring(4, &marker_at, &marker_chunk, nullptr) : nullptr;
    if (!marker) return;
    const std::uint32_t value = 0x1d000000u | (static_cast<std::uint32_t>(indirect_readback_generation & 0xff) << 16) |
                                static_cast<std::uint32_t>(index);
    std::memcpy(marker, &value, 4);
    indirect_readback_type[index] = static_cast<unsigned char>(type | 0x80);
    const u64 dest = u64{static_cast<UINT>(index)} * 32;
    D3D12_RESOURCE_BARRIER b;
    indirect_transition(b, window, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(indirect_readback, dest + 4, window, at, bytes < 28 ? bytes : 28);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(indirect_readback, dest, marker_chunk, marker_at, 4);
}

void indirect_run(Wrapper* self, ID3D12GraphicsCommandList* list, int type, Wrapper* signature, UINT stride, UINT max_count,
                  Wrapper* arguments, u64 offset, Wrapper* count_resource, u64 count_offset)
{
    indirect_announce();
    if (type == itype_other || (type == itype_mesh && !indirect_mesh_hook)) {
        ID3D12Resource* window = nullptr;
        u64 at = 0;
        if (indirect_readback_wanted() && indirect_window(arguments, offset, u64{stride} * max_count, &window, &at))
            indirect_probe(list, type, window, at, u64{stride} * max_count);
        return indirect_skip(type, skip_not_implemented);
    }
    if (!indirect_type_enabled(type)) return indirect_skip(type, skip_switched_off);
    auto* desktop_signature = static_cast<ID3D12CommandSignature*>(signature->own().real);
    if (!desktop_signature) return indirect_skip(type, skip_unknown_signature);
    if (type == itype_draw || type == itype_indexed) {
        if (pass_of(self).topology == 0x11) return indirect_skip(type, skip_topology);
    }
    IndirectPending p{};
    p.list = self;
    p.type = type;
    p.signature = desktop_signature;
    p.stride = stride;
    p.max_count = max_count;
    p.refused = skip_no_pipeline;
    if (!indirect_window(arguments, offset, u64{stride} * max_count, &p.args_window, &p.args_at) ||
        (count_resource && !indirect_window(count_resource, count_offset, 4, &p.count_window, &p.count_at)))
        return indirect_skip(type, skip_unresolved_buffer);
    mesh_trace_record = nullptr;
    if (type == itype_mesh && mesh_record_wanted()) {  // LOCAL meshrt: traced frame, pending dump, frame statistics
        mesh_trace_record = arguments->own().kind == resource_kind && arguments->own().title_memory
                                ? static_cast<const unsigned char*>(arguments->own().title_memory) + offset : nullptr;
        mesh_trace_stride = stride;
        mesh_trace_max = max_count;
    }
    if (type == itype_mesh && !mesh_prepare(self, list)) return indirect_skip(type, skip_no_pipeline);  // LOCAL meshrt: before staging
    if (!indirect_stage(self, list, &p)) {
        if (type == itype_mesh && trace_frame_active) mesh_trace_call(self, false, nullptr, indirect_skip_names[p.refused]);  // LOCAL meshrt
        return indirect_skip(type, p.refused);
    }
    indirect_current = &p;
    if (type == itype_dispatch) execute_dispatch(self, 0, 0, 0, compute_key_of(self));
    else if (type == itype_mesh) indirect_mesh_hook(self, list);
    else draw_translated(self, type == itype_indexed, 0, 0, 0, 0, 0);
    indirect_current = nullptr;
    indirect_unstage(list, &p);
    if (p.issued) {
        InterlockedIncrement(&indirect_counts.executed[type]);
    } else {
        indirect_probe(list, type, p.args_window, p.args_at, u64{stride} * max_count);
        indirect_skip(type, p.refused);
    }
}

// List slot 61 (called from list_execute_indirect before its frame trace).
void indirect_execute(Wrapper* self, void* signature, UINT max_count, Wrapper* arguments, u64 offset, Wrapper* count_resource,
                      u64 count_offset)
{
    UINT stride = 0, argument_type = ~0u;
    const bool known = indirect_signature(signature, &stride, &argument_type);
    const int type = known ? indirect_type_of(argument_type) : itype_other;
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
    auto* list = recording(self);
    if (!list) return indirect_skip(type, skip_not_recording);
    if (!known) return indirect_skip(type, skip_unknown_signature);
    if (!max_count) return indirect_skip(type, skip_zero_count);
    indirect_run(self, list, type, static_cast<Wrapper*>(signature), stride, max_count, arguments, offset, count_resource, count_offset);
}

// The records read back since the last report: how many the GPU copied (marker present), how many were
// non-zero, how many break the v1 draw assumption (entry values 0: indexed BaseVertex and StartInstance,
// draw StartInstance), and up to 6 samples. Then a new generation starts.
void report_indirect_readback()
{
    if (!indirect_readback_wanted() || !indirect_readback_words) return;
    const LONG used = InterlockedExchange(&indirect_readback_used, indirect_readback_wanted());  // no new copies while reading
    const LONG count = used < indirect_readback_wanted() ? used : indirect_readback_wanted();
    // [0] executed calls (record as ExecuteIndirect read it), [1] calls not executed (record at that point of the list)
    LONG arrived[2] = {}, records[2][itype_types] = {}, nonzero[2][itype_types] = {}, broken = 0;
    char samples[640] = "";
    int at = 0, shown = 0;
    for (LONG i = 0; i < count; ++i) {
        const std::uint32_t* w = indirect_readback_words + u64{static_cast<UINT>(i)} * 8;
        const std::uint32_t expected = 0x1d000000u | (static_cast<std::uint32_t>(indirect_readback_generation & 0xff) << 16) |
                                       static_cast<std::uint32_t>(i);
        if (w[0] != expected) continue;
        const int probed = indirect_readback_type[i] & 0x80 ? 1 : 0;
        const int type = (indirect_readback_type[i] & 0x7f) < itype_types ? (indirect_readback_type[i] & 0x7f) : itype_other;
        ++arrived[probed];
        ++records[probed][type];
        const bool any = w[1] | w[2] | w[3] | w[4] | w[5];
        if (any) ++nonzero[probed][type];
        if (!probed && ((type == itype_indexed && (w[4] || w[5])) || (type == itype_draw && w[4]))) ++broken;
        if (any && shown < 8 && at < static_cast<int>(sizeof(samples)) - 80) {
            at += std::snprintf(samples + at, sizeof(samples) - at, "%s%s%s %u %u %u %u %u", shown ? "; " : "", indirect_type_names[type],
                                probed ? "(not run)" : "", w[1], w[2], w[3], w[4], w[5]);
            ++shown;
        }
    }
    const auto sum = [](const LONG* v) { return v[0] + v[1] + v[2] + v[3] + v[4]; };
    log("x2w-indirect read-back: %ld slots; executed: %ld arrived, non-zero %ld (draw %ld/%ld, indexed %ld/%ld, dispatch %ld/%ld, mesh %ld/%ld, "
        "other %ld/%ld); not executed: %ld arrived, non-zero %ld (draw %ld/%ld, indexed %ld/%ld, dispatch %ld/%ld, mesh %ld/%ld, other %ld/%ld); "
        "executed draws with base vertex / start instance not 0: %ld; samples: %s",
        count, arrived[0], sum(nonzero[0]), nonzero[0][0], records[0][0], nonzero[0][1], records[0][1], nonzero[0][2], records[0][2],
        nonzero[0][3], records[0][3], nonzero[0][4], records[0][4], arrived[1], sum(nonzero[1]), nonzero[1][0], records[1][0],
        nonzero[1][1], records[1][1], nonzero[1][2], records[1][2], nonzero[1][3], records[1][3], nonzero[1][4], records[1][4], broken,
        shown ? samples : "none");
    ++indirect_readback_generation;
    InterlockedExchange(&indirect_readback_used, 0);
}

// Every 300 frames (queue_present): one line with the calls by type, the executed calls and the
// skipped calls by reason (each reason with its non-zero types). Counters restart after each line.
void report_indirect(LONG frames)
{
    IndirectCounters c{};
    for (int t = 0; t < itype_types; ++t) {
        c.calls[t] = InterlockedExchange(&indirect_counts.calls[t], 0);
        c.executed[t] = InterlockedExchange(&indirect_counts.executed[t], 0);
        for (int r = 0; r < skip_reasons; ++r) c.skipped[r][t] = InterlockedExchange(&indirect_counts.skipped[r][t], 0);
    }
    c.with_count_buffer = InterlockedExchange(&indirect_counts.with_count_buffer, 0);
    c.max_count_above_one = InterlockedExchange(&indirect_counts.max_count_above_one, 0);
    c.max_count_max = InterlockedExchange(&indirect_counts.max_count_max, 0);
    LONG calls = 0, executed = 0;
    for (int t = 0; t < itype_types; ++t) calls += c.calls[t], executed += c.executed[t];
    char skipped[768] = "";
    int at = 0;
    for (int r = 0; r < skip_reasons; ++r) {
        LONG total = 0;
        for (int t = 0; t < itype_types; ++t) total += c.skipped[r][t];
        if (!total || at >= static_cast<int>(sizeof(skipped)) - 96) continue;
        at += std::snprintf(skipped + at, sizeof(skipped) - at, "%s%s %ld [", at ? ", " : "", indirect_skip_names[r], total);
        bool first = true;
        for (int t = 0; t < itype_types; ++t)
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
    // Records the guard zeroed since the last report (the GPU adds to the counter; values of lists still in
    // flight arrive in a later report).
    if (indirect_guard_pso && executed) {
        if (indirect_guard_words) {
            std::uint32_t zeroed[itype_types];
            for (int t = 0; t < itype_types; ++t) {
                const std::uint32_t now = indirect_guard_words[t];
                zeroed[t] = now - indirect_guard_reported[t];
                indirect_guard_reported[t] = now;
            }
            log("x2w-indirect guard: zeroed %u records over the bounds (draw %u, indexed %u, dispatch %u, mesh %u)",
                zeroed[0] + zeroed[1] + zeroed[2] + zeroed[3], zeroed[0], zeroed[1], zeroed[2], zeroed[3]);
        } else {
            log("x2w-indirect guard: active, zeroed count not visible (default-heap counter)");
        }
    }
    report_indirect_readback();
}
