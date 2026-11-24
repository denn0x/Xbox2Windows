// Local experiment (never upstream): dispatches through translated console compute programs.
// Included inside d3d12_xs_device.cpp's anonymous namespace, after desktop_va().
//
// A list-slot-67 compute pipeline is looked up by its key in X2W_SHADER_CACHE: <key>.cs.txt
// (manifest) and <key>.cs.cso, produced offline by out/gamerun/gfx2hlsl.py from req_c67_<key>.*.
// The translated program uses its own root signature: [0] 32-bit constants ("entry" values),
// [1] one table of n SRVs (t0..) followed by n UAVs (u0..). Buffers are bound in place: a view on
// the desktop window over the title's own memory (desktop_range), so GPU writes reach the title's
// pages and later work reads them. Textures come from the view records the title created.

struct WindowSpan { D3D12_GPU_VIRTUAL_ADDRESS va; u64 size; ID3D12Resource* buffer; };
SRWLOCK window_span_lock = SRWLOCK_INIT;
std::vector<WindowSpan>& window_spans()
{
    static std::vector<WindowSpan> spans;
    return spans;
}
void note_window(ID3D12Resource* buffer)
{
    if (!buffer) return;
    AcquireSRWLockExclusive(&window_span_lock);
    window_spans().push_back({buffer->GetGPUVirtualAddress(), buffer->GetDesc().Width, buffer});
    ReleaseSRWLockExclusive(&window_span_lock);
}

LONG range_split, range_unmapped, range_ok;
// The desktop window buffer and byte offset that address [address, address + bytes) of title memory,
// when the range is contiguous in one window. Results are cached for the frame.
bool desktop_range(u64 address, u64 bytes, ID3D12Resource** buffer, u64* offset)
{
    Timed timed(time_cx_desktop_range);
    struct Hit { ID3D12Resource* buffer; u64 offset; };
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_map<u64, Hit> cache;
    static LONG cache_frame = -1;
    if (!address || !bytes) return false;
    const u64 key = address * 1099511628211ull ^ bytes;
    AcquireSRWLockExclusive(&lock);
    if (cache_frame != frame_number) {
        cache.clear();
        cache_frame = frame_number;
    }
    if (const auto found = cache.find(key); found != cache.end()) {
        *buffer = found->second.buffer;
        *offset = found->second.offset;
        ReleaseSRWLockExclusive(&lock);
        return *buffer != nullptr;
    }
    ReleaseSRWLockExclusive(&lock);
    Hit hit{nullptr, 0};
    const D3D12_GPU_VIRTUAL_ADDRESS va = desktop_va(address);
    if (!va) {
        InterlockedIncrement(&range_unmapped);
    } else {
        AcquireSRWLockShared(&window_span_lock);
        for (const WindowSpan& s : window_spans())
            if (va >= s.va && va + bytes <= s.va + s.size) {
                hit = {s.buffer, va - s.va};
                break;
            }
        ReleaseSRWLockShared(&window_span_lock);
        MEMORY_BASIC_INFORMATION info{};
        u64 known_base = 0, known_end = 0;
        if (hit.buffer && known_private(address, &known_base, &known_end)) {
            // A private allocation opened as a heap: the range is one allocation when it ends inside it.
            if (address + bytes > known_end) hit.buffer = nullptr;
            if (!hit.buffer) InterlockedIncrement(&range_split);
        } else if (hit.buffer && timed_query(reinterpret_cast<const void*>(address), &info) == sizeof(info)) {
            if (info.Type == MEM_MAPPED) {
                // Chunk pages: each 64 KiB page of the range must sit at the matching section offset.
                // LOCAL: asked of xmem directly (one lookup per page) instead of desktop_va, which
                // queried the page's memory and walked the windows for every page.
                using Location = BOOL(WINAPI*)(const void*, HANDLE*, ULONG64*);
                static const Location location = [] {
                    const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
                    return xmem ? reinterpret_cast<Location>(GetProcAddress(xmem, "x2w_xmem_gpu_location")) : nullptr;
                }();
                HANDLE first_section = nullptr;
                ULONG64 first_at = 0;
                if (!location || !location(reinterpret_cast<const void*>(address), &first_section, &first_at)) {
                    for (u64 at = (address | 0xffff) + 1; at < address + bytes; at += 0x10000)
                        if (desktop_va(at) != va + (at - address)) {
                            hit.buffer = nullptr;
                            break;
                        }
                } else {
                    for (u64 at = (address | 0xffff) + 1; at < address + bytes; at += 0x10000) {
                        HANDLE section = nullptr;
                        ULONG64 where = 0;
                        if (!location(reinterpret_cast<const void*>(at), &section, &where) || section != first_section ||
                            where != first_at + (at - address)) {
                            hit.buffer = nullptr;
                            break;
                        }
                    }
                }
            } else {
                MEMORY_BASIC_INFORMATION last{};
                if (timed_query(reinterpret_cast<const void*>(address + bytes - 1), &last) != sizeof(last) ||
                    last.AllocationBase != info.AllocationBase)
                    hit.buffer = nullptr;
            }
            if (!hit.buffer) InterlockedIncrement(&range_split);
        }
    }
    if (hit.buffer) InterlockedIncrement(&range_ok);
    AcquireSRWLockExclusive(&lock);
    cache[key] = hit;
    ReleaseSRWLockExclusive(&lock);
    *buffer = hit.buffer;
    *offset = hit.offset;
    return hit.buffer != nullptr;
}

