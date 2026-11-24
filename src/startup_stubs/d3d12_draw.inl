// Local experiment (never upstream): draws through translated pipelines.
// Included inside d3d12_xs_device.cpp's anonymous namespace, after the Pass helpers.
//
// A machine-code pipeline is looked up by the hash of its two code blobs and its root signature in
// X2W_SHADER_CACHE: <key>.txt (manifest), <key>.vs.cso and <key>.ps.cso, produced offline by
// out/gamerun/gfx2hlsl.py. A miss writes req_<key>.{a,b,root} there for the next offline pass.
// The translated shaders use their own root signature: [0] 32-bit constants ("entry" values such
// as the base vertex), [1] one table of SRVs. Each draw copies what the title bound (vertex streams,
// buffers named by the hardware descriptors in its tables, textures behind view tokens) into an
// upload ring / a private descriptor heap.

struct Translation {
    struct Srv { int kind, source, a, b; };  // kind 0 buffer, 1 2D texture, 3 3D texture; source 0 table(param a, byte b), 1 vertex stream a, 2 root parameter a
    struct Ud { int kind, a, b; };           // 0 rect list, 1 base vertex, 2 start instance, 3 root constant(param a, word b), 4 stride of srv a
    int state = 0;                           // 1 ready, 2 unavailable
    std::vector<Srv> srvs;
    std::vector<Ud> uds;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::uint32_t compare_samplers = 0;  // LOCAL: bit i set when sampler i is a SamplerComparisonState
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12RootSignature* root = nullptr;
    std::unordered_map<u64, ID3D12PipelineState*> pipelines;  // by target formats
    u64 key = 0;
    LONG logged_draws = 0, logged_bindings = 0, logged_large = 0;  // diagnostics budget per translation
};

// Per title pipeline: its translation and the blend state found in its state block (if any).
struct BoundPipeline {
    Translation* translation = nullptr;
    bool has_blend = false;
    D3D12_BLEND_DESC blend{};
    std::unordered_map<u64, ID3D12PipelineState*> pipelines;  // by target formats
};

// Looks for a desktop-layout D3D12_BLEND_DESC inside a state block: two flags, then eight render
// target entries whose fields are all inside their enumerations.
bool find_blend(const std::vector<unsigned char>& block, D3D12_BLEND_DESC* out, std::size_t* where)
{
    for (std::size_t at = 0; at + sizeof(D3D12_BLEND_DESC) <= block.size(); at += 4) {
        D3D12_BLEND_DESC d;
        std::memcpy(&d, block.data() + at, sizeof(d));
        bool ok = (d.AlphaToCoverageEnable == 0 || d.AlphaToCoverageEnable == 1) && (d.IndependentBlendEnable == 0 || d.IndependentBlendEnable == 1);
        for (int i = 0; ok && i < 8; ++i) {
            const auto& t = d.RenderTarget[i];
            ok = (t.BlendEnable == 0 || t.BlendEnable == 1) && (t.LogicOpEnable == 0 || t.LogicOpEnable == 1) && t.SrcBlend >= 1 &&
                 t.SrcBlend <= 19 && t.DestBlend >= 1 && t.DestBlend <= 19 && t.BlendOp >= 1 && t.BlendOp <= 5 && t.SrcBlendAlpha >= 1 &&
                 t.SrcBlendAlpha <= 19 && t.DestBlendAlpha >= 1 && t.DestBlendAlpha <= 19 && t.BlendOpAlpha >= 1 && t.BlendOpAlpha <= 5 &&
                 static_cast<unsigned>(t.LogicOp) <= 15 && t.RenderTargetWriteMask <= 15;
        }
        if (ok) {
            *out = d;
            *where = at;
            return true;
        }
    }
    return false;
}

SRWLOCK draw_lock = SRWLOCK_INIT;
std::unordered_map<const Wrapper*, BoundPipeline*>& translation_by_pipeline() { static std::unordered_map<const Wrapper*, BoundPipeline*> m; return m; }
std::unordered_map<u64, Translation*>& translation_by_key() { static std::unordered_map<u64, Translation*> m; return m; }
LONG translated_draws, untranslated_draws, translation_ready, translation_missing, null_bindings;
LONG frame_number;

u64 fnv_bytes(const void* data, std::size_t bytes, u64 hash = 1469598103934665603ull)
{
    for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ static_cast<const unsigned char*>(data)[i]) * 1099511628211ull;
    return hash;
}

bool cache_path(wchar_t* out, const wchar_t* prefix, u64 key, const wchar_t* extension)
{
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"X2W_SHADER_CACHE", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    swprintf(out, MAX_PATH, L"%s\\%s%016llx%s", dir, prefix, key, extension);
    return true;
}

bool read_text(const wchar_t* path, std::string& out)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[8192];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) out.append(buffer, got);
    CloseHandle(file);
    return true;
}

