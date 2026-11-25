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
    struct Res { bool uav; int kind, source, a, b; bool integer = false; };  // kind 0 buffer, 1 2D, 2 2D array, 3 3D, 4 1D; source 0 table(param a, byte b), 2 root a
                                                                             // integer: R32_UINT texture UAV (manifest kinds tex2du, tex3du)
    struct Ud { int kind, a, b; };                     // 3 root constant(param a, word b), 4 stride of res a, 5 word 3 of res a, 6 base of res a,
                                                       // 7 descriptor dword at byte b of table param a (descword)
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
    LONG entry_logged = 0;  // X2W_CX_ENTRY_LOG lines written
};
std::unordered_map<u64, ComputeTranslation>& compute_translations() { static std::unordered_map<u64, ComputeTranslation> m; return m; }
LONG cx_ready, cx_missing, cx_dispatched, cx_null_res, cx_bad_uav;
volatile LONG64 cx_compile_ms;  // LOCAL stall: driver compile time of every translation loaded this run

void load_compute(u64 key, ComputeTranslation& tr)
{
    tr.state = 2;
    // LOCAL stall, fixtures only: X2W_CX_TEST_HOLD_EVENT (d3d12_draw.inl test_hold), so a test can issue a
    // dispatch while its pipeline is deliberately not ready.
    test_hold("x2w-cx", key);
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
            else if (!std::strcmp(source, "descword")) ud.kind = 7;
            if (ud.kind < 0 || static_cast<std::size_t>(i) != tr.uds.size()) {
                log("x2w-cx %016llx: entry value %d from %s not supported", key, i, source);
                return;
            }
            tr.uds.push_back(ud);
        } else if (std::sscanf(line.c_str(), "%7s %d %15s %15s %d %d", view, &i, kind, source, &x, &y) >= 5 &&
                   (!std::strcmp(view, "srv") || !std::strcmp(view, "uav"))) {
            ComputeTranslation::Res r{view[0] == 'u', !std::strcmp(kind, "tex2d") ? 1 : !std::strcmp(kind, "tex2da") ? 2 : !std::strcmp(kind, "tex3d") ? 3 : !std::strcmp(kind, "tex1d") ? 4 : 0,
                                      !std::strcmp(source, "root") ? 2 : 0, x, y};
            if (!std::strcmp(kind, "tex2du") || !std::strcmp(kind, "tex3du")) {
                r.kind = kind[3] == '2' ? 1 : 3;
                r.integer = true;
                if (!r.uav) {
                    log("x2w-cx %016llx: integer texture kind %s only as a uav", key, kind);
                    return;
                }
            }
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
    LARGE_INTEGER compile_start{}, compile_end{}, frequency{};
    QueryPerformanceCounter(&compile_start);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = tr.root;
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        hr = device_of(the_device)->CreateComputePipelineState(&pd, IID_PPV_ARGS(&tr.pso));
    }
    QueryPerformanceCounter(&compile_end);
    QueryPerformanceFrequency(&frequency);
    const SIZE_T code_bytes = code->GetBufferSize();
    code->Release();
    InterlockedAdd64(&cx_compile_ms, (compile_end.QuadPart - compile_start.QuadPart) * 1000 / frequency.QuadPart);
    // LOCAL: the driver compile time and DXIL size, so a run shows which first uses are slow
    log("x2w-cx %016llx translated: %zu entry values, %u resources, %zu samplers, group %ux%ux%u -> 0x%08lx (%zu bytes, compiled in %lld ms)",
        key, tr.uds.size(), n, tr.samplers.size(), tr.threads[0], tr.threads[1], tr.threads[2], static_cast<unsigned long>(hr), code_bytes,
        static_cast<long long>((compile_end.QuadPart - compile_start.QuadPart) * 1000 / frequency.QuadPart));
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
        d.Format = r.integer ? DXGI_FORMAT_R32_UINT : r.kind ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_TYPELESS;
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

// LOCAL: integer texture UAVs (tex2du/tex3du) bound and refused, for the 300-frame report.
LONG cxu_bound, cxu_refused;
void note_integer_uav(bool bound, DXGI_FORMAT resource_format, UINT flags)
{
    InterlockedIncrement(bound ? &cxu_bound : &cxu_refused);
    static LONG logged_bound, logged_refused;
    if (bound ? first_calls(logged_bound, 1) : first_calls(logged_refused, 6))
        log("x2w-cx integer texture uav %s: resource format %u flags 0x%x%s", bound ? "bound as R32_UINT" : "REFUSED (null R32_UINT view)",
            resource_format, flags, bound ? "" : " (needs ALLOW_UNORDERED_ACCESS and an R32_TYPELESS or R32_UINT resource)");
}