struct ComputeTranslation {
    struct Res { bool uav; int kind, source, a, b; };  // kind 0 buffer, 1 2D, 2 2D array, 3 3D, 4 1D; source 0 table(param a, byte b), 2 root a
    struct Ud { int kind, a, b; };                     // 3 root constant(param a, word b), 4 stride of res a, 5 word 3 of res a, 6 base of res a
    int state = 0;                                     // 1 ready, 2 unavailable
    std::vector<Res> res;
    std::vector<Ud> uds;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::uint32_t compare_samplers = 0;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    UINT threads[3] = {};
    bool heap = false;  // indexes the title's resource heap (SM 6.6 ResourceDescriptorHeap)
    LONG logged = 0;
};
std::unordered_map<u64, ComputeTranslation>& compute_translations() { static std::unordered_map<u64, ComputeTranslation> m; return m; }
LONG cx_ready, cx_missing, cx_dispatched, cx_null_res, cx_bad_uav;

void load_compute(u64 key, ComputeTranslation& tr)
{
    tr.state = 2;
    wchar_t path[MAX_PATH];
    std::string manifest;
    if (!cache_path(path, L"", key, L".cs.txt") || !read_text(path, manifest)) {
        InterlockedIncrement(&cx_missing);
        return;
    }
    for (std::size_t at = 0; at < manifest.size();) {
        std::size_t end = manifest.find('\n', at);
        if (end == std::string::npos) end = manifest.size();
        const std::string line = manifest.substr(at, end - at);
        at = end + 1;
        int i = 0, x = 0, y = 0, z = 0;
        char kind[16] = "", source[16] = "", view[8] = "";
        unsigned w[4] = {};
        if (std::sscanf(line.c_str(), "cs %d %d %d", &x, &y, &z) == 3) {
            tr.threads[0] = x, tr.threads[1] = y, tr.threads[2] = z;
        } else if (line.rfind("heap 1", 0) == 0) {
            tr.heap = true;
        } else if (std::sscanf(line.c_str(), "udsrc %d %15s %d %d", &i, source, &x, &y) >= 2) {
            ComputeTranslation::Ud ud{-1, x, y};
            if (!std::strcmp(source, "rootconst")) ud.kind = 3;
            else if (!std::strcmp(source, "stride")) ud.kind = 4;
            else if (!std::strcmp(source, "bufword3")) ud.kind = 5;
            else if (!std::strcmp(source, "bufbase")) ud.kind = 6;
            if (ud.kind < 0 || static_cast<std::size_t>(i) != tr.uds.size()) {
                log("x2w-cx %016llx: entry value %d from %s not supported", key, i, source);
                return;
            }
            tr.uds.push_back(ud);
        } else if (std::sscanf(line.c_str(), "%7s %d %15s %15s %d %d", view, &i, kind, source, &x, &y) >= 5 &&
                   (!std::strcmp(view, "srv") || !std::strcmp(view, "uav"))) {
            ComputeTranslation::Res r{view[0] == 'u', !std::strcmp(kind, "tex2d") ? 1 : !std::strcmp(kind, "tex2da") ? 2 : !std::strcmp(kind, "tex3d") ? 3 : !std::strcmp(kind, "tex1d") ? 4 : 0,
                                      !std::strcmp(source, "root") ? 2 : 0, x, y};
            if (static_cast<std::size_t>(i) != tr.res.size()) return;
            tr.res.push_back(r);
        } else if (std::sscanf(line.c_str(), "smp %d %x %x %x %x", &i, &w[0], &w[1], &w[2], &w[3]) == 5) {
            tr.samplers.push_back({w[0], w[1], w[2], w[3]});
        } else if (std::sscanf(line.c_str(), "smpcmp %d", &i) == 1 && i >= 0 && i < 32) {
            tr.compare_samplers |= 1u << i;
        }
    }
    if (tr.uds.size() > 60 || tr.res.size() > 64) return;
    ID3DBlob* code = nullptr;
    if (!cache_path(path, L"", key, L".cs.cso") || FAILED(D3DReadFileToBlob(path, &code))) return;
    const UINT n = static_cast<UINT>(tr.res.size());
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = tr.uds.empty() ? 1u : static_cast<UINT>(tr.uds.size());
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, n, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, n, 0, 0, n}};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    for (std::size_t i = 0; i < tr.samplers.size(); ++i) samplers.push_back(x2w_startup::decode_sampler(tr.samplers[i], static_cast<UINT>(i), (tr.compare_samplers >> i) & 1));
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = E_FAIL;
    if (!tr.heap) {
        D3D12_ROOT_SIGNATURE_DESC desc{n ? 2u : 1u, parameters, static_cast<UINT>(samplers.size()), samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE};
        hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    } else {
        // [0] constants, [1] t0 space1 = the title heap's memory, [2] t1 space1 = per-view info, [3] the table
        D3D12_DESCRIPTOR_RANGE1 ranges1[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, n, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, n, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, n}};
        D3D12_ROOT_PARAMETER1 p1[4]{};
        p1[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p1[0].Constants.Num32BitValues = parameters[0].Constants.Num32BitValues;
        p1[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p1[1].Descriptor = {0, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        p1[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p1[2].Descriptor = {1, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        p1[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p1[3].DescriptorTable = {2, ranges1};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd{};
        vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        vd.Desc_1_1 = {n ? 4u : 3u, p1, static_cast<UINT>(samplers.size()), samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED};
        hr = D3D12SerializeVersionedRootSignature(&vd, &blob, &errors);
        if (FAILED(hr) && errors) log("x2w-cx %016llx root signature: %.300s", key, static_cast<const char*>(errors->GetBufferPointer()));
    }
    if (SUCCEEDED(hr)) hr = device_of(the_device)->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&tr.root));
    if (blob) blob->Release();
    if (errors) errors->Release();
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = tr.root;
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        hr = device_of(the_device)->CreateComputePipelineState(&pd, IID_PPV_ARGS(&tr.pso));
    }
    code->Release();
    log("x2w-cx %016llx translated: %zu entry values, %u resources, %zu samplers, group %ux%ux%u -> 0x%08lx", key, tr.uds.size(), n,
        tr.samplers.size(), tr.threads[0], tr.threads[1], tr.threads[2], static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    tr.state = 1;
    InterlockedIncrement(&cx_ready);
}

DXGI_FORMAT typeless_to_float(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
    }
}
bool float_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R8_UNORM:
        return true;
    default:
        return false;
    }
}