D3D12_STATIC_SAMPLER_DESC decode_sampler(const std::array<std::uint32_t, 4>& words, UINT shader_register, bool compare = false)
{
    static const D3D12_TEXTURE_ADDRESS_MODE modes[8] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
    const UINT mag = (words[2] >> 20) & 3, min = (words[2] >> 22) & 3, mip = (words[2] >> 26) & 3;
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = (mag >= 2 || min >= 2) ? D3D12_FILTER_ANISOTROPIC
        : D3D12_ENCODE_BASIC_FILTER(min & 1, mag & 1, mip == 2 ? 1 : 0, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    s.AddressU = modes[words[0] & 7];
    s.AddressV = modes[(words[0] >> 3) & 7];
    s.AddressW = modes[(words[0] >> 6) & 7];
    s.MaxAnisotropy = 1u << ((words[0] >> 9) & 7);
    if (s.MaxAnisotropy > 16) s.MaxAnisotropy = 16;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (compare) {
        // S# word 0 bits 14:12 hold the depth compare function (never, less, ..., always); D3D12's enum is that + 1.
        s.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(((words[0] >> 12) & 7) + 1);
        s.Filter = D3D12_ENCODE_BASIC_FILTER(min & 1, mag & 1, mip == 2 ? 1 : 0, D3D12_FILTER_REDUCTION_TYPE_COMPARISON);
    }
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = shader_register;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

void load_translation(Translation& tr, const void* a, std::size_t a_bytes, const void* b, std::size_t b_bytes,
                      const std::vector<unsigned char>& root_blob)
{
    tr.state = 2;
    wchar_t path[MAX_PATH];
    std::string manifest;
    if (!cache_path(path, L"", tr.key, L".txt")) return;
    if (!read_text(path, manifest)) {
        InterlockedIncrement(&translation_missing);
        if (cache_path(path, L"req_", tr.key, L".a") && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            write_file(path, a, a_bytes);
            cache_path(path, L"req_", tr.key, L".b");
            write_file(path, b, b_bytes);
            cache_path(path, L"req_", tr.key, L".root");
            write_file(path, root_blob.data(), root_blob.size());
            static LONG logged;
            if (first_calls(logged, 12)) log("x2w-draw no translation for pipeline %016llx: request written", tr.key);
        }
        return;
    }
    for (std::size_t at = 0; at < manifest.size();) {
        std::size_t end = manifest.find('\n', at);
        if (end == std::string::npos) end = manifest.size();
        const std::string line = manifest.substr(at, end - at);
        at = end + 1;
        int i = 0, x = 0, y = 0;
        char kind[16] = "", source[16] = "";
        unsigned w[4] = {};
        if (std::sscanf(line.c_str(), "udsrc %d %15s %d %d", &i, source, &x, &y) >= 2) {
            Translation::Ud ud{-1, x, y};
            if (!std::strcmp(source, "rect")) ud.kind = 0;
            else if (!std::strcmp(source, "basevertex")) ud.kind = 1;
            else if (!std::strcmp(source, "startinstance")) ud.kind = 2;
            else if (!std::strcmp(source, "rootconst")) ud.kind = 3;
            else if (!std::strcmp(source, "stride")) ud.kind = 4;
            else if (!std::strcmp(source, "bufword3")) ud.kind = 5;  // LOCAL: word 3 of srv a's buffer descriptor
            if (static_cast<std::size_t>(i) == tr.uds.size() && ud.kind >= 0) tr.uds.push_back(ud);
        } else if (std::sscanf(line.c_str(), "srv %d %15s %15s %d %d", &i, kind, source, &x, &y) >= 4) {
            Translation::Srv srv{!std::strcmp(kind, "tex2d") ? 1 : !std::strcmp(kind, "tex2da") ? 2 : !std::strcmp(kind, "tex3d") ? 3 : !std::strcmp(kind, "tex1d") ? 4 : 0, !std::strcmp(source, "vb") ? 1 : !std::strcmp(source, "root") ? 2 : 0, x, y};
            if (static_cast<std::size_t>(i) == tr.srvs.size()) tr.srvs.push_back(srv);
        } else if (std::sscanf(line.c_str(), "smp %d %x %x %x %x", &i, &w[0], &w[1], &w[2], &w[3]) == 5) {
            tr.samplers.push_back({w[0], w[1], w[2], w[3]});
        } else if (std::sscanf(line.c_str(), "smpcmp %d", &i) == 1 && i >= 0 && i < 32) {
            tr.compare_samplers |= 1u << i;
        }
    }
    if (tr.uds.empty() || tr.uds.size() > 32 || tr.srvs.size() > 32) return;
    if (!cache_path(path, L"", tr.key, L".vs.cso") || FAILED(D3DReadFileToBlob(path, &tr.vs))) return;
    if (!cache_path(path, L"", tr.key, L".ps.cso") || FAILED(D3DReadFileToBlob(path, &tr.ps))) return;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = static_cast<UINT>(tr.uds.size());
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(tr.srvs.size()), 0, 0, 0};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    for (std::size_t i = 0; i < tr.samplers.size(); ++i) samplers.push_back(decode_sampler(tr.samplers[i], static_cast<UINT>(i), (tr.compare_samplers >> i) & 1));
    D3D12_ROOT_SIGNATURE_DESC desc{tr.srvs.empty() ? 1u : 2u, parameters, static_cast<UINT>(samplers.size()), samplers.data(),
                                   D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device_of(the_device)->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&tr.root));
    if (blob) blob->Release();
    if (errors) errors->Release();
    if (FAILED(hr)) {
        log("x2w-draw root signature for %016llx failed 0x%08lx", tr.key, static_cast<unsigned long>(hr));
        return;
    }
    tr.state = 1;
    InterlockedIncrement(&translation_ready);
    log("x2w-draw pipeline %016llx translated: %zu entry values, %zu resources, %zu samplers", tr.key, tr.uds.size(), tr.srvs.size(),
        tr.samplers.size());
}