// Binds a texture behind a view record as SRV or UAV (true when a real view was written).
// prepare: also upload a plain texture and move it to the view's state (false for the bulk heap fill).
bool texture_view(Wrapper* self, const ComputeTranslation::Res& r, const ViewRecord& record, D3D12_CPU_DESCRIPTOR_HANDLE dest, bool prepare = true)
{
    Timed timed(time_cx_texture);
    Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
    if (prepare && resource && resource->own().kind == resource_kind) memcut_revive(resource);  // LOCAL memcut: X2W_MEMCUT_RELEASE
    ID3D12Resource* texture = resource && resource->own().kind == resource_kind ? static_cast<ID3D12Resource*>(resource->own().real) : nullptr;
    if (!texture || record.desc_bytes < 24) return false;
    if (resource->own().refs <= 0) x2w_memcut::note_after_release(true);  // LOCAL memcut
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
        if (r.integer) format = x2w_startup::integer_uav_format(rd.Format);
        if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || (r.integer ? format == DXGI_FORMAT_UNKNOWN : !float_format(format))) {
            InterlockedIncrement(&cx_bad_uav);
            if (r.integer) note_integer_uav(false, rd.Format, rd.Flags);
            return false;
        }
        if (r.integer) note_integer_uav(true, rd.Format, rd.Flags);
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
// vinfo (an upload buffer, 16 bytes per id): word 0 the index stride in bytes (structured: the structure stride,
// typed: the element size, raw: 0), word 1 the GFX10_FORMAT code of a typed view (0: raw, structured or unmapped).
// Filled up to the newest record.
constexpr UINT view_ids_max = 90000;
UINT views_filled;
ID3D12Resource* vinfo_buffer;
std::uint32_t* vinfo_words;
LONG views_bound, views_null;
// LOCAL: typed buffer views (FirstElement x element size, index stride = element size). X2W_CX_TYPED_OFF=1 restores
// the old layout (element 4 bytes, stride 0, no format) for heap views, table views and bufword3.
bool typed_views_off()
{
    static const bool off = x2w_local::env_on("X2W_CX_TYPED_OFF");
    return off;
}
LONG cxv_typed_heap, cxv_typed_table, cxv_misaligned, cxv_word3;  // heap: views filled; table: bindings
LONG cxd_image, cxd_buffer, cxd_title, cxd_null, cxd_unmapped;  // descword values by source
LONG cxe_rootconst, cxe_descword, cxe_null_res;  // entry values: unset root constant, unbound descword table, of a null resource
// LOCAL: X2W_CX_ENTRY_LOG=1 (every key) or <key>[,<key>...] (hex): the entry values of the first 4 dispatches per key.
bool cx_entry_log(u64 key)
{
    static bool all = false;
    static const std::vector<u64> keys = [] {
        std::vector<u64> out;
        char value[2048]{};
        const DWORD n = GetEnvironmentVariableA("X2W_CX_ENTRY_LOG", value, sizeof(value));
        if (!n || n >= sizeof(value)) return out;
        if (!std::strcmp(value, "1")) {
            all = true;
            log("x2w-cx X2W_CX_ENTRY_LOG: every key, first 4 dispatches each");
            return out;
        }
        for (const char* p = value; *p;) {
            char* end = nullptr;
            const u64 k = std::strtoull(p, &end, 16);
            if (end == p) {
                ++p;
                continue;
            }
            out.push_back(k);
            p = end;
        }
        log("x2w-cx X2W_CX_ENTRY_LOG: %zu keys, first 4 dispatches each", out.size());
        return out;
    }();
    return all || (!keys.empty() && std::find(keys.begin(), keys.end(), key) != keys.end());
}
void note_typed_view(const x2w_startup::BufferViewLayout& l, const ViewRecord& record, bool heap)
{
    if (l.kind != x2w_startup::BufferViewKind::typed) return;
    InterlockedIncrement(heap ? &cxv_typed_heap : &cxv_typed_table);
    static LONG logged;
    if (first_calls(logged, 6)) {
        std::uint32_t format = 0;
        std::memcpy(&format, record.desc, 4);
        log("x2w-cx typed buffer view %u (%s): format %u -> gfx10 format %u, %u-byte elements from byte %llu, %llu bytes%s", record.id, heap ? "heap" : "table", format,
            l.format.code, l.index_stride, static_cast<unsigned long long>(l.offset), static_cast<unsigned long long>(l.bytes),
            typed_views_off() ? " (X2W_CX_TYPED_OFF)" : "");
    }
}
// Called every 300 frames (render mode): view facts handed to translated programs since the last report.
void report_cx_views()
{
    log("x2w-cx view facts (300 frames): typed buffer views heap %ld table %ld (heap misaligned %ld), bufword3 synthesized %ld%s; "
        "descword values image %ld buffer %ld title %ld null %ld (unmapped format %ld); integer texture uavs bound %ld refused %ld",
        InterlockedExchange(&cxv_typed_heap, 0), InterlockedExchange(&cxv_typed_table, 0), InterlockedExchange(&cxv_misaligned, 0),
        InterlockedExchange(&cxv_word3, 0), typed_views_off() ? " (X2W_CX_TYPED_OFF)" : "", InterlockedExchange(&cxd_image, 0),
        InterlockedExchange(&cxd_buffer, 0), InterlockedExchange(&cxd_title, 0), InterlockedExchange(&cxd_null, 0), InterlockedExchange(&cxd_unmapped, 0),
        InterlockedExchange(&cxu_bound, 0), InterlockedExchange(&cxu_refused, 0));
    log("x2w-cx entry values (300 frames): root constant not set %ld, descword table not bound %ld (these dispatches %s), "
        "of a null resource %ld (run); SetComputeRootSignature same %ld changed %ld%s",
        InterlockedExchange(&cxe_rootconst, 0), InterlockedExchange(&cxe_descword, 0),
        x2w_local::env_on("X2W_CX_UNRESOLVED_RUN") ? "run, X2W_CX_UNRESOLVED_RUN" : "skipped", InterlockedExchange(&cxe_null_res, 0),
        InterlockedExchange(&compute_root_same, 0), InterlockedExchange(&compute_root_changed, 0),
        x2w_local::env_on("X2W_CX_ROOT_RESET") ? " (X2W_CX_ROOT_RESET: both clear)" : "");
}
void vdesc_fill(const ViewRecord& record);  // LOCAL meshrt: d3d12_vdesc.inl (`heapwords 1`; inert until a manifest asks)
void fill_view_kinds(Wrapper* self, UINT base, const ViewRecord* record)
{
    if (record) vdesc_fill(*record);  // LOCAL meshrt
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
            std::uint32_t stride = 0, format_code = 0;
            bool typed = false;
            if (record->slot == 17 && record->desc_bytes >= 12) {
                std::uint32_t size = 0;
                std::memcpy(&address, record->desc, 8);
                std::memcpy(&size, record->desc + 8, 4);
                bytes = size;
            } else if ((record->slot == 18 || record->slot == 19) && record->desc_bytes >= 24 && resource && resource->own().title_memory) {
                x2w_startup::BufferViewLayout l;
                if (x2w_startup::buffer_view_layout_from_desc(record->slot == 19, record->desc, record->desc_bytes, l, typed_views_off())) {
                    address = reinterpret_cast<u64>(resource->own().title_memory) + l.offset;
                    bytes = static_cast<std::size_t>(l.bytes);
                    stride = l.index_stride;
                    format_code = l.format.code;
                    note_typed_view(l, *record, true);
                    typed = l.kind == x2w_startup::BufferViewKind::typed;
                }
            }
            if (bytes > (256u << 20)) bytes = 256u << 20;
            ID3D12Resource* window = nullptr;
            u64 offset = 0;
            const bool in_window = address && bytes && desktop_range(address, bytes, &window, &offset);
            if (in_window && (offset & 15) && typed) InterlockedIncrement(&cxv_misaligned);  // raw views must start 16-aligned
            if (in_window && (offset & 15) == 0) {
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
            if (vinfo_words && record->id < view_ids_max) {
                vinfo_words[record->id * 4] = stride;
                vinfo_words[record->id * 4 + 1] = format_code;
            }
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

// The 8 gfx10 descriptor words the console would hold for a view the title created (gfx10_view_words.h): a CBV
// or buffer view gives the V# (raw for a CBV), a texture view the T# from the view and its resource. False: no
// words (unknown slot, unreadable desc, texture view of an unknown resource). Used by descword and vdesc
// (meshrt, `heapwords 1`); no counters here.
bool view_record_words(const ViewRecord& record, std::uint32_t out[8], bool* image, bool* unmapped)
{
    std::memset(out, 0, 32);
    *image = *unmapped = false;
    Wrapper* resource = as_wrapper(reinterpret_cast<const void*>(record.resource));
    ID3D12Resource* real = resource && resource->own().kind == resource_kind ? static_cast<ID3D12Resource*>(resource->own().real) : nullptr;
    x2w_startup::BufferViewLayout l;
    if (record.slot == 17 && record.desc_bytes >= 12) {
        std::uint32_t size = 0;
        std::memcpy(&size, record.desc + 8, 4);
        l = x2w_startup::buffer_view_layout(DXGI_FORMAT_R32_TYPELESS, 0, size / 4, 0, 1);  // a CBV reads as a raw buffer
    } else if ((record.slot == 18 || record.slot == 19) &&
               x2w_startup::buffer_view_layout_from_desc(record.slot == 19, record.desc, record.desc_bytes, l, typed_views_off())) {
    } else if (real && (record.slot == 18 || record.slot == 19)) {
        x2w_startup::ImageViewFacts v;
        if (!x2w_startup::image_view_from_desc(record.slot == 19, record.desc, record.desc_bytes, v)) return false;
        const D3D12_RESOURCE_DESC rd = real->GetDesc();
        v.dimension = rd.Dimension;
        v.width = rd.Width;
        v.height = rd.Height;
        v.depth_or_array = rd.DepthOrArraySize;
        v.mip_levels = rd.MipLevels;
        v.format = typeless_to_float(desktop_format(v.format ? v.format : rd.Format));
        return *image = x2w_startup::gfx10_image_words(v, out, unmapped);
    } else {
        return false;
    }
    x2w_startup::gfx10_buffer_words(l, out);
    return true;
}

// LOCAL: `udsrc <i> descword <param> <byte offset>`: the dword at <byte offset> of the descriptor table bound at root
// parameter <param>, as the console would hold it. Entries are 32 bytes: entry = offset & ~31, word = (offset & 31) / 4.
// A view token becomes the synthesized gfx10 T# (texture view) or V# (buffer view, CBV) word (gfx10_view_words.h);
// any other readable entry gives the title's own dword; null, unreadable or unknown views give 0.
// resolved: false when the table is not bound or its entry is not readable (the value 0 is then not the title's).
std::uint32_t descword_value(const Pass& pass, int param, int offset, u64 key, bool* resolved = nullptr)
{
    if (resolved) *resolved = false;
    if (param < 0 || param >= 16 || offset < 0 || pass.compute_kind[param] != 5 || !pass.compute_address[param]) {
        static LONG logged;
        if (first_calls(logged, 4))
            log("x2w-cx %016llx descword %d %d: root parameter is not a bound descriptor table (kind %d): value 0", key, param, offset,
                param >= 0 && param < 16 ? pass.compute_kind[param] : -1);
        InterlockedIncrement(&cxd_null);
        return 0;
    }
    const u64 entry = pass.compute_address[param] + (static_cast<u64>(offset) & ~31ull);
    const int word = (offset & 31) / 4;
    std::uint32_t words[8] = {};
    if (!x2w_local::readable(reinterpret_cast<const void*>(entry), 32)) {
        InterlockedIncrement(&cxd_null);
        return 0;
    }
    std::memcpy(words, reinterpret_cast<const void*>(entry), 32);
    if (resolved) *resolved = true;
    if (words[0] != view_token_magic) {
        InterlockedIncrement(words[word] ? &cxd_title : &cxd_null);
        return words[word];
    }
    ViewRecord record{};
    AcquireSRWLockShared(&note_lock);
    const bool known = words[1] < view_records().size();
    if (known) record = view_records()[words[1]];
    ReleaseSRWLockShared(&note_lock);
    std::uint32_t out[8] = {};
    bool image = false, unmapped = false;
    const bool made = known && view_record_words(record, out, &image, &unmapped);
    if (made && unmapped) {
        InterlockedIncrement(&cxd_unmapped);
        static LONG logged;
        if (first_calls(logged, 8)) log("x2w-cx descword: view %u has no gfx10 format here: FORMAT 0 (word 1 may then read as a null view)", record.id);
    }
    if (!made) {
        InterlockedIncrement(&cxd_null);
        return 0;
    }
    InterlockedIncrement(image ? &cxd_image : &cxd_buffer);
    static LONG logged_image, logged_buffer;
    if (first_calls(image ? logged_image : logged_buffer, 1))
        log("%s", image ? "x2w-cx descword: image views give T# FORMAT, WIDTH, HEIGHT, DST_SEL, BASE_LEVEL, LAST_LEVEL, TYPE, DEPTH, BASE_ARRAY, "
                    "ARRAY_PITCH, MAX_MIP; left 0: base address, MIN_LOD, RESOURCE_LEVEL, SW_MODE, BC_SWIZZLE, PERF_MOD, words 6-7"
                  : "x2w-cx descword: buffer views give V# STRIDE, NUM_RECORDS, DST_SEL, FORMAT, OOB_SELECT; left 0: base address, "
                    "SWIZZLE_ENABLE, CACHE_SWIZZLE, INDEX_STRIDE, ADD_TID_ENABLE, RESOURCE_LEVEL, words 4-7");
    static LONG logged_value;
    if (first_calls(logged_value, 8))
        log("x2w-cx %016llx descword %d %d: view %u slot %u %s word %d = %08x", key, param, offset, record.id, record.slot, image ? "T#" : "V#", word, out[word]);
    return out[word];
}

// LOCAL: the translation of `key`, loaded on first use without holding draw_lock. A driver compile of
// a large translated shader takes seconds (run 85: >= 11 s in amdxc64 for one key); under draw_lock it
// blocked every other recording thread and ExecuteCommandLists, so the render thread could not finish
// a frame and the title's frame-loop watchdog stopped it. Different keys now compile in parallel; a
// thread that needs a key another thread is compiling waits for that key only. State 3 = loading.
SRWLOCK compute_load_lock = SRWLOCK_INIT;
CONDITION_VARIABLE compute_loaded = CONDITION_VARIABLE_INIT;
ComputeTranslation& loaded_compute(u64 key)
{
    AcquireSRWLockExclusive(&compute_load_lock);
    ComputeTranslation& tr = compute_translations()[key];  // element references survive rehashing
    while (tr.state == 3) SleepConditionVariableSRW(&compute_loaded, &compute_load_lock, INFINITE, 0);
    if (tr.state != 0) {
        ReleaseSRWLockExclusive(&compute_load_lock);
        return tr;
    }
    tr.state = 3;
    ReleaseSRWLockExclusive(&compute_load_lock);
    ComputeTranslation loaded;
    load_compute(key, loaded);
    AcquireSRWLockExclusive(&compute_load_lock);
    tr = loaded;  // final state (1 ready, 2 unavailable) and the objects; nobody reads tr while it is 3
    ReleaseSRWLockExclusive(&compute_load_lock);
    WakeAllConditionVariable(&compute_loaded);
    return tr;
}

// LOCAL stall: the translation of a slot 67 pipeline is loaded (and its desktop pipeline compiled by the
// driver) when the title creates the pipeline, on two below-normal background threads, so the first
// dispatch usually finds it ready instead of compiling on a recording thread inside the frame. A
// dispatch of a key still queued compiles it itself; one of a key being compiled waits for that key
// only (loaded_compute). X2W_CX_PREWARM_OFF=1 (or X2W_CX_COMPILE_LOCKED=1) turns this off;
// X2W_CX_PREWARM_THREADS=<1..4> sets the thread count.
SRWLOCK prewarm_lock = SRWLOCK_INIT;
CONDITION_VARIABLE prewarm_wanted = CONDITION_VARIABLE_INIT;
std::deque<u64>& prewarm_queue()
{
    static std::deque<u64> q;
    return q;
}
std::unordered_set<u64>& prewarm_keys()  // every key ever queued (prewarm_lock): each is queued once
{
    static std::unordered_set<u64> s;
    return s;
}
LONG cx_prewarm_queued, cx_prewarm_done;
// LOCAL stall: X2W_CX_ASYNC=1. A dispatch whose pipeline is not loaded yet is skipped (outcome co_c67_compiling,
// "skipped while compiling") and its key goes to the front of the prewarm queue; the recording thread never
// compiles and never waits for a compile. A skipped dispatch is dropped, not replayed later. Counted per key here
// (compute_load_lock), outside ComputeTranslation, which loaded_compute overwrites when a load completes.
struct CxAsync {
    LONG skipped = 0;         // since the run started
    LONG since_report = 0;    // since the last report_cx_async
    ULONGLONG first_skip = 0;  // GetTickCount64 of the first skip
};
std::unordered_map<u64, CxAsync>& cx_async_keys()
{
    static std::unordered_map<u64, CxAsync> m;
    return m;
}
bool cx_async()
{
    static const bool on = x2w_local::env_on("X2W_CX_ASYNC") && !x2w_local::env_on("X2W_CX_COMPILE_LOCKED");
    return on;
}
// LOCAL stall: X2W_CX_PREWARM_ALL=1 (prewarm_known_start, below) queues the known keys when the device is made.
// known_keys is filled before any of them is queued and not changed after, so the workers read it unlocked.
std::unordered_set<u64>& known_keys()
{
    static std::unordered_set<u64> s;
    return s;
}
LONG cx_known_done;
ULONGLONG cx_known_started;
// The prewarm workers. A key with skipped dispatches logs when it is ready.
DWORD WINAPI prewarm_thread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        AcquireSRWLockExclusive(&prewarm_lock);
        while (prewarm_queue().empty()) SleepConditionVariableSRW(&prewarm_wanted, &prewarm_lock, INFINITE, 0);
        const u64 key = prewarm_queue().front();
        prewarm_queue().pop_front();
        ReleaseSRWLockExclusive(&prewarm_lock);
        const int state = loaded_compute(key).state;
        const LONG n = InterlockedIncrement(&cx_prewarm_done);
        if (n <= 4 || n % 200 == 0) log("x2w-cx prewarm %ld of %ld queued pipelines loaded (latest %016llx)", n, cx_prewarm_queued, key);
        if (known_keys().count(key)) {
            const LONG done = InterlockedIncrement(&cx_known_done);
            if (done == static_cast<LONG>(known_keys().size()))
                log("x2w-cx prewarm: all %ld known pipelines loaded %.1f s after the device was made (X2W_CX_PREWARM_ALL); "
                    "driver compile time of all translations so far %.1f s", done, (GetTickCount64() - cx_known_started) / 1000.0,
                    static_cast<double>(cx_compile_ms) / 1000.0);
        }
        if (cx_async()) {
            AcquireSRWLockExclusive(&compute_load_lock);
            const auto found = cx_async_keys().find(key);
            const CxAsync seen = found != cx_async_keys().end() ? found->second : CxAsync{};
            ReleaseSRWLockExclusive(&compute_load_lock);
            if (seen.skipped)
                log("x2w-cx %016llx %s (X2W_CX_ASYNC) %llu ms after its first skipped dispatch; %ld dispatches skipped while compiling",
                    key, state == 1 ? "ready" : "unavailable", static_cast<unsigned long long>(GetTickCount64() - seen.first_skip), seen.skipped);
        }
    }
}
bool prewarm_enabled()
{
    static const bool off = x2w_local::env_on("X2W_CX_PREWARM_OFF") || x2w_local::env_on("X2W_CX_COMPILE_LOCKED") ||
                            x2w_local::env_on("X2W_CX_OFF") || x2w_local::env_on("X2W_COMPUTE_OFF");
    return !off;
}
// Queues `key` for the workers once. `front`: a dispatch needs it now (X2W_CX_ASYNC); a key still waiting in the
// queue moves to the front. With X2W_CX_ASYNC the workers run even under X2W_CX_PREWARM_OFF (then only demanded
// keys are queued). False: the key cannot be queued (prewarm off, or not a loadable slot-67 key).
bool prewarm_compute(u64 key, bool front)
{
    if ((!prewarm_enabled() && !(front && cx_async())) || (key >> 56) != 0x67 || !(key & 0x00ffffffffffffffull)) return false;
    static const int threads = [] {
        char value[4]{};
        const DWORD n = GetEnvironmentVariableA("X2W_CX_PREWARM_THREADS", value, sizeof(value));
        int count = n && n < sizeof(value) ? std::atoi(value) : x2w_local::env_on("X2W_CX_PREWARM_ALL") ? 3 : 2;
        count = count < 1 ? 1 : count > 4 ? 4 : count;
        for (int i = 0; i < count; ++i)
            if (const HANDLE t = CreateThread(nullptr, 0, prewarm_thread, nullptr, 0, nullptr)) CloseHandle(t);
        log("x2w-cx prewarm: %d background threads%s", count,
            cx_async() ? "; X2W_CX_ASYNC=1: a dispatch of a pipeline not loaded yet is skipped, never compiled or waited for on its thread" : "");
        return count;
    }();
    (void)threads;
    AcquireSRWLockExclusive(&prewarm_lock);
    const bool fresh = prewarm_keys().insert(key).second;
    if (fresh) {
        ++cx_prewarm_queued;
        if (front) prewarm_queue().push_front(key);
        else prewarm_queue().push_back(key);
    } else if (front) {
        std::deque<u64>& q = prewarm_queue();
        for (auto it = q.begin(); it != q.end(); ++it)
            if (*it == key) {
                q.erase(it);
                q.push_front(key);
                break;
            }  // not in the queue: a worker is loading it or has loaded it
    }
    ReleaseSRWLockExclusive(&prewarm_lock);
    if (fresh) WakeConditionVariable(&prewarm_wanted);
    return true;
}
// X2W_CX_ASYNC: the translation of `key` if its load has finished (state 1 ready or 2 unavailable), else nullptr
// after counting the skip and queueing the key at the front. A key the workers cannot take is loaded here as before.
ComputeTranslation* ready_compute(u64 key)
{
    AcquireSRWLockExclusive(&compute_load_lock);
    ComputeTranslation& tr = compute_translations()[key];
    if (tr.state == 1 || tr.state == 2) {
        ReleaseSRWLockExclusive(&compute_load_lock);
        return &tr;
    }
    CxAsync& a = cx_async_keys()[key];
    const bool first = !a.skipped;
    if (first) a.first_skip = GetTickCount64();
    ++a.skipped;
    ++a.since_report;
    ReleaseSRWLockExclusive(&compute_load_lock);
    if (!prewarm_compute(key, true)) {
        AcquireSRWLockExclusive(&compute_load_lock);
        --a.skipped, --a.since_report;  // element references survive rehashing
        ReleaseSRWLockExclusive(&compute_load_lock);
        return &loaded_compute(key);
    }
    static LONG logged;
    if (first && first_calls(logged, 32))
        log("x2w-cx %016llx: pipeline not loaded yet, dispatch skipped and the key queued first for the prewarm workers (X2W_CX_ASYNC)", key);
    return nullptr;
}
// Heartbeat (every 5 s): the dispatches skipped while compiling since the last report, per key.
void report_cx_async()
{
    if (!cx_async()) return;
    std::vector<std::pair<u64, LONG>> keys;
    LONG total = 0, all_time = 0;
    AcquireSRWLockExclusive(&compute_load_lock);
    for (auto& entry : cx_async_keys()) {
        all_time += entry.second.skipped;
        if (!entry.second.since_report) continue;
        keys.push_back({entry.first, entry.second.since_report});
        total += entry.second.since_report;
        entry.second.since_report = 0;
    }
    ReleaseSRWLockExclusive(&compute_load_lock);
    AcquireSRWLockShared(&prewarm_lock);
    const std::size_t waiting = prewarm_queue().size();
    ReleaseSRWLockShared(&prewarm_lock);
    if (!total) return;
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[1400];
    int n = std::snprintf(line, sizeof(line), "x2w-cx dispatches skipped while compiling since the last report (X2W_CX_ASYNC): %ld over %zu keys "
                          "(%ld this run; %zu keys waiting in the prewarm queue):", total, keys.size(), all_time, waiting);
    for (const auto& k : keys) {
        if (n > static_cast<int>(sizeof(line)) - 40) {
            n += std::snprintf(line + n, sizeof(line) - n, " ...");
            break;
        }
        n += std::snprintf(line + n, sizeof(line) - n, " %016llx=%ld", k.first, k.second);
    }
    log("%s", line);
}