// Writes a null view of the right shape for resource r.
void null_view(const ComputeTranslation::Res& r, D3D12_CPU_DESCRIPTOR_HANDLE dest, bool uav)
{
    InterlockedIncrement(&cx_null_res);
    if (uav) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = r.kind ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = r.kind == 1 ? D3D12_UAV_DIMENSION_TEXTURE2D : r.kind == 2 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
            : r.kind == 3 ? D3D12_UAV_DIMENSION_TEXTURE3D : r.kind == 4 ? D3D12_UAV_DIMENSION_TEXTURE1D : D3D12_UAV_DIMENSION_BUFFER;
        if (!r.kind) {
            d.Buffer.NumElements = 1;
            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        if (r.kind == 2) d.Texture2DArray.ArraySize = 1;
        if (r.kind == 3) d.Texture3D.WSize = 1;
        device_of(the_device)->CreateUnorderedAccessView(nullptr, nullptr, &d, dest);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = r.kind ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = r.kind == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : r.kind == 2 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
            : r.kind == 3 ? D3D12_SRV_DIMENSION_TEXTURE3D : r.kind == 4 ? D3D12_SRV_DIMENSION_TEXTURE1D : D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (!r.kind) {
            d.Buffer.NumElements = 1;
            d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        } else {
            d.Texture2D.MipLevels = 1;
            if (r.kind == 2) d.Texture2DArray.ArraySize = 1;
        }
        device_of(the_device)->CreateShaderResourceView(nullptr, &d, dest);
    }
}