// draw_lock held.
BoundPipeline* translation_for(Wrapper* pipeline)
{
    auto& by_pipeline = translation_by_pipeline();
    const auto known = by_pipeline.find(pipeline);
    if (known != by_pipeline.end()) return known->second;
    auto* bound = new BoundPipeline;
    Translation*& result = bound->translation;
    const PipelineInfo* info = pipeline_info(pipeline);
    const PipelineInfo* own = info;
    for (int depth = 0; info && info->path == 55 && info->base && depth < 8; ++depth) info = pipeline_info(info->base);
    {
        // The pipeline's own description first (a derived pipeline overrides state), then its base's state block.
        std::size_t where = 0;
        const char* from = "none";
        if (own && own != info && find_blend(own->desc, &bound->blend, &where)) from = "derived description";
        else if (info && find_blend(info->state_a, &bound->blend, &where)) from = "state block A";
        else if (info && find_blend(info->state_b, &bound->blend, &where)) from = "state block B";
        bound->has_blend = from[0] != 'n';
        static LONG logged;
        if (first_calls(logged, 24)) {
            const auto& t = bound->blend.RenderTarget[0];
            log("x2w-draw pipeline %p (creation slot %d, base slot %d): blend from %s at +0x%zx: enable %d src %d dest %d op %d / alpha %d %d %d "
                "mask 0x%x; blocks %zu + %zu bytes", static_cast<void*>(pipeline), own ? own->path : 0, info ? info->path : 0, from, where,
                t.BlendEnable, t.SrcBlend, t.DestBlend, t.BlendOp, t.SrcBlendAlpha, t.DestBlendAlpha, t.BlendOpAlpha, t.RenderTargetWriteMask,
                info ? info->state_a.size() : 0, info ? info->state_b.size() : 0);
            if (info && !bound->has_blend) {
                char hex[3 * 0x1a0 + 1];
                hex_text(info->state_a.data(), info->state_a.size() < 0x1a0 ? info->state_a.size() : 0x1a0, hex, sizeof(hex));
                log("x2w-draw   state A: %s", hex);
                hex_text(info->state_b.data(), info->state_b.size() < 0x1a0 ? info->state_b.size() : 0x1a0, hex, sizeof(hex));
                log("x2w-draw   state B: %s", hex);
                if (own != info && own) {
                    hex_text(own->desc.data(), own->desc.size() < 0x100 ? own->desc.size() : 0x100, hex, sizeof(hex));
                    log("x2w-draw   derived: %s", hex);
                }
            }
        }
    }
    if (info && info->path == 57 && info->desc.size() >= 0x60) {
        u64 words[12];
        std::memcpy(words, info->desc.data(), sizeof(words));
        Wrapper* root = as_wrapper(reinterpret_cast<const void*>(words[0]));
        std::vector<unsigned char> root_blob;
        if (root) {
            AcquireSRWLockShared(&note_lock);
            const auto found = root_blobs().find(root);
            if (found != root_blobs().end()) root_blob = found->second;
            ReleaseSRWLockShared(&note_lock);
        }
        const auto* a = reinterpret_cast<const void*>(words[5]);
        const auto* b = reinterpret_cast<const void*>(words[9]);
        const std::size_t a_bytes = static_cast<std::size_t>(words[6]), b_bytes = static_cast<std::size_t>(words[10]);
        if (!root_blob.empty() && a_bytes && b_bytes && a_bytes <= (1u << 20) && b_bytes <= (1u << 20) && x2w_local::readable(a, a_bytes) &&
            x2w_local::readable(b, b_bytes)) {
            const u64 key = fnv_bytes(root_blob.data(), root_blob.size(), fnv_bytes(b, b_bytes, fnv_bytes(a, a_bytes)));
            auto& by_key = translation_by_key();
            const auto found = by_key.find(key);
            if (found != by_key.end()) {
                result = found->second;
            } else {
                result = new Translation;
                result->key = key;
                by_key[key] = result;
                load_translation(*result, a, a_bytes, b, b_bytes, root_blob);
            }
        }
    }
    by_pipeline[pipeline] = bound;
    return bound;
}

ID3D12PipelineState* pipeline_for(BoundPipeline& bound, const Pass& pass)
{
    Translation& tr = *bound.translation;
    u64 key = pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) key = key * 131 + static_cast<u64>(pass.target_formats[i]);
    const auto found = bound.pipelines.find(key);
    if (found != bound.pipelines.end()) return found->second;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = tr.root;
    desc.VS = {tr.vs->GetBufferPointer(), tr.vs->GetBufferSize()};
    desc.PS = {tr.ps->GetBufferPointer(), tr.ps->GetBufferSize()};
    // Blend state as found in the pipeline's state block, else opaque. Raster and depth state are
    // not decoded yet: no culling, no depth (enough for the 2D start-up screens).
    for (auto& target : desc.BlendState.RenderTarget)
        target = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
                  D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    if (bound.has_blend) desc.BlendState = bound.blend;
    // LOCAL EXPERIMENT (X2W_DRAW_DEFAULT_BLEND=1): when no blend description was found, blend
    // source alpha over the target. Draws that output alpha 1 look the same; coverage-alpha draws
    // (atlas glyphs) need it. The real fix decodes the console blend registers.
    static const bool default_blend = x2w_local::env_on("X2W_DRAW_DEFAULT_BLEND");
    if (!bound.has_blend && default_blend)
        for (auto& target : desc.BlendState.RenderTarget)
            target = {TRUE, FALSE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                      D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    desc.SampleMask = ~0u;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = pass.target_count;
    for (UINT i = 0; i < pass.target_count; ++i) desc.RTVFormats[i] = pass.target_formats[i];
    desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pipeline = nullptr;
    const HRESULT hr = device_of(the_device)->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    log("x2w-draw desktop pipeline for %016llx, %u targets (format %d) -> 0x%08lx", tr.key, pass.target_count,
        pass.target_count ? pass.target_formats[0] : 0, static_cast<unsigned long>(hr));
    check_device("translated CreateGraphicsPipelineState", 0);
    bound.pipelines[key] = pipeline;
    return pipeline;
}

// Private shader-visible heap for the translated draws' tables, used as a ring.
constexpr UINT draw_heap_size = 1u << 18;
ID3D12DescriptorHeap* draw_heap;
UINT draw_heap_increment;
volatile LONG draw_heap_cursor;

bool ensure_draw_heap()
{
    if (draw_heap) return true;
    // LOCAL: the ring is [0, draw_heap_size); after it, the view-id region of translated compute (d3d12_compute.inl).
    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, draw_heap_size + 8 + 8 * 90000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device_of(the_device)->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&draw_heap)))) return false;
    draw_heap_increment = device_of(the_device)->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// Copies of title memory made for the current frame: (address, bytes) -> ring offset.
std::unordered_map<u64, u64>& frame_copies() { static std::unordered_map<u64, u64> m; return m; }
LONG frame_copies_frame = -1;

bool frame_copy(u64 address, std::size_t bytes, u64* offset)
{
    if (frame_copies_frame != frame_number) {
        frame_copies().clear();
        frame_copies_frame = frame_number;
    }
    const u64 key = address * 1099511628211ull ^ bytes;
    const auto found = frame_copies().find(key);
    if (found != frame_copies().end()) {
        *offset = found->second;
        return true;
    }
    if (!ring_copy(address, bytes, offset)) return false;
    frame_copies()[key] = *offset;
    return true;
}

void buffer_view(D3D12_CPU_DESCRIPTOR_HANDLE dest, bool have, u64 offset, std::size_t bytes)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = have ? offset / 4 : 0;
    desc.Buffer.NumElements = have ? static_cast<UINT>((bytes + 3) / 4) : 1;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_of(the_device)->CreateShaderResourceView(have ? upload_ring : nullptr, &desc, dest);
    if (!have) InterlockedIncrement(&null_bindings);
}

