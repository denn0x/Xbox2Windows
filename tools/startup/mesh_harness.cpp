// Offline harness for translated mesh shaders (startup tools): runs one ms_6_5 + ps_6_x pair with DispatchMesh on
// WARP (default) or the first hardware adapter (--hw), with root constants b0, root SRVs t0..tN-1, a dump UAV u0 and
// a primitive-hit UAV u1 as described by a small text file, and writes the two UAVs back.
//
// usage: mesh_harness <cfg.txt> [--hw] [--debug]
// cfg lines: ms <cso> | ps <cso> | groups <n> | consts <n> <v0> ... | srv <file> (t0, t1, ... in order) |
//            dump <bytes> | hits <bytes> | out <prefix>   -> writes <prefix>.dump, <prefix>.hits
// Exit 77 (capability skip) when the adapter has no mesh shader support.
//
// Resource-heap programs (manifest `heap 1`, `heapwords 1`; gfx2hlsl HEAP_KINDS, RUNTIME_NEEDS.md 6.3) add:
//   heapmem <file> | vinfo <file> | vdesc <file>   t0, t1, t2 in space1 (title heap memory with the X2WV token
//                                                  + view id per 32-byte slot; 16 bytes per id; 32 bytes per id)
//   blob <file>                                    bytes the view lines below point into
//   hbuf <id> <offset> <bytes>                     raw buffer view of blob[offset, offset + bytes) (offset 16-aligned)
//   htex1d <id> <dxgi> <width> <offset>            Texture1D of blob texels from offset, one level
//   htex2d <id> <dxgi> <width> <height> <offset>   Texture2D, rows packed (width * texel bytes apart)
// The three space1 buffers are bounded raw views in ONE descriptor table (the runtime binds them as root SRVs; a
// root SRV has no bounds check, MESH.md 4.4). The view-id region is a second table: 8 + 8 * 90000 descriptors,
// the five arrays t0 in spaces 2..6 (Texture2D, Texture2DArray, Texture3D, Texture1D, raw buffer) all start at
// offset 0, as the runtime's create_draw_root (each array is indexed 8 + 8 * id + kind, so the kinds never meet).
// Slots 0..7 (the empty views x2w_hix falls back to) and all kinds of every listed id hold null views; the
// listed views overwrite theirs. The region range is DESCRIPTORS_VOLATILE: the slots of unlisted ids stay unset.
//
// Build (x64 Developer prompt): cl /nologo /EHsc /O2 /std:c++17 mesh_harness.cpp d3d12.lib dxgi.lib
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(x)                                                                   \
    do {                                                                           \
        HRESULT hr_ = (x);                                                         \
        if (FAILED(hr_)) {                                                         \
            std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); \
            dump_messages();                                                       \
            return 1;                                                              \
        }                                                                          \
    } while (0)

template <typename T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static ID3D12InfoQueue* g_info = nullptr;

static void dump_messages() {
    if (!g_info) return;
    UINT64 n = g_info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T size = 0;
        g_info->GetMessage(i, nullptr, &size);
        std::vector<char> buf(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        g_info->GetMessage(i, m, &size);
        std::printf("debug-layer: severity %d id %d: %.*s\n", (int)m->Severity, (int)m->ID,
                    (int)m->DescriptionByteLength, m->pDescription);
    }
    std::printf("debug-layer messages: %llu\n", (unsigned long long)n);
}

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, typename D> struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T;
    D data{};
};