// Binds a texture behind a view record as SRV or UAV (true when a real view was written).
// prepare: also upload a plain texture and move it to the view's state (false for the bulk heap fill).
bool texture_view(Wrapper* self, const ComputeTranslation::Res& r, const ViewRecord& record, D3D12_CPU_DESCRIPTOR_HANDLE dest, bool prepare = true)
{
    Timed timed(time_cx_texture);
    Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
    ID3D12Resource* texture = resource && resource->own().kind == resource_kind ? static_cast<ID3D12Resource*>(resource->own().real) : nullptr;
    if (!texture || record.desc_bytes < 24) return false;
    const D3D12_RESOURCE_DESC rd = texture->GetDesc();
    const int dim = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 3 : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 4
        : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? (rd.DepthOrArraySize > 1 ? 2 : 1) : 0;
    if (!dim || (dim != r.kind && !(dim == 2 && r.kind == 1) && !(dim == 1 && r.kind == 2))) return false;
    // The view's format and first mip / slice from the title's view description.
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mip = 0, slice = 0;
    std::memcpy(&format, record.desc, 4);
    format = typeless_to_float(desktop_format(format ? format : rd.Format));
    if (record.slot == 19) {
        std::memcpy(&mip, record.desc + 8, 4);
        if (r.kind == 2 || r.kind == 3) std::memcpy(&slice, record.desc + 12, 4);
    } else if (record.slot == 18) {
        std::memcpy(&mip, record.desc + 16, 4);  // MostDetailedMip
    }
    if (mip >= rd.MipLevels) mip = 0;
    if (r.uav) {
        if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !float_format(format)) {
            InterlockedIncrement(&cx_bad_uav);
            return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = format;
        if (r.kind == 1) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice = mip; }
        else if (r.kind == 2) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray = {mip, slice < rd.DepthOrArraySize ? slice : 0, rd.DepthOrArraySize - (slice < rd.DepthOrArraySize ? slice : 0), 0}; }
        else if (r.kind == 3) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; d.Texture3D = {mip, 0, UINT(-1)}; }
        else { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; d.Texture1D.MipSlice = mip; }
        if (prepare) require_state(self, texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_of(the_device)->CreateUnorderedAccessView(texture, nullptr, &d, dest);
        return true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (r.kind == 1) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MostDetailedMip = mip; d.Texture2D.MipLevels = UINT(-1); }
    else if (r.kind == 2) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MostDetailedMip = mip; d.Texture2DArray.MipLevels = UINT(-1); d.Texture2DArray.ArraySize = rd.DepthOrArraySize; }
    else if (r.kind == 3) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; d.Texture3D.MostDetailedMip = mip; d.Texture3D.MipLevels = UINT(-1); }
    else { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; d.Texture1D.MostDetailedMip = mip; d.Texture1D.MipLevels = UINT(-1); }
    if (prepare && record.slot != 19 && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
        !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        upload_texture(self, resource, texture);
    if (prepare) require_state(self, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    fix_srv(d, texture);
    device_of(the_device)->CreateShaderResourceView(texture, &d, dest);
    return true;
}

// The view-id region of the shader-visible heap: 8 views per title view id i at
// draw_heap_size + 8 + 8 * i (0 Texture2D, 1 Texture2DArray, 2 Texture3D, 3 Texture1D, 4 raw buffer SRV,
// 5 RWTexture2D, 6 RWTexture2DArray, 7 raw buffer UAV), empty views of each kind at draw_heap_size.
// vinfo (an upload buffer, 16 bytes per id) holds the buffer stride. Filled up to the newest record.
constexpr UINT view_ids_max = 90000;
UINT views_filled;
ID3D12Resource* vinfo_buffer;
std::uint32_t* vinfo_words;
LONG views_bound, views_null;
void fill_view_kinds(Wrapper* self, UINT base, const ViewRecord* record)
{
    const SIZE_T inc = draw_heap_increment;
    auto at = [&](UINT k) { return D3D12_CPU_DESCRIPTOR_HANDLE{draw_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{base + k} * inc}; };
    static const ComputeTranslation::Res kinds[8] = {{false, 1, 0, 0, 0}, {false, 2, 0, 0, 0}, {false, 3, 0, 0, 0}, {false, 4, 0, 0, 0},
                                                     {false, 0, 0, 0, 0}, {true, 1, 0, 0, 0}, {true, 2, 0, 0, 0}, {true, 0, 0, 0, 0}};
    bool done[8] = {};
    if (record) {
        Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record->resource));
        ID3D12Resource* real = resource && resource->own().kind == resource_kind ? static_cast<ID3D12Resource*>(resource->own().real) : nullptr;
        const bool buffer_view = record->slot == 17 || (real && real->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        if (!buffer_view && real) {
            for (UINT k : {0u, 1u, 2u, 3u, 5u, 6u}) done[k] = texture_view(self, kinds[k], *record, at(k), false);
        } else if (buffer_view) {
            u64 address = 0;
            std::size_t bytes = 0;
            std::uint32_t stride = 0;
            if (record->slot == 17 && record->desc_bytes >= 12) {
                std::uint32_t size = 0;
                std::memcpy(&address, record->desc, 8);
                std::memcpy(&size, record->desc + 8, 4);
                bytes = size;
            } else if ((record->slot == 18 || record->slot == 19) && record->desc_bytes >= 24 && resource && resource->own().title_memory) {
                u64 first_element = 0;
                std::uint32_t count = 0;
                const std::size_t off = record->slot == 18 ? 16 : 8;
                std::memcpy(&first_element, record->desc + off, 8);
                std::memcpy(&count, record->desc + off + 8, 4);
                std::memcpy(&stride, record->desc + off + 12, 4);
                const UINT element = stride ? stride : 4;
                address = reinterpret_cast<u64>(resource->own().title_memory) + first_element * element;
                bytes = std::size_t{count} * element;
            }
            if (bytes > (256u << 20)) bytes = 256u << 20;
            ID3D12Resource* window = nullptr;
            u64 offset = 0;
            if (address && bytes && desktop_range(address, bytes, &window, &offset) && (offset & 15) == 0) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32_TYPELESS;
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Buffer.FirstElement = offset / 4;
                sd.Buffer.NumElements = static_cast<UINT>((bytes + 3) / 4);
                sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                device_of(the_device)->CreateShaderResourceView(window, &sd, at(4));
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = offset / 4;
                ud.Buffer.NumElements = sd.Buffer.NumElements;
                ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                device_of(the_device)->CreateUnorderedAccessView(window, nullptr, &ud, at(7));
                done[4] = done[7] = true;
            }
            if (vinfo_words && record->id < view_ids_max) vinfo_words[record->id * 4] = stride;
        }
    }
    for (UINT k = 0; k < 8; ++k) {
        if (done[k]) {
            InterlockedIncrement(&views_bound);
            continue;
        }
        null_view(kinds[k], at(k), kinds[k].uav);
        InterlockedDecrement(&cx_null_res);
        if (record) InterlockedIncrement(&views_null);
    }
}
// draw_lock held.
void fill_views(Wrapper* self)
{
    Timed timed(time_cx_fill_views);
    if (!vinfo_buffer) {
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = u64{view_ids_max} * 16;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_of(the_device)->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vinfo_buffer))))
            return;
        vinfo_buffer->Map(0, nullptr, reinterpret_cast<void**>(&vinfo_words));
        std::memset(vinfo_words, 0, u64{view_ids_max} * 16);
        fill_view_kinds(self, draw_heap_size, nullptr);  // the empty views
    }
    AcquireSRWLockShared(&note_lock);
    const UINT count = static_cast<UINT>(view_records().size() < view_ids_max ? view_records().size() : view_ids_max);
    ReleaseSRWLockShared(&note_lock);
    const UINT from = views_filled;
    for (UINT i = views_filled; i < count; ++i) {
        AcquireSRWLockShared(&note_lock);
        const ViewRecord record = view_records()[i];
        ReleaseSRWLockShared(&note_lock);
        fill_view_kinds(self, draw_heap_size + 8 + 8 * i, &record);
    }
    views_filled = count;
    if (count != from) {
        static LONG logged;
        if (first_calls(logged, 12)) log("x2w-cx views %u..%u filled; bound %ld, empty %ld", from, count, views_bound, views_null);
    }
}