// LOCAL stall: X2W_CX_PREWARM_ALL=1. When the device is made, every slot-67 key that has a translation in the
// cache (<key>.cs.cso) and is in the keys-seen file goes to the prewarm workers (3 by default), smallest .cso
// first; without a keys-seen file, every cached translation does. The keys-seen file (X2W_CX_KEYS_SEEN, default
// <X2W_SHADER_CACHE>\cx_keys_seen.lst: one key per line; not .txt, so the manifest watcher ignores it) holds the
// keys of earlier runs plus every key dispatched in this one; it is rewritten every 10 s when it changed (a
// temporary file and a rename), since a run usually ends without a clean exit.
bool prewarm_all()
{
    static const bool on = x2w_local::env_on("X2W_CX_PREWARM_ALL") && prewarm_enabled();
    return on;
}
SRWLOCK keys_seen_lock = SRWLOCK_INIT;
std::unordered_set<u64>& keys_seen()  // keys_seen_lock: the file's keys and the keys dispatched this run
{
    static std::unordered_set<u64> s;
    return s;
}
LONG keys_seen_changed;
bool keys_seen_path(wchar_t* out)
{
    DWORD n = GetEnvironmentVariableW(L"X2W_CX_KEYS_SEEN", out, MAX_PATH);
    if (n && n < MAX_PATH) return true;
    wchar_t dir[MAX_PATH];
    n = GetEnvironmentVariableW(L"X2W_SHADER_CACHE", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    swprintf(out, MAX_PATH, L"%s\\cx_keys_seen.lst", dir);
    return true;
}
// execute_translated: the key was dispatched (every dispatch; a shared lookup once the key is known).
void note_key_seen(u64 key)
{
    if (!prewarm_all()) return;
    AcquireSRWLockShared(&keys_seen_lock);
    const bool known = keys_seen().count(key) != 0;
    ReleaseSRWLockShared(&keys_seen_lock);
    if (known) return;
    AcquireSRWLockExclusive(&keys_seen_lock);
    if (keys_seen().insert(key).second) InterlockedIncrement(&keys_seen_changed);
    ReleaseSRWLockExclusive(&keys_seen_lock);
}
DWORD WINAPI keys_seen_writer(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    if (!keys_seen_path(path)) return 0;
    swprintf(temp, MAX_PATH, L"%s.%lu.tmp", path, GetCurrentProcessId());
    for (;;) {
        Sleep(10000);
        if (!InterlockedExchange(&keys_seen_changed, 0)) continue;
        std::string text;
        AcquireSRWLockShared(&keys_seen_lock);
        std::vector<u64> keys(keys_seen().begin(), keys_seen().end());
        ReleaseSRWLockShared(&keys_seen_lock);
        std::sort(keys.begin(), keys.end());
        for (const u64 key : keys) {
            char line[24];
            std::snprintf(line, sizeof(line), "%016llx\n", key);
            text += line;
        }
        const HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        CloseHandle(file);
        const bool moved = ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
        if (!moved) DeleteFileW(temp);
        static LONG logged;
        if (first_calls(logged, 4) || !moved) log("x2w-cx keys seen: %zu keys written to %ls%s", keys.size(), path, moved ? "" : " FAILED");
    }
}
// attach_device, after the_device is set.
void prewarm_known_start()
{
    if (!prewarm_all()) return;
    cx_known_started = GetTickCount64();
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    const DWORD dn = GetEnvironmentVariableW(L"X2W_SHADER_CACHE", dir, MAX_PATH);
    if (!dn || dn >= MAX_PATH) return;
    std::string text;
    const bool have_file = keys_seen_path(path) && read_text(path, text);
    std::size_t file_keys = 0;
    AcquireSRWLockExclusive(&keys_seen_lock);
    for (std::size_t at = 0; have_file && at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const u64 key = std::strtoull(text.substr(at, end - at).c_str(), nullptr, 16);
        if ((key >> 56) == 0x67 && keys_seen().insert(key).second) ++file_keys;
        at = end + 1;
    }
    ReleaseSRWLockExclusive(&keys_seen_lock);
    // The cached translations, smallest first.
    std::vector<std::pair<u64, u64>> cached;  // (size, key)
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%s\\67*.cs.cso", dir);
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t* tail = nullptr;
            const u64 key = std::wcstoull(found.cFileName, &tail, 16);
            if (!tail || tail - found.cFileName != 16 || _wcsicmp(tail, L".cs.cso") != 0) continue;
            if (have_file && !keys_seen().count(key)) continue;  // keys_seen is not written by another thread yet
            cached.push_back({(static_cast<u64>(found.nFileSizeHigh) << 32) | found.nFileSizeLow, key});
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    std::sort(cached.begin(), cached.end());
    for (const auto& c : cached) known_keys().insert(c.second);
    if (have_file)
        log("x2w-cx prewarm: %zu known pipelines queued (X2W_CX_PREWARM_ALL; keys seen file %ls: %zu keys, %zu with a translation), smallest first",
            cached.size(), path, file_keys, cached.size());
    else
        log("x2w-cx prewarm: %zu known pipelines queued (X2W_CX_PREWARM_ALL; no keys seen file: every cached translation), smallest first",
            cached.size());
    for (const auto& c : cached) prewarm_compute(c.second);
    if (const HANDLE t = CreateThread(nullptr, 0, keys_seen_writer, nullptr, 0, nullptr)) CloseHandle(t);
}
// Heartbeat, and the first translated dispatch: how many of the known pipelines are loaded.
void report_prewarm_known(const char* when)
{
    static LONG complete_logged;
    if (!prewarm_all() || known_keys().empty() || complete_logged) return;
    const LONG done = cx_known_done;
    if (done == static_cast<LONG>(known_keys().size())) InterlockedExchange(&complete_logged, 1);
    log("x2w-cx prewarm: %ld of %zu pipelines ready %s, %.1f s after the device was made; driver compile time so far %.1f s",
        done, known_keys().size(), when, (GetTickCount64() - cx_known_started) / 1000.0, static_cast<double>(cx_compile_ms) / 1000.0);
}

// Runs a dispatch through the translation of compute key `key`. False: not translated (or no list).
bool execute_translated(Wrapper* self, ID3D12GraphicsCommandList* list, u64 key, UINT x, UINT y, UINT z)
{
    static const bool enabled = !x2w_local::env_on("X2W_CX_OFF");
    if (!enabled || !list || (key >> 56) != 0x67) {
        dispatch_outcome = !enabled ? co_c67_off : list ? co_c67_other_key : co_not_run;
        return false;
    }
    Timed timed(time_cx_execute);
    Pass& pass = pass_of(self);
    // LOCAL: X2W_CX_COMPILE_LOCKED=1 restores the old order (compile while holding draw_lock), for comparison.
    static const bool compile_locked = x2w_local::env_on("X2W_CX_COMPILE_LOCKED");
    note_key_seen(key);  // LOCAL stall: X2W_CX_PREWARM_ALL keys-seen file
    static LONG first_dispatch;
    if (!first_dispatch && !InterlockedExchange(&first_dispatch, 1)) report_prewarm_known("at the first translated dispatch");
    // LOCAL stall: X2W_CX_ASYNC=1 skips a dispatch whose pipeline is not loaded yet (ready_compute).
    ComputeTranslation* ready = compile_locked ? nullptr : cx_async() ? ready_compute(key) : &loaded_compute(key);
    if (!compile_locked && !ready) {
        dispatch_outcome = co_c67_compiling;
        return false;
    }
    lock_draw(200000 + __LINE__);
    if (!ready) {
        ready = &compute_translations()[key];
        if (!ready->state) load_compute(key, *ready);
    }
    ComputeTranslation& tr = *ready;
    if (tr.state != 1 || !ensure_draw_heap(the_device)) {
        dispatch_outcome = tr.state != 1 ? co_c67_untranslated : co_c67_heap;
        ReleaseSRWLockExclusive(&draw_lock);
        return false;
    }
    D3D12_GPU_VIRTUAL_ADDRESS heap_va = 0;
    if (tr.heap) {
        u64 heap_address = 0, heap_bytes = 0;
        static LONG logged_heap;
        if (draw_heap_increment != 32 || !title_view_heap(pass, &heap_address, &heap_bytes) || !(heap_va = desktop_va(heap_address))) {
            if (first_calls(logged_heap, 4)) log("x2w-cx %016llx: title view heap not reachable (increment %u)", key, draw_heap_increment);
            dispatch_outcome = co_c67_heap;
            ReleaseSRWLockExclusive(&draw_lock);
            return false;
        }
        fill_views(self);
        if (!vinfo_buffer) {
            dispatch_outcome = co_c67_heap;
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
    bool bound_res[64] = {};
    u64 res_address[64] = {};      // title address of a bound buffer (X2W_CX_ENTRY_LOG)
    std::size_t res_bytes[64] = {};
    const bool show = x2w_local::first_calls(tr.logged, 2);
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
                x2w_startup::BufferViewLayout l;
                if (dim == 1 /* BUFFER in both SRV and UAV enums */ && resource && resource->own().title_memory &&
                    x2w_startup::buffer_view_layout_from_desc(record.slot == 19, record.desc, record.desc_bytes, l, typed_views_off())) {
                    address = reinterpret_cast<u64>(resource->own().title_memory) + l.offset;
                    bytes = static_cast<std::size_t>(l.bytes);
                    strides[i] = l.index_stride;
                    note_typed_view(l, record, false);
                    if (!typed_views_off() && l.kind == x2w_startup::BufferViewKind::typed) {
                        // word 3 of the V# the console holds for this typed view (format, swizzle), for bufword3 entry
                        // values; raw and structured views keep 0 (the translator's raw-words fallback)
                        std::uint32_t v[8];
                        x2w_startup::gfx10_buffer_words(l, v);
                        word3[i] = v[3];
                        InterlockedIncrement(&cxv_word3);
                    }
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
                res_address[i] = address;
                res_bytes[i] = bytes;
                // LOCAL (video): X2W_CX_UAV_WRITTEN=1 registers a translated compute buffer UAV as GPU-written, as draw UAVs and
                // GPU copies are, so translated draws read the range in place instead of from a record-time copy. Ranges above
                // 1 MiB are not registered (counted), so a wide UAV does not turn every buffer in its window into an in-place read.
                static const bool uav_written = x2w_local::env_on("X2W_CX_UAV_WRITTEN");
                if (r.uav && uav_written) {
                    static LONG registered, too_wide;
                    const bool wide = bytes > (1u << 20);
                    if (!wide) note_gpu_written(address, bytes);
                    const LONG n = InterlockedIncrement(wide ? &too_wide : &registered);
                    if ((n & (n - 1)) == 0)
                        log("x2w-cx X2W_CX_UAV_WRITTEN: %s %ld (latest %016llx uav %u 0x%llx, %zu bytes)", wide ? "not registered (> 1 MiB)" : "registered",
                            n, key, i, address, bytes);
                }
            }
            if (const int rw = range_watch_hit(address, bytes); rw >= 0) {
                if (range_watch_line(rw))
                    log("x2w-watch 0x%llx: compute %016llx %s %u 0x%llx (%zu bytes) frame %ld: %s", range_watch_address(rw), key, r.uav ? "uav" : "srv",
                        i, address, bytes, frame_number, bound ? "bound" : "NULL");
                range_watch_sample("compute-bind", key);
            }
        }
        if (!bound) null_view(r, dest, r.uav);
        bound_res[i] = bound;
        if (show)
            log("x2w-cx   %016llx %s %u kind %d source %d(%d,%d): entry 0x%llx words %08x %08x %08x %08x -> address 0x%llx %zu bytes stride %u: %s",
                key, r.uav ? "uav" : "srv", i, r.kind, r.source, r.a, r.b, entry, words[0], words[1], words[2], words[3], address, bytes,
                strides[i], bound ? "bound" : "NULL");
    }
    std::uint32_t values[64] = {};
    // LOCAL: whether each entry value is the title's. A root constant word the title has not set since the root
    // signature changed (or set as another kind of root argument), or a descword whose table is not bound, would
    // be a stale value or 0; such a dispatch is skipped ("entry value unresolved"; X2W_CX_UNRESOLVED_RUN=1 runs it).
    // A stride / word 3 / base of a resource that is not bound only addresses that null resource: counted, run.
    char state[64] = {};  // 0 resolved, 'u' unresolved (skips), 'n' of a null resource
    int unresolved = -1;
    for (std::size_t i = 0; i < tr.uds.size(); ++i) {
        const ComputeTranslation::Ud& ud = tr.uds[i];
        const bool in = ud.a >= 0 && ud.a < 64;
        values[i] = ud.kind == 3 ? ((ud.a < 16 && ud.b < 16) ? pass.compute_constants[ud.a][ud.b] : 0)
            : ud.kind == 4 ? (in ? strides[ud.a] : 0) : ud.kind == 5 ? (in ? word3[ud.a] : 0) : (in ? bases[ud.a] : 0);
        if (ud.kind == 3) {
            const bool set = ud.a >= 0 && ud.a < 16 && ud.b >= 0 && ud.b < 16 && pass.compute_kind[ud.a] == 1 &&
                             ((pass.compute_set[ud.a] >> ud.b) & 1);
            if (!set) state[i] = 'u';
        } else if (ud.kind == 7) {
            bool resolved = false;
            values[i] = descword_value(pass, ud.a, ud.b, key, &resolved);
            if (!resolved) state[i] = 'u';
        } else if (!in || static_cast<std::size_t>(ud.a) >= n || !bound_res[ud.a]) {
            state[i] = 'n';
        }
        if (state[i] == 'u' && unresolved < 0) unresolved = static_cast<int>(i);
        if (state[i]) InterlockedIncrement(state[i] == 'u' ? (ud.kind == 3 ? &cxe_rootconst : &cxe_descword) : &cxe_null_res);
    }
    static const bool run_unresolved = x2w_local::env_on("X2W_CX_UNRESOLVED_RUN");
    if (cx_entry_log(key) && x2w_local::first_calls(tr.entry_logged, 4)) {
        char text[1600];
        int used = std::snprintf(text, sizeof(text), "x2w-cx entry %016llx dispatch (%u, %u, %u):", key, x, y, z);
        for (std::size_t i = 0; i < tr.uds.size() && used < static_cast<int>(sizeof(text)) - 80; ++i) {
            const ComputeTranslation::Ud& ud = tr.uds[i];
            static const char* const names[] = {"?", "?", "?", "rootconst", "stride", "bufword3", "bufbase", "descword"};
            const char* name = ud.kind >= 0 && ud.kind <= 7 ? names[ud.kind] : "?";
            used += std::snprintf(text + used, sizeof(text) - used, " ud%zu=%08x %s(%d,%d)%s", i, values[i], name, ud.a, ud.b,
                                  state[i] == 'u' ? " UNRESOLVED" : state[i] == 'n' ? " null-res" : "");
        }
        log("%s", text);
        // The first words of each small bound buffer, as the CPU sees the title's memory now. The GPU reads the same
        // pages in place (desktop_range) when the list executes, so a later title write can still change them.
        for (UINT i = 0; i < n; ++i) {
            if (!res_address[i] || !res_bytes[i] || res_bytes[i] > 256) continue;
            std::uint32_t w[8] = {};
            const std::size_t count = res_bytes[i] / 4 < 8 ? res_bytes[i] / 4 : 8;
            if (!count || !x2w_local::readable(reinterpret_cast<const void*>(res_address[i]), count * 4)) continue;
            std::memcpy(w, reinterpret_cast<const void*>(res_address[i]), count * 4);
            log("x2w-cx entry %016llx   res %u at 0x%llx (%zu bytes): %08x %08x %08x %08x %08x %08x %08x %08x", key, i, res_address[i],
                res_bytes[i], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    if (unresolved >= 0) {
        const ComputeTranslation::Ud& ud = tr.uds[unresolved];
        static LONG logged;
        if (first_calls(logged, 12))
            log("x2w-cx %016llx: entry value %d (%s %d %d) unresolved: kind %d, words set %04x%s", key, unresolved,
                ud.kind == 3 ? "rootconst" : "descword", ud.a, ud.b, ud.a >= 0 && ud.a < 16 ? pass.compute_kind[ud.a] : -1,
                ud.a >= 0 && ud.a < 16 ? pass.compute_set[ud.a] : 0, run_unresolved ? "; run anyway (X2W_CX_UNRESOLVED_RUN)" : "; dispatch skipped");
        if (!run_unresolved) {
            dispatch_outcome = co_c67_unresolved;
            ReleaseSRWLockExclusive(&draw_lock);
            return false;
        }
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
    gpu_time::dispatch(self, list, key, false);  // LOCAL: X2W_CX_GPU_TIME (d3d12_gpu_time.inl)
    indirect_dispatch(self, list, x, y, z);  // LOCAL: Dispatch, or the pending ExecuteIndirect (d3d12_indirect.inl)
    gpu_time::dispatch(self, list, key, true);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    const LONG done = InterlockedIncrement(&cx_dispatched);
    dispatch_outcome = co_c67_run;
    if (x2w_local::first_of(done, 8) || done % 5000 == 0)
        log("x2w-cx dispatch %ld: %016llx (%u, %u, %u); ready %ld missing %ld null %ld bad-uav %ld; ranges ok %ld split %ld unmapped %ld", done, key, x, y, z,
            cx_ready, cx_missing, cx_null_res, cx_bad_uav, range_ok, range_split, range_unmapped);
    if ((done & 63) == 1) check_device("translated dispatch", 0);
    return true;
}