// Texture contents. The XG stand-in reports the desktop copyable-footprint layout to the title
// (mip footprints from GetCopyableFootprints, array slices every 64 KiB-aligned slice size), so a
// texture's title memory is already in a layout CopyTextureRegion can read: each subresource is
// copied through the upload ring as a placed footprint. A texture is uploaded on its first bind and
// again when its memory changed: XG copies announce themselves (x2w_d3d12_texture_written), and
// textures up to 32 MiB are also re-hashed every 15 frames for writes the title makes on its own.
// Targets and UAVs are GPU-written and never uploaded.
struct TextureUpload { u64 hash = 0; u64 generation = ~0ull; LONG checked_frame = -1000; bool done = false; };
std::unordered_map<ID3D12Resource*, TextureUpload>& texture_uploads() { static std::unordered_map<ID3D12Resource*, TextureUpload> m; return m; }
LONG textures_uploaded, texture_bytes_uploaded, textures_unreadable;
u64 hash_memory(const unsigned char* data, std::size_t bytes)
{
    u64 h = 0xcbf29ce484222325ull;
    const std::size_t words = bytes / 8;
    const u64* w = reinterpret_cast<const u64*>(data);
    for (std::size_t i = 0; i < words; ++i) h = (h ^ w[i]) * 0x100000001b3ull;
    for (std::size_t i = words * 8; i < bytes; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
    return h;
}
void upload_texture(Wrapper* self, Wrapper* resource, ID3D12Resource* texture)
{
    auto* const memory = static_cast<const unsigned char*>(resource->own().title_memory);
    auto* list = recording(self);
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    {
        static LONG reasons;
        if ((!memory || (desc.Flags & 0x7) || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) && !texture_uploads().count(texture) &&
            first_calls(reasons, 12)) {
            texture_uploads()[texture].done = true;
            log("x2w-draw texture not uploaded %p: %llux%u format %d flags 0x%x dim %d title memory %p", static_cast<void*>(texture),
                desc.Width, desc.Height, desc.Format, desc.Flags, desc.Dimension, static_cast<const void*>(memory));
        }
    }
    if ((desc.Format == DXGI_FORMAT_A8_UNORM || desc.Format == DXGI_FORMAT_R8_UNORM) && !texture_uploads().count(texture)) {
        static LONG skipped;
        if ((!memory || (desc.Flags & 0x7)) && first_calls(skipped, 12))
            log("x2w-draw single-channel texture %p %llux%u format %d not uploaded: title memory %p, flags 0x%x", static_cast<void*>(texture),
                desc.Width, desc.Height, desc.Format, static_cast<const void*>(memory), desc.Flags);
    }
    if (!memory || !list) return;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
    const UINT slices = desc.DepthOrArraySize ? desc.DepthOrArraySize : 1;
    if (mips > 15) return;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[15];
    UINT rows[15];
    UINT64 row_bytes[15], slice_total = 0;
    device_of(the_device)->GetCopyableFootprints(&desc, 0, mips, 0, footprints, rows, row_bytes, &slice_total);
    if (slice_total == ~0ull || !slice_total) return;
    const u64 stride = (slice_total + 0xFFFF) & ~u64{0xFFFF};
    const u64 total = slices > 1 ? stride * (slices - 1) + slice_total : slice_total;
    TextureUpload& state = texture_uploads()[texture];
    u64 generation = 0;
    AcquireSRWLockShared(&written_lock);
    if (const auto found = written_textures().find(reinterpret_cast<u64>(memory)); found != written_textures().end()) generation = found->second;
    ReleaseSRWLockShared(&written_lock);
    const bool announced = state.generation != generation;
    // Glyph atlases (A8) are written by the CPU just before the draws that use them, and the UI caches
    // the result in its own target: they are checked on every frame, other textures every 15.
    const LONG recheck_frames = desc.Format == DXGI_FORMAT_A8_UNORM && total <= (u64{8} << 20) ? 1 : 15;
    const bool recheck = state.done && !announced && total <= (u64{32} << 20) && frame_number - state.checked_frame >= recheck_frames;
    if (state.done && !announced && !recheck) return;
    if (readable_window(reinterpret_cast<u64>(memory), static_cast<std::size_t>(total)) < total) {
        if (!state.done) InterlockedIncrement(&textures_unreadable);
        if (!state.done && desc.Format == DXGI_FORMAT_A8_UNORM)
            log("x2w-draw A8 texture %p %llux%u unreadable: %llu bytes at %p", static_cast<void*>(texture), desc.Width, desc.Height,
                total, static_cast<const void*>(memory));
        state.done = true;
        state.generation = generation;
        return;
    }
    state.checked_frame = frame_number;
    const u64 hash = hash_memory(memory, static_cast<std::size_t>(total));
    if (state.done && hash == state.hash) {
        state.generation = generation;
        return;
    }
    state.hash = hash;
    state.generation = generation;
    state.done = true;
    require_state(self, texture, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT s = 0; s < slices; ++s)
        for (UINT m = 0; m < mips; ++m) {
            const auto& fp = footprints[m];
            const u64 bytes = u64{fp.Footprint.RowPitch} * rows[m] * fp.Footprint.Depth;
            u64 offset = 0;
            if (!ring_copy(reinterpret_cast<u64>(memory) + stride * s + fp.Offset, static_cast<std::size_t>(bytes), &offset)) continue;
            D3D12_TEXTURE_COPY_LOCATION source{upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            source.PlacedFootprint = {offset, fp.Footprint};
            D3D12_TEXTURE_COPY_LOCATION target{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            target.SubresourceIndex = m + s * mips;
            list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
    InterlockedIncrement(&textures_uploaded);
    InterlockedExchangeAdd(&texture_bytes_uploaded, static_cast<LONG>(total >> 10));
    if (desc.Format == DXGI_FORMAT_A8_UNORM || desc.Format == DXGI_FORMAT_R8_UNORM) {
        // Single-channel textures (glyph atlases): what the first mip holds.
        // LOCAL DIAGNOSTIC: A8 textures are logged on each of their first 4 uploads (R8 ones once
        // in total: video planes re-upload every frame); with X2W_DRAW_DUMP_DIR set, an A8 texture's
        // first mip is also written as atlas_<texture>_<n>.pgm.
        static std::unordered_map<ID3D12Resource*, int> single;
        static LONG r8_logged;
        const bool a8 = desc.Format == DXGI_FORMAT_A8_UNORM;
        int& seen = single[texture];
        if (a8 ? seen++ < 4 : first_calls(r8_logged, 2)) {
            const std::size_t bytes = static_cast<std::size_t>(footprints[0].Footprint.RowPitch) * rows[0];
            std::size_t zero = 0, full = 0;
            for (std::size_t i = 0; i < bytes; ++i) zero += memory[i] == 0, full += memory[i] == 0xFF;
            log("x2w-draw single-channel upload %p %llux%u format %d mips %u: first mip %zu bytes, %.1f%% zero, %.1f%% 0xff, hash %016llx",
                static_cast<void*>(texture), desc.Width, desc.Height, desc.Format, mips, bytes, 100.0 * zero / bytes, 100.0 * full / bytes, hash);
            char dir[MAX_PATH];
            const DWORD n = GetEnvironmentVariableA("X2W_DRAW_DUMP_DIR", dir, MAX_PATH);
            if (a8 && n && n < MAX_PATH - 64) {
                char path[MAX_PATH];
                std::snprintf(path, sizeof path, "%s\\atlas_%p_%d.pgm", dir, static_cast<void*>(texture), seen);
                if (FILE* out = std::fopen(path, "wb")) {
                    std::fprintf(out, "P5\n%llu %u\n255\n", static_cast<unsigned long long>(desc.Width), desc.Height);
                    for (UINT y = 0; y < rows[0]; ++y) std::fwrite(memory + std::size_t{footprints[0].Footprint.RowPitch} * y, 1, static_cast<std::size_t>(desc.Width), out);
                    std::fclose(out);
                }
            }
        }
    }
    static LONG shown;
    if (first_calls(shown, 12))
        log("x2w-draw texture upload %p %llux%u x%u mips %u format %d from %p (%llu KiB)%s", static_cast<void*>(texture), desc.Width,
            desc.Height, slices, mips, desc.Format, static_cast<const void*>(memory), total >> 10, announced ? " (announced write)" : "");
}

// Per-draw dumps (X2W_DRAW_DUMP_FRAME=<frame>, X2W_DRAW_DUMP_DIR=<folder>): during that frame, render
// target 0 is copied to a readback buffer after every translated draw (at most 48); 30 frames later
// the copies are written as BMPs reduced 4x (draw_<n>_<pipeline>.bmp), so one run shows what each
// draw added. Only 4-byte target formats are written.
struct DrawDump { ID3D12Resource* buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; UINT rows; u64 key; UINT count; DXGI_FORMAT format; };
std::vector<DrawDump>& draw_dumps() { static std::vector<DrawDump> v; return v; }
// A file named dump_now in X2W_DRAW_DUMP_DIR also asks for a dump: the frame after it is seen is
// dumped and the file is deleted, so a dump can be taken when a screen of interest is up.
LONG draw_dump_wanted = -1;
LONG draw_dump_frame()
{
    if (draw_dump_wanted < 0) {
        char value[16]{};
        draw_dump_wanted = GetEnvironmentVariableA("X2W_DRAW_DUMP_FRAME", value, sizeof(value)) ? std::atol(value) : 0L;
    }
    return draw_dump_wanted;
}
void draw_dump_poll(LONG frame)
{
    static char trigger[MAX_PATH + 16];
    static const bool armed = [] {
        char folder[MAX_PATH]{};
        if (!GetEnvironmentVariableA("X2W_DRAW_DUMP_DIR", folder, sizeof(folder))) return false;
        std::snprintf(trigger, sizeof(trigger), "%s\\dump_now", folder);
        return true;
    }();
    if (!armed || frame % 15 || GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES) return;
    const LONG wanted = draw_dump_frame();
    if (wanted && frame <= wanted + 30) return;  // a dump is in progress
    DeleteFileA(trigger);
    draw_dump_wanted = frame + 2;
    log("x2w-draw per-draw dump requested for frame %ld", frame + 2);
}
void draw_dump_after(Wrapper* self, ID3D12GraphicsCommandList* list, u64 key, UINT count)
{
    Pass& pass = pass_of(self);
    const LONG wanted = draw_dump_frame();
    if (!wanted || frame_number < wanted || frame_number > wanted + 1 || !pass.target0 || draw_dumps().size() >= 48) return;
    const D3D12_RESOURCE_DESC desc = pass.target0->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device_of(the_device)->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    if (FAILED(device_of(the_device)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&readback)))) return;
    require_state(self, pass.target0, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION target{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    target.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{pass.target0, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    require_state(self, pass.target0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw_dumps().push_back({readback, footprint, rows, key, count, desc.Format});
}
void draw_dump_flush(LONG frame)
{
    draw_dump_poll(frame);
    const LONG wanted = draw_dump_frame();
    if (!wanted || frame != wanted + 30) return;
    static const UINT scale = [] {
        char value[8]{};
        const long n = GetEnvironmentVariableA("X2W_DRAW_DUMP_SCALE", value, sizeof(value)) ? std::atol(value) : 4L;
        return static_cast<UINT>(n >= 1 && n <= 8 ? n : 4);
    }();
    char folder[MAX_PATH] = ".";
    GetEnvironmentVariableA("X2W_DRAW_DUMP_DIR", folder, sizeof(folder));
    AcquireSRWLockExclusive(&draw_lock);
    int index = 0;
    for (const DrawDump& dump : draw_dumps()) {
        ++index;
        const UINT bytes_per_pixel = dump.footprint.Footprint.RowPitch / (dump.footprint.Footprint.Width ? dump.footprint.Footprint.Width : 1);
        void* mapped = nullptr;
        if (bytes_per_pixel != 4 || FAILED(dump.buffer->Map(0, nullptr, &mapped))) continue;
        const UINT w = dump.footprint.Footprint.Width / scale, h = dump.rows / scale;
        std::vector<unsigned char> pixels(std::size_t{w} * h * 4);
        const bool bgra = dump.format == DXGI_FORMAT_B8G8R8A8_UNORM || dump.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const unsigned char* p = static_cast<const unsigned char*>(mapped) + std::size_t{y * scale} * dump.footprint.Footprint.RowPitch + std::size_t{x} * scale * 4;
                unsigned char* q = &pixels[(std::size_t{h - 1 - y} * w + x) * 4];
                q[0] = bgra ? p[0] : p[2];
                q[1] = p[1];
                q[2] = bgra ? p[2] : p[0];
                q[3] = 255;
            }
        dump.buffer->Unmap(0, nullptr);
        char path[MAX_PATH + 64];
        std::snprintf(path, sizeof(path), "%s\\draw_%02d_%016llx_%u.bmp", folder, index, dump.key, dump.count);
        if (FILE* file = std::fopen(path, "wb")) {
            const std::uint32_t data = static_cast<std::uint32_t>(pixels.size());
            unsigned char header[54] = {'B', 'M'};
            const std::uint32_t file_size = 54 + data, offset = 54, info = 40, planes_bits = 1u | (32u << 16);
            const std::int32_t width = static_cast<std::int32_t>(w), height = static_cast<std::int32_t>(h);
            std::memcpy(header + 2, &file_size, 4);
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &info, 4);
            std::memcpy(header + 18, &width, 4);
            std::memcpy(header + 22, &height, 4);
            std::memcpy(header + 26, &planes_bits, 4);
            std::memcpy(header + 34, &data, 4);
            std::fwrite(header, 1, 54, file);
            std::fwrite(pixels.data(), 1, pixels.size(), file);
            std::fclose(file);
        }
    }
    log("x2w-draw per-draw dump: %d target copies written to %s", index, folder);
    for (const DrawDump& dump : draw_dumps()) dump.buffer->Release();
    draw_dumps().clear();
    ReleaseSRWLockExclusive(&draw_lock);
}

thread_local u64 last_draw_key;          // LOCAL: translation key of the last draw (trace)
thread_local const char* last_draw_why;  // LOCAL: why the last draw was not drawn (trace)
bool draw_translated(Wrapper* self, bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
    auto* list = recording(self);
    Pass& pass = pass_of(self);
    const bool rect = pass.topology == 0x11;
    last_draw_key = 0;
    last_draw_why = !list ? "no list" : !pass.pipeline ? "no pipeline" : !pass.targets_bound ? "targets not bound" : "topology";
    if (!list || !pass.pipeline || !pass.targets_bound || !(rect || pass.topology == 4 || pass.topology == 5) || (rect && indexed)) return false;
    AcquireSRWLockExclusive(&draw_lock);
    BoundPipeline* bound = translation_for(pass.pipeline);
    Translation* tr = bound ? bound->translation : nullptr;
    ID3D12PipelineState* pipeline = tr && tr->state == 1 && ensure_draw_heap() ? pipeline_for(*bound, pass) : nullptr;
    last_draw_key = tr ? tr->key : 0;
    last_draw_why = !tr ? "no translation (creation path)" : tr->state != 1 ? "translation missing or failed" : "desktop pipeline not created";
    if (!pipeline) {
        ReleaseSRWLockExclusive(&draw_lock);
        return false;
    }
    const UINT n = static_cast<UINT>(tr->srvs.size());
    UINT first = static_cast<UINT>(InterlockedExchangeAdd(&draw_heap_cursor, static_cast<LONG>(n))) % draw_heap_size;
    if (first + n > draw_heap_size) {
        first = 0;
        InterlockedExchange(&draw_heap_cursor, static_cast<LONG>(n));
    }
    std::uint32_t strides[32] = {};
    std::uint32_t word3[32] = {};  // LOCAL: word 3 of each table buffer descriptor (format for buffer_load_format)
    // Only the vertices this draw can reference are copied: [vertex_first, vertex_first + vertex_count).
    // The shader adds the "base vertex" entry value to the vertex index, so that value is biased by
    // -vertex_first and each stream's view starts at vertex_first.
    const UINT index_bytes = pass.ib.Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    const u64 index_address = pass.ib.BufferLocation + u64{start} * index_bytes;
    const std::size_t index_total = std::size_t{count} * index_bytes;
    const bool indices_ok = indexed && pass.ib.BufferLocation && readable_window(index_address, index_total) >= index_total;
    INT vertex_first = indexed ? 0 : static_cast<INT>(start);
    UINT vertex_count = indexed ? 0 : count;
    if (indices_ok) {
        std::uint32_t low = ~0u, high = 0;
        for (UINT k = 0; k < count; ++k) {
            const std::uint32_t value = index_bytes == 2 ? reinterpret_cast<const std::uint16_t*>(index_address)[k]
                                                         : reinterpret_cast<const std::uint32_t*>(index_address)[k];
            if (value < low) low = value;
            if (value > high) high = value;
        }
        if (low <= high) {
            vertex_first = base_vertex + static_cast<INT>(low);
            vertex_count = high - low + 1;
        }
    }
    if (vertex_first < 0) vertex_first = 0;
    for (UINT i = 0; i < n; ++i) {
        const Translation::Srv& srv = tr->srvs[i];
        const D3D12_CPU_DESCRIPTOR_HANDLE dest{draw_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{first + i} * draw_heap_increment};
        u64 address = 0, offset = 0;
        std::size_t bytes = 0;
        if (srv.source == 1 && srv.a < 8) {
            address = pass.vb[srv.a].BufferLocation;
            bytes = pass.vb[srv.a].SizeInBytes;
            strides[i] = pass.vb[srv.a].StrideInBytes;
            const u64 skip = u64{static_cast<UINT>(vertex_first)} * strides[i];
            if (strides[i] && vertex_count && skip < bytes) {
                const u64 wanted = u64{vertex_count} * strides[i];
                address += skip;
                bytes = static_cast<std::size_t>(wanted < bytes - skip ? wanted : bytes - skip);
            }
        } else if (srv.source == 2 && srv.a < 16) {
            address = pass.root_address[srv.a];
            bytes = readable_window(address, 0x10000);
        }
        const u64 entry = srv.source == 0 && srv.a < 16 && pass.table_address[srv.a] ? pass.table_address[srv.a] + static_cast<u64>(srv.b) : 0;
        std::uint32_t words[8] = {};
        if (entry && x2w_local::readable(reinterpret_cast<const void*>(entry), 32)) std::memcpy(words, reinterpret_cast<const void*>(entry), 32);
        if (i < 32 && words[0] != view_token_magic) word3[i] = words[3];
        if (srv.kind >= 1 && srv.kind <= 4) {
            // kind 1 Texture2D, 2 Texture2DArray (upstream #1381), 3 Texture3D, 4 Texture1D
            const D3D12_SRV_DIMENSION want = srv.kind == 3 ? D3D12_SRV_DIMENSION_TEXTURE3D
                : srv.kind == 4 ? D3D12_SRV_DIMENSION_TEXTURE1D
                : srv.kind == 2 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
            // A texture: the title's descriptor memory carries the id of the view it created there.
            ViewRecord record{};
            bool known = false;
            if (words[0] == view_token_magic) {
                AcquireSRWLockShared(&note_lock);
                known = words[1] < view_records().size();
                if (known) record = view_records()[words[1]];
                ReleaseSRWLockShared(&note_lock);
            }
            Wrapper* resource = known ? as_wrapper(reinterpret_cast<const void*>(record.resource)) : nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.ViewDimension = want;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;  // same offset as Texture3D.MipLevels and Texture2DArray.MipLevels
            if (want == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) desc.Texture2DArray.ArraySize = 1;
            ID3D12Resource* texture = resource && resource->own().kind == resource_kind ? static_cast<ID3D12Resource*>(resource->own().real) : nullptr;
            if (texture && record.desc_bytes >= 40) {
                std::memcpy(&desc, record.desc, 40);
                desc.Format = desktop_format(desc.Format);
            }
            if (texture) upload_texture(self, resource, texture);
            if (texture) require_state(self, texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            else InterlockedIncrement(&null_bindings);
            // The first two texture bindings of every pipeline are logged.
            static SRWLOCK seen_lock = SRWLOCK_INIT;
            static std::unordered_map<u64, int> seen;
            AcquireSRWLockExclusive(&seen_lock);
            const bool show = seen[tr->key * 31 + i]++ < 2;
            ReleaseSRWLockExclusive(&seen_lock);
            if (show) {
                const D3D12_RESOURCE_DESC rd = texture ? texture->GetDesc() : D3D12_RESOURCE_DESC{};
                log("x2w-draw   %016llx srv %u texture: table entry 0x%llx words %08x %08x -> view %s, resource %p %llux%u fmt %d, view format %d",
                    tr->key, i, entry, words[0], words[1], known ? "known" : "UNKNOWN", static_cast<void*>(texture),
                    static_cast<unsigned long long>(rd.Width), rd.Height, rd.Format, desc.Format);
            }
            if (texture && want == D3D12_SRV_DIMENSION_TEXTURE2DARRAY && desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                // The shader declares an array; a plain 2D view is the same data as a one-slice array.
                const D3D12_TEX2D_SRV plain = desc.Texture2D;
                desc.ViewDimension = want;
                desc.Texture2DArray = {};
                desc.Texture2DArray.MostDetailedMip = plain.MostDetailedMip;
                desc.Texture2DArray.MipLevels = plain.MipLevels;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.PlaneSlice = plain.PlaneSlice;
                desc.Texture2DArray.ResourceMinLODClamp = plain.ResourceMinLODClamp;
            }
            if (texture && desc.ViewDimension != want && record.desc_bytes >= 40) {
                static LONG mismatch;
                if (first_calls(mismatch, 6)) log("x2w-draw   %016llx srv %u: view dimension %d does not match the shader; binding null", tr->key, i, desc.ViewDimension);
                texture = nullptr;
                desc.ViewDimension = want;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.Texture2DArray = {};
                desc.Texture2D.MipLevels = 1;
                if (want == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) desc.Texture2DArray.ArraySize = 1;
            }
            if (texture) fix_srv_plane(desc);
            device_of(the_device)->CreateShaderResourceView(texture, texture && record.desc_bytes < 40 ? nullptr : &desc, dest);
            continue;
        }
        if (srv.source == 0 && words[0] == view_token_magic && entry) {
            // A view the title created: a constant buffer view names its memory directly; a buffer
            // shader-resource view names a range of its resource's title memory.
            ViewRecord record{};
            AcquireSRWLockShared(&note_lock);
            if (words[1] < view_records().size()) record = view_records()[words[1]];
            ReleaseSRWLockShared(&note_lock);
            if (record.slot == 17 && record.desc_bytes >= 12) {
                std::uint32_t size = 0;
                std::memcpy(&address, record.desc, 8);
                std::memcpy(&size, record.desc + 8, 4);
                bytes = size;
            } else if (record.slot == 18 && record.desc_bytes >= 40) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view;
                std::memcpy(&view, record.desc, 40);
                Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
                if (view.ViewDimension == D3D12_SRV_DIMENSION_BUFFER && resource && resource->own().title_memory) {
                    const UINT element = view.Buffer.StructureByteStride ? view.Buffer.StructureByteStride : 4;
                    address = reinterpret_cast<u64>(resource->own().title_memory) + view.Buffer.FirstElement * element;
                    bytes = std::size_t{view.Buffer.NumElements} * element;
                    strides[i] = view.Buffer.StructureByteStride;
                }
            }
        }
        if (srv.source == 0 && words[0] != view_token_magic && entry) {
            // A hardware buffer descriptor the title wrote itself: base address, stride and record count.
            address = words[0] | (static_cast<u64>(words[1] & 0xffff) << 32);
            strides[i] = (words[1] >> 16) & 0x3fff;
            bytes = static_cast<std::size_t>(strides[i] ? strides[i] : 1) * words[2];
        }
        if (bytes > (16u << 20)) bytes = 16u << 20;
        bool have = address && bytes && readable_window(address, bytes) >= bytes && frame_copy(address, bytes, &offset);
        // Experiment (X2W_DRAW_NULL_IDENTITY=1): an empty buffer slot reads as repeated identity
        // matrices instead of zeros. The start-up screen's last pass takes its world matrix from a
        // slot the title leaves empty here; why is not known yet.
        static const bool identity_for_null = x2w_local::env_on("X2W_DRAW_NULL_IDENTITY");
        if (!have && identity_for_null) {
            static float identity[64];
            for (int k = 0; k < 64; ++k) identity[k] = (k % 16) % 5 == 0 ? 1.0f : 0.0f;
            address = reinterpret_cast<u64>(identity);
            bytes = sizeof(identity);
            have = frame_copy(address, bytes, &offset);
        }
        buffer_view(dest, have, offset, bytes);
        if (InterlockedIncrement(&tr->logged_bindings) <= 8) {
            char head[3 * 48 + 1] = "";
            if (have) hex_text(reinterpret_cast<const void*>(address), bytes < 48 ? bytes : 48, head, sizeof(head));
            log("x2w-draw   %016llx srv %u buffer source %d(%d,%d): entry 0x%llx words %08x %08x %08x %08x -> address 0x%llx, %zu bytes, stride %u: %s [%s]",
                tr->key, i, srv.source, srv.a, srv.b, entry, words[0], words[1], words[2], words[3], address, bytes, strides[i],
                have ? "bound" : "NULL", head);
        }
    }
    static LONG checks;
    if ((InterlockedIncrement(&checks) & 63) == 1) check_device("translated draw views", 0);
    std::uint32_t values[32] = {};
    for (std::size_t i = 0; i < tr->uds.size(); ++i) {
        const Translation::Ud& ud = tr->uds[i];
        values[i] = ud.kind == 0 ? (rect ? 1u : 0u) : ud.kind == 1 ? static_cast<std::uint32_t>((indexed ? base_vertex : static_cast<INT>(start)) - vertex_first)
            : ud.kind == 2 ? start_instance : ud.kind == 3 ? ((ud.a < 16 && ud.b < 8) ? pass.constants[ud.a][ud.b] : 0)
            : ud.kind == 5 ? (ud.a >= 0 && ud.a < 32 ? word3[ud.a] : 0)
            : (ud.a >= 0 && ud.a < 32 ? strides[ud.a] : 0);
    }
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootSignature(tr->root);
    if (!pass.heap_set) {
        list->SetDescriptorHeaps(1, &draw_heap);
        pass.heap_set = true;
    }
    list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(tr->uds.size()), values, 0);
    if (n) list->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{draw_heap->GetGPUDescriptorHandleForHeapStart().ptr + u64{first} * draw_heap_increment});
    list->IASetPrimitiveTopology(pass.topology == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bool drawn = false;
    if (indexed) {
        u64 offset = 0;
        const u64 address = index_address;
        const std::size_t bytes = index_total;
        if (indices_ok && frame_copy(address, bytes, &offset)) {
            const D3D12_INDEX_BUFFER_VIEW view{upload_ring->GetGPUVirtualAddress() + offset, static_cast<UINT>(bytes),
                                               index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
            list->IASetIndexBuffer(&view);
            list->DrawIndexedInstanced(count, instances ? instances : 1, 0, 0, start_instance);
            drawn = true;
        }
    } else {
        list->DrawInstanced(rect ? count / 3 * 6 : count, instances ? instances : 1, 0, start_instance);
        drawn = true;
    }
    if (drawn) draw_dump_after(self, list, tr->key, count);
    ReleaseSRWLockExclusive(&draw_lock);
    if (InterlockedIncrement(&tr->logged_draws) <= 10 || (count > 100 && InterlockedIncrement(&tr->logged_large) <= 12)) {
        char indices[3 * 24 + 1] = "";
        if (indexed && pass.ib.BufferLocation) hex_text(reinterpret_cast<const void*>(pass.ib.BufferLocation + u64{start} * 2), 24, indices, sizeof(indices));
        log("x2w-draw %s(%u, %u, start %u, base %d) pipeline %016llx topology 0x%x -> %s; entry [%x %x %x %x %x %x]; ib 0x%llx+%u fmt %d [%s]; "
            "vb 0x%llx %u/%u; targets %u", indexed ? "indexed" : "draw", count, instances, start, base_vertex, tr->key, pass.topology,
            drawn ? "drawn" : "skipped (index data)", values[0], values[1], values[2], values[3], values[4], values[5], pass.ib.BufferLocation,
            pass.ib.SizeInBytes, pass.ib.Format, indices, pass.vb[0].BufferLocation, pass.vb[0].SizeInBytes, pass.vb[0].StrideInBytes,
            pass.target_count);
    }
    return drawn;
}

// List slot 36 (desktop 32 + 4), SetGraphicsRootDescriptorTable(parameter, GPU handle): the table's
// address in the title's descriptor memory is kept for the draw.
void __stdcall list_table(Wrapper* self, UINT parameter, u64 gpu_handle)
{
    if (parameter >= 16) return;
    u64 address = 0;
    AcquireSRWLockShared(&note_lock);
    for (const HeapInfo& heap : heap_infos())
        if (heap.desktop_gpu && heap.title_cpu && gpu_handle >= heap.desktop_gpu && gpu_handle < heap.desktop_gpu + u64{heap.count} * heap.increment)
            address = heap.title_cpu + (gpu_handle - heap.desktop_gpu);
    ReleaseSRWLockShared(&note_lock);
    pass_of(self).table_address[parameter] = address;
}
// List slot 40 (desktop 36 + 4), SetGraphicsRoot32BitConstants(parameter, count, data, offset).
void __stdcall list_root_constants(Wrapper* self, UINT parameter, UINT count, const std::uint32_t* data, UINT offset)
{
    if (parameter >= 16 || offset >= 8 || !count || !x2w_local::readable(data, std::size_t{count} * 4)) return;
    for (UINT i = 0; i < count && offset + i < 8; ++i) pass_of(self).constants[parameter][offset + i] = data[i];
}
// List slot 47 (desktop 43 + 4), IASetIndexBuffer(view).
void __stdcall list_index_buffer(Wrapper* self, const D3D12_INDEX_BUFFER_VIEW* view)
{
    pass_of(self).ib = view && x2w_local::readable(view, sizeof(*view)) ? *view : D3D12_INDEX_BUFFER_VIEW{};
}
// List slot 48 (desktop 44 + 4), IASetVertexBuffers(start, count, views).
void __stdcall list_vertex_buffers(Wrapper* self, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views)
{
    if (!views || !x2w_local::readable(views, std::size_t{count} * sizeof(*views))) return;
    for (UINT i = 0; i < count && start + i < 8; ++i) pass_of(self).vb[start + i] = views[i];
}
// List slot 13, DrawIndexedInstanced(index count, instances, start index, base vertex, start instance).
void __stdcall list_draw_indexed(Wrapper* self, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
    const bool done = draw_translated(self, true, count, instances, start, base_vertex, start_instance);
    InterlockedIncrement(done ? &translated_draws : &untranslated_draws);
    if (trace_frame_active) {
        const Pass& pass = pass_of(self);
        note_traced_target(self, pass.target0, done);
        const D3D12_RESOURCE_DESC rd = pass.target0 ? pass.target0->GetDesc() : D3D12_RESOURCE_DESC{};
        log("x2w-frame draw-indexed %u %s target %p %llux%u fmt %d key %016llx %s", count, done ? "drawn" : "NOT drawn",
            static_cast<void*>(pass.target0), static_cast<unsigned long long>(rd.Width), rd.Height, rd.Format, last_draw_key,
            done ? "" : (last_draw_why ? last_draw_why : "draw failed"));
    }
}