// The title heap behind the bound compute tables (else the newest shader-visible view heap).
bool title_view_heap(const Pass& pass, u64* address, u64* bytes)
{
    AcquireSRWLockShared(&note_lock);
    const HeapInfo* best = nullptr;
    for (const HeapInfo& h : heap_infos()) {
        if (h.type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !h.title_cpu || !h.desktop_gpu) continue;
        for (UINT i = 0; i < 16; ++i)
            if (pass.compute_kind[i] == 5 && pass.compute_address[i] >= h.title_cpu && pass.compute_address[i] < h.title_cpu + u64{h.count} * h.increment)
                best = &h;
        if (!best) best = &h;
    }
    if (best) {
        *address = best->title_cpu;
        *bytes = u64{best->count} * best->increment;
    }
    ReleaseSRWLockShared(&note_lock);
    return best != nullptr;
}

// Runs a dispatch through the translation of compute key `key`. False: not translated (or no list).
bool execute_translated(Wrapper* self, ID3D12GraphicsCommandList* list, u64 key, UINT x, UINT y, UINT z)
{
    static const bool enabled = !x2w_local::env_on("X2W_CX_OFF");
    if (!enabled || !list || (key >> 56) != 0x67) return false;
    Timed timed(time_cx_execute);
    Pass& pass = pass_of(self);
    AcquireSRWLockExclusive(&draw_lock); draw_lock_taken(200000 + __LINE__);
    ComputeTranslation& tr = compute_translations()[key];
    if (!tr.state) load_compute(key, tr);
    if (tr.state != 1 || !ensure_draw_heap(the_device)) {
        ReleaseSRWLockExclusive(&draw_lock);
        return false;
    }
    D3D12_GPU_VIRTUAL_ADDRESS heap_va = 0;
    if (tr.heap) {
        u64 heap_address = 0, heap_bytes = 0;
        static LONG logged_heap;
        if (draw_heap_increment != 32 || !title_view_heap(pass, &heap_address, &heap_bytes) || !(heap_va = desktop_va(heap_address))) {
            if (first_calls(logged_heap, 4)) log("x2w-cx %016llx: title view heap not reachable (increment %u)", key, draw_heap_increment);
            ReleaseSRWLockExclusive(&draw_lock);
            return false;
        }
        fill_views(self);
        if (!vinfo_buffer) {
            ReleaseSRWLockExclusive(&draw_lock);
            return false;
        }
    }
    const UINT n = static_cast<UINT>(tr.res.size());
    UINT first = static_cast<UINT>(InterlockedExchangeAdd(&draw_heap_cursor, static_cast<LONG>(2 * n))) % draw_heap_size;
    if (first + 2 * n > draw_heap_size) {
        first = 0;
        InterlockedExchange(&draw_heap_cursor, static_cast<LONG>(2 * n));
    }
    std::uint32_t strides[64] = {}, word3[64] = {}, bases[64] = {};
    const bool show = InterlockedIncrement(&tr.logged) <= 2;
    for (UINT i = 0; i < n; ++i) {
        const ComputeTranslation::Res& r = tr.res[i];
        const SIZE_T inc = draw_heap_increment;
        const D3D12_CPU_DESCRIPTOR_HANDLE srv{draw_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{first + i} * inc};
        const D3D12_CPU_DESCRIPTOR_HANDLE uav{draw_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{first + n + i} * inc};
        // the unused half of each pair stays null
        null_view(r, r.uav ? srv : uav, !r.uav);
        InterlockedDecrement(&cx_null_res);
        const D3D12_CPU_DESCRIPTOR_HANDLE dest = r.uav ? uav : srv;
        u64 entry = 0, address = 0;
        std::size_t bytes = 0;
        if (r.source == 0 && r.a < 16 && pass.compute_kind[r.a] == 5 && pass.compute_address[r.a]) entry = pass.compute_address[r.a] + static_cast<u64>(r.b);
        if (r.source == 2 && r.a < 16 && pass.compute_kind[r.a] >= 2 && pass.compute_kind[r.a] <= 4) {
            address = pass.compute_address[r.a];
            bytes = readable_window(address, 1u << 20);
        }
        std::uint32_t words[8] = {};
        if (entry && x2w_local::readable(reinterpret_cast<const void*>(entry), 32)) std::memcpy(words, reinterpret_cast<const void*>(entry), 32);
        ViewRecord record{};
        bool known = false;
        if (words[0] == view_token_magic) {
            AcquireSRWLockShared(&note_lock);
            known = words[1] < view_records().size();
            if (known) record = view_records()[words[1]];
            ReleaseSRWLockShared(&note_lock);
        } else if (entry) {
            word3[i] = words[3];
        }
        bool bound = false;
        if (r.kind) {
            bound = known && texture_view(self, r, record, dest);
        } else {
            if (known && record.slot == 17 && record.desc_bytes >= 12) {
                std::uint32_t size = 0;
                std::memcpy(&address, record.desc, 8);
                std::memcpy(&size, record.desc + 8, 4);
                bytes = size;
            } else if (known && (record.slot == 18 || record.slot == 19) && record.desc_bytes >= 24) {
                Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
                D3D12_SRV_DIMENSION dim{};
                std::memcpy(&dim, record.desc + 4, 4);
                if (dim == 1 /* BUFFER in both SRV and UAV enums */ && resource && resource->own().title_memory) {
                    u64 first_element = 0;
                    std::uint32_t count = 0, stride = 0;
                    const std::size_t at = record.slot == 18 ? 16 : 8;
                    std::memcpy(&first_element, record.desc + at, 8);
                    std::memcpy(&count, record.desc + at + 8, 4);
                    std::memcpy(&stride, record.desc + at + 12, 4);
                    const UINT element = stride ? stride : 4;
                    address = reinterpret_cast<u64>(resource->own().title_memory) + first_element * element;
                    bytes = std::size_t{count} * element;
                    strides[i] = stride;
                }
            } else if (entry && words[0] != view_token_magic) {
                address = words[0] | (static_cast<u64>(words[1] & 0xffff) << 32);
                strides[i] = (words[1] >> 16) & 0x3fff;
                bytes = static_cast<std::size_t>(strides[i] ? strides[i] : 1) * words[2];
            }
            if (bytes > (256u << 20)) bytes = 256u << 20;
            ID3D12Resource* window = nullptr;
            u64 offset = 0;
            if (address && bytes && desktop_range(address, bytes, &window, &offset)) {
                const u64 aligned = offset & ~15ull;
                bases[i] = static_cast<std::uint32_t>(offset - aligned);
                const UINT elements = static_cast<UINT>((bytes + bases[i] + 3) / 4);
                if (r.uav) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
                    d.Format = DXGI_FORMAT_R32_TYPELESS;
                    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    d.Buffer.FirstElement = aligned / 4;
                    d.Buffer.NumElements = elements;
                    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                    device_of(the_device)->CreateUnorderedAccessView(window, nullptr, &d, dest);
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
                    d.Format = DXGI_FORMAT_R32_TYPELESS;
                    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    d.Buffer.FirstElement = aligned / 4;
                    d.Buffer.NumElements = elements;
                    d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                    device_of(the_device)->CreateShaderResourceView(window, &d, dest);
                }
                bound = true;
            }
        }
        if (!bound) null_view(r, dest, r.uav);
        if (show)
            log("x2w-cx   %016llx %s %u kind %d source %d(%d,%d): entry 0x%llx words %08x %08x %08x %08x -> address 0x%llx %zu bytes stride %u: %s",
                key, r.uav ? "uav" : "srv", i, r.kind, r.source, r.a, r.b, entry, words[0], words[1], words[2], words[3], address, bytes,
                strides[i], bound ? "bound" : "NULL");
    }
    std::uint32_t values[64] = {};
    for (std::size_t i = 0; i < tr.uds.size(); ++i) {
        const ComputeTranslation::Ud& ud = tr.uds[i];
        const bool in = ud.a >= 0 && ud.a < 64;
        values[i] = ud.kind == 3 ? ((ud.a < 16 && ud.b < 16) ? pass.compute_constants[ud.a][ud.b] : 0)
            : ud.kind == 4 ? (in ? strides[ud.a] : 0) : ud.kind == 5 ? (in ? word3[ud.a] : 0) : (in ? bases[ud.a] : 0);
    }
    list->SetPipelineState(tr.pso);
    if (!pass.heap_set) {
        list->SetDescriptorHeaps(1, &draw_heap);
        pass.heap_set = true;
        pass.bound_sampler_heap = nullptr;  // this call unbinds any sampler heap a draw bound
    }
    list->SetComputeRootSignature(tr.root);
    list->SetComputeRoot32BitConstants(0, tr.uds.empty() ? 1u : static_cast<UINT>(tr.uds.size()), values, 0);
    if (tr.heap) {
        list->SetComputeRootShaderResourceView(1, heap_va);
        list->SetComputeRootShaderResourceView(2, vinfo_buffer->GetGPUVirtualAddress());
        if (n) list->SetComputeRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{draw_heap->GetGPUDescriptorHandleForHeapStart().ptr + u64{first} * draw_heap_increment});
    } else if (n) {
        list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{draw_heap->GetGPUDescriptorHandleForHeapStart().ptr + u64{first} * draw_heap_increment});
    }
    ReleaseSRWLockExclusive(&draw_lock);
    list->Dispatch(x, y, z);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    const LONG done = InterlockedIncrement(&cx_dispatched);
    if (done <= 8 || done % 5000 == 0)
        log("x2w-cx dispatch %ld: %016llx (%u, %u, %u); ready %ld missing %ld null %ld bad-uav %ld; ranges ok %ld split %ld unmapped %ld", done, key, x, y, z,
            cx_ready, cx_missing, cx_null_res, cx_bad_uav, range_ok, range_split, range_unmapped);
    if ((done & 63) == 1) check_device("translated dispatch", 0);
    return true;
}