struct MeshStream {
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
};

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size < 16 ? 16 : (size + 15) & ~15ull;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return x;
}

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mesh_harness <cfg.txt> [--hw] [--debug]\n");
        return 2;
    }
    bool hw = false, debug = false;
    for (int i = 2; i < argc; ++i) {
        hw |= std::string(argv[i]) == "--hw";
        debug |= std::string(argv[i]) == "--debug";
    }
    std::string ms_path, ps_path, out;
    UINT groups = 1;
    std::vector<UINT> consts;
    std::vector<std::string> srvs;
    UINT64 dump_bytes = 16, hits_bytes = 16;
    std::string heap_files[3], blob_path;  // heapmem, vinfo, vdesc
    struct HeapView {
        UINT id = 0, kind = 4;  // gfx2hlsl HEAP_KINDS slot: 0 tex2d, 3 tex1d, 4 buf
        UINT dxgi = 0, width = 0, height = 1;
        UINT64 offset = 0, bytes = 0;
    };
    std::vector<HeapView> hviews;
    {
        std::ifstream cfg(argv[1]);
        std::string line;
        while (std::getline(cfg, line)) {
            std::istringstream s(line);
            std::string key;
            s >> key;
            if (key == "ms") std::getline(s >> std::ws, ms_path);
            else if (key == "ps") std::getline(s >> std::ws, ps_path);
            else if (key == "groups") s >> groups;
            else if (key == "consts") { UINT n; s >> n; consts.resize(n); for (auto& c : consts) s >> c; }
            else if (key == "srv") { std::string p; std::getline(s >> std::ws, p); srvs.push_back(p); }
            else if (key == "dump") s >> dump_bytes;
            else if (key == "hits") s >> hits_bytes;
            else if (key == "out") std::getline(s >> std::ws, out);
            else if (key == "heapmem") std::getline(s >> std::ws, heap_files[0]);
            else if (key == "vinfo") std::getline(s >> std::ws, heap_files[1]);
            else if (key == "vdesc") std::getline(s >> std::ws, heap_files[2]);
            else if (key == "blob") std::getline(s >> std::ws, blob_path);
            else if (key == "hbuf") { HeapView v; s >> v.id >> v.offset >> v.bytes; hviews.push_back(v); }
            else if (key == "htex1d") { HeapView v; v.kind = 3; s >> v.id >> v.dxgi >> v.width >> v.offset; hviews.push_back(v); }
            else if (key == "htex2d") { HeapView v; v.kind = 0; s >> v.id >> v.dxgi >> v.width >> v.height >> v.offset; hviews.push_back(v); }
            else if (!key.empty() && key[0] != '#') { std::fprintf(stderr, "cfg: unknown line '%s'\n", line.c_str()); return 2; }
        }
    }
    const bool heap = !heap_files[0].empty();
    if (heap && (heap_files[1].empty() || heap_files[2].empty())) { std::fprintf(stderr, "cfg: heapmem needs vinfo and vdesc\n"); return 2; }
    constexpr UINT heap_ids = 90000, region_size = 8 + 8 * heap_ids;  // gfx2hlsl HEAP_IDS, array size 8 + 8 * HEAP_IDS
    for (const auto& v : hviews)
        if (v.id >= heap_ids || (v.kind == 4 && (v.offset & 15))) { std::fprintf(stderr, "cfg: bad view id %u / offset %llu\n", v.id, v.offset); return 2; }
    if (consts.empty()) consts.push_back(0);

    OSVERSIONINFOW os{sizeof(os)};
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&os);
    std::printf("windows %lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);

    if (debug) {
        Ref<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    std::printf("debug layer: %s\n", debug ? "on" : "off");
    Ref<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    Ref<IDXGIAdapter1> adapter;
    if (hw) CHECK(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    else CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    LARGE_INTEGER umd{};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    std::printf("adapter: %ls (vendor %04x device %04x) driver %u.%u.%u.%u\n", ad.Description, ad.VendorId, ad.DeviceId,
                HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
    Ref<ID3D12Device2> device;
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    if (debug && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&g_info)))) g_info->SetMuteDebugOutput(FALSE);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    std::printf("mesh shader tier %d, shader model 0x%x\n", (int)o7.MeshShaderTier, (unsigned)sm.HighestShaderModel);
    if (o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED || sm.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
        std::printf("capability skip: no mesh shaders\n");
        return 77;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    bool stats_ok = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9))) &&
                    o9.MeshShaderPipelineStatsSupported;

    // root signature: b0 constants, t0..tN-1 as ONE descriptor table of raw buffer views (bounded: a load past
    // the end reads 0, as the console's V# range check; root SRVs have no bounds check and an out-of-range load
    // there is undefined, which hung a GPU), u0 dump, u1 hits
    std::vector<D3D12_ROOT_PARAMETER1> params;
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.Num32BitValues = (UINT)consts.size();
    params.push_back(p);
    D3D12_DESCRIPTOR_RANGE1 srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = (UINT)srvs.size();
    srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    if (!srvs.empty()) {
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        q.DescriptorTable.NumDescriptorRanges = 1;
        q.DescriptorTable.pDescriptorRanges = &srv_range;
        params.push_back(q);
    }
    // heap: t0..t2 space1 (bounded raw views) and the view-id region (spaces 2..6, overlaid at offset 0)
    D3D12_DESCRIPTOR_RANGE1 space1_range{};
    space1_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    space1_range.NumDescriptors = 3;
    space1_range.RegisterSpace = 1;
    space1_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_DESCRIPTOR_RANGE1 region_ranges[5]{};
    for (UINT k = 0; k < 5; ++k) {
        region_ranges[k].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        region_ranges[k].NumDescriptors = region_size;
        region_ranges[k].RegisterSpace = 2 + k;
        region_ranges[k].OffsetInDescriptorsFromTableStart = 0;
        region_ranges[k].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    }
    UINT heap_param = 0;
    if (heap) {
        heap_param = (UINT)params.size();
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        q.DescriptorTable.NumDescriptorRanges = 1;
        q.DescriptorTable.pDescriptorRanges = &space1_range;
        params.push_back(q);
        q.DescriptorTable.NumDescriptorRanges = 5;
        q.DescriptorTable.pDescriptorRanges = region_ranges;
        params.push_back(q);
    }
    const UINT uav_param = (UINT)params.size();
    for (UINT i = 0; i < 2; ++i) {
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        q.Descriptor.ShaderRegister = i;
        params.push_back(q);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1.NumParameters = (UINT)params.size();
    rsd.Desc_1_1.pParameters = params.data();
    Ref<ID3DBlob> rs_blob, rs_err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err))) {
        std::fprintf(stderr, "root signature: %s\n", rs_err ? (const char*)rs_err->GetBufferPointer() : "?");
        return 1;
    }
    Ref<ID3D12RootSignature> root;
    CHECK(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)));

    auto ms = read_file(ms_path), ps = read_file(ps_path);
    if (ms.empty() || ps.empty()) { std::fprintf(stderr, "missing shader file\n"); return 1; }
    MeshStream st{};
    st.root.data = root;
    st.ms.data = {ms.data(), ms.size()};
    st.ps.data = {ps.data(), ps.size()};
    st.blend.data.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    st.raster.data.FillMode = D3D12_FILL_MODE_SOLID;
    st.raster.data.CullMode = D3D12_CULL_MODE_NONE;
    st.raster.data.DepthClipEnable = TRUE;
    st.depth.data.DepthEnable = FALSE;
    st.rtv.data.NumRenderTargets = 1;
    st.rtv.data.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    st.sample.data.Count = 1;
    st.mask.data = 0xffffffff;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(st), &st};
    Ref<ID3D12PipelineState> pso;
    CHECK(device->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));

    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD}, def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    std::vector<Ref<ID3D12Resource>> srv_res(srvs.size());
    for (size_t i = 0; i < srvs.size(); ++i) {
        auto data = read_file(srvs[i]);
        auto d = buffer_desc(data.size());
        CHECK(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&srv_res[i])));
        void* m = nullptr;
        CHECK(srv_res[i]->Map(0, nullptr, &m));
        std::memset(m, 0, (size_t)d.Width);
        std::memcpy(m, data.data(), data.size());
        srv_res[i]->Unmap(0, nullptr);
    }
    // one shader-visible heap: [0, n) the srv table, then (heap) 3 space1 views, then the view-id region
    const UINT n_srv = (UINT)srvs.size(), space1_at = n_srv, region_at = n_srv + 3;
    const UINT heap_size = heap ? region_at + region_size : (n_srv ? n_srv : 1u);
    Ref<ID3D12DescriptorHeap> srv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC shd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heap_size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)));
    UINT srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu_at = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T{i} * srv_step;
        return h;
    };
    auto upload = [&](const std::vector<char>& data, UINT64 at_least, Ref<ID3D12Resource>& res) -> HRESULT {
        auto d = buffer_desc(data.size() > at_least ? data.size() : at_least);
        HRESULT hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&res));
        if (FAILED(hr)) return hr;
        void* m = nullptr;
        if (FAILED(hr = res->Map(0, nullptr, &m))) return hr;
        std::memset(m, 0, (size_t)d.Width);
        if (!data.empty()) std::memcpy(m, data.data(), data.size());
        res->Unmap(0, nullptr);
        return S_OK;
    };
    auto raw_view = [&](ID3D12Resource* res, UINT64 offset, UINT64 bytes, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.FirstElement = offset / 4;
        v.Buffer.NumElements = (UINT)(bytes / 4);
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(v.Buffer.NumElements ? res : nullptr, &v, h);
    };
    auto null_view = [&](UINT kind, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        switch (kind) {
        case 0: v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1; break;
        case 1: v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.MipLevels = 1; v.Texture2DArray.ArraySize = 1; break;
        case 2: v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; v.Texture3D.MipLevels = 1; break;
        case 3: v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; v.Texture1D.MipLevels = 1; break;
        default: raw_view(nullptr, 0, 0, h); return;
        }
        device->CreateShaderResourceView(nullptr, &v, h);
    };
    Ref<ID3D12Resource> heap_res[3], blob_res;
    std::vector<Ref<ID3D12Resource>> tex_res(hviews.size()), tex_up(hviews.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> tex_fp(hviews.size());
    if (heap) {
        // space1: exact sizes, so a load past the end reads 0 (heapmem: the title heap; vinfo 16 and vdesc 32 bytes per id)
        const UINT64 min_bytes[3] = {16, 16ull * heap_ids, 32ull * heap_ids};
        for (int i = 0; i < 3; ++i) {
            auto data = read_file(heap_files[i]);
            if (data.empty() && i == 0) { std::fprintf(stderr, "missing %s\n", heap_files[i].c_str()); return 1; }
            CHECK(upload(data, min_bytes[i], heap_res[i]));
            raw_view(heap_res[i], 0, data.size() > min_bytes[i] ? data.size() : min_bytes[i], cpu_at(space1_at + i));
        }
        auto blob = read_file(blob_path);
        CHECK(upload(blob, 16, blob_res));
        for (UINT k = 0; k < 8; ++k) null_view(k, cpu_at(region_at + k));
        for (const auto& v : hviews)
            for (UINT k = 0; k < 5; ++k) null_view(k, cpu_at(region_at + 8 + 8 * v.id + k));
        UINT counts[5] = {};
        for (size_t i = 0; i < hviews.size(); ++i) {
            const auto& v = hviews[i];
            const D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_at(region_at + 8 + 8 * v.id + v.kind);
            ++counts[v.kind];
            if (v.kind == 4) {
                if (v.offset + v.bytes > blob.size()) { std::fprintf(stderr, "hbuf %u past the blob\n", v.id); return 1; }
                raw_view(blob_res, v.offset, v.bytes, h);
                continue;
            }
            D3D12_RESOURCE_DESC td{};
            td.Dimension = v.kind == 3 ? D3D12_RESOURCE_DIMENSION_TEXTURE1D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = v.width;
            td.Height = v.kind == 3 ? 1 : v.height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = (DXGI_FORMAT)v.dxgi;
            td.SampleDesc.Count = 1;
            CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&tex_res[i])));
            UINT rows = 0;
            UINT64 row_bytes = 0, total = 0;
            device->GetCopyableFootprints(&td, 0, 1, 0, &tex_fp[i], &rows, &row_bytes, &total);
            if (v.offset + row_bytes * rows > blob.size()) { std::fprintf(stderr, "htex %u past the blob\n", v.id); return 1; }
            std::vector<char> staged((size_t)total);
            for (UINT r = 0; r < rows; ++r)
                std::memcpy(staged.data() + tex_fp[i].Offset + (size_t)r * tex_fp[i].Footprint.RowPitch, blob.data() + v.offset + r * row_bytes, (size_t)row_bytes);
            CHECK(upload(staged, total, tex_up[i]));
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = td.Format;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (v.kind == 3) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; sv.Texture1D.MipLevels = 1; }
            else { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; }
            device->CreateShaderResourceView(tex_res[i], &sv, h);
        }
        std::printf("heap: heapmem %zu bytes, region %u descriptors at %u, views tex2d %u tex1d %u buf %u\n",
                    read_file(heap_files[0]).size(), region_size, region_at, counts[0], counts[3], counts[4]);
    }
    for (size_t i = 0; i < srvs.size(); ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = (UINT)(read_file(srvs[i]).size() / 4);
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * srv_step;
        device->CreateShaderResourceView(srv_res[i], &v, h);
    }
    Ref<ID3D12Resource> uav[2], back[2], zero;
    UINT64 sizes[2] = {dump_bytes, hits_bytes};
    UINT64 zmax = dump_bytes > hits_bytes ? dump_bytes : hits_bytes;
    {
        auto d = buffer_desc(zmax);
        CHECK(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&zero)));
        void* m = nullptr;
        CHECK(zero->Map(0, nullptr, &m));
        std::memset(m, 0, (size_t)d.Width);
        zero->Unmap(0, nullptr);
    }
    for (int i = 0; i < 2; ++i) {
        auto d = buffer_desc(sizes[i], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&uav[i])));
        auto r = buffer_desc(sizes[i]);
        CHECK(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&back[i])));
    }
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 256;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}};
    Ref<ID3D12Resource> rt;
    CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                          IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    Ref<ID3D12DescriptorHeap> rtv_heap;
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(rt, nullptr, rtv);

    Ref<ID3D12QueryHeap> qheap;
    Ref<ID3D12Resource> qback;
    if (stats_ok) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 1};
        if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&qheap)))) stats_ok = false;
        auto r = buffer_desc(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1));
        CHECK(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&qback)));
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)));
    Ref<ID3D12CommandAllocator> alloc;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    Ref<ID3D12GraphicsCommandList6> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));

    D3D12_RESOURCE_BARRIER b[2];
    for (int i = 0; i < 2; ++i) b[i] = transition(uav[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(2, b);
    for (int i = 0; i < 2; ++i) list->CopyBufferRegion(uav[i], 0, zero, 0, buffer_desc(sizes[i]).Width);
    for (int i = 0; i < 2; ++i) b[i] = transition(uav[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(2, b);
    for (size_t i = 0; i < hviews.size(); ++i) {
        if (!tex_res[i]) continue;
        D3D12_TEXTURE_COPY_LOCATION dst{tex_res[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src{tex_up[i], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.SubresourceIndex = 0;
        src.PlacedFootprint = tex_fp[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER t = transition(tex_res[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &t);
    }
    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, 256, 256, 0, 1};
    D3D12_RECT sc{0, 0, 256, 256};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetGraphicsRootSignature(root);
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, (UINT)consts.size(), consts.data(), 0);
    ID3D12DescriptorHeap* heaps[] = {srv_heap};
    list->SetDescriptorHeaps(1, heaps);
    if (!srvs.empty()) list->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
    if (heap) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64{space1_at} * srv_step;
        list->SetGraphicsRootDescriptorTable(heap_param, g);
        g.ptr += UINT64{3} * srv_step;
        list->SetGraphicsRootDescriptorTable(heap_param + 1, g);
    }
    for (UINT i = 0; i < 2; ++i) list->SetGraphicsRootUnorderedAccessView(uav_param + i, uav[i]->GetGPUVirtualAddress());
    if (stats_ok) list->BeginQuery(qheap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    list->DispatchMesh(groups, 1, 1);
    if (stats_ok) {
        list->EndQuery(qheap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
        list->ResolveQueryData(qheap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 1, qback, 0);
    }
    for (int i = 0; i < 2; ++i) b[i] = transition(uav[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(2, b);
    for (int i = 0; i < 2; ++i) list->CopyBufferRegion(back[i], 0, uav[i], 0, buffer_desc(sizes[i]).Width);
    CHECK(list->Close());
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    Ref<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(queue->Signal(fence, 1));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(fence->SetEventOnCompletion(1, ev));
    DWORD t0 = GetTickCount();
    DWORD waited = WaitForSingleObject(ev, 60000);
    printf("gpu wait: %s after %lu ms, fence %llu\n", waited == WAIT_OBJECT_0 ? "done" : "TIMEOUT",
           (unsigned long)(GetTickCount() - t0), (unsigned long long)fence->GetCompletedValue());
    CHECK(device->GetDeviceRemovedReason());
    if (waited != WAIT_OBJECT_0) {
        fprintf(stderr, "the GPU did not finish within 60 s\n");
        return 3;
    }

    const char* ext[2] = {".dump", ".hits"};
    for (int i = 0; i < 2; ++i) {
        void* m = nullptr;
        D3D12_RANGE r{0, (SIZE_T)sizes[i]};
        CHECK(back[i]->Map(0, &r, &m));
        std::ofstream f(out + ext[i], std::ios::binary);
        f.write((const char*)m, (std::streamsize)sizes[i]);
        back[i]->Unmap(0, nullptr);
    }
    if (stats_ok) {
        void* m = nullptr;
        CHECK(qback->Map(0, nullptr, &m));
        auto* q = (D3D12_QUERY_DATA_PIPELINE_STATISTICS1*)m;
        std::printf("stats: MSInvocations %llu MSPrimitives %llu CInvocations %llu CPrimitives %llu PSInvocations %llu\n",
                    q->MSInvocations, q->MSPrimitives, q->CInvocations, q->CPrimitives, q->PSInvocations);
        qback->Unmap(0, nullptr);
    } else {
        std::printf("stats: mesh pipeline statistics not supported\n");
    }
    dump_messages();
    std::printf("ok\n");
    return 0;
}
