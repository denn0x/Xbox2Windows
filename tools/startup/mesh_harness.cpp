// Offline harness for translated mesh shaders (startup tools): runs one ms_6_5 + ps_6_x pair with DispatchMesh on
// WARP (default) or the first hardware adapter (--hw), with root constants b0, root SRVs t0..tN-1, a dump UAV u0 and
// a primitive-hit UAV u1 as described by a small text file, and writes the two UAVs back.
//
// usage: mesh_harness <cfg.txt> [--hw] [--debug]
// cfg lines: ms <cso> | ps <cso> | groups <n> | consts <n> <v0> ... | srv <file> (t0, t1, ... in order) |
//            dump <bytes> | hits <bytes> | out <prefix>   -> writes <prefix>.dump, <prefix>.hits
// Exit 77 (capability skip) when the adapter has no mesh shader support.
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
        }
    }
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

    // root signature: b0 constants, t0..tN-1 root SRVs, u0 dump, u1 hits
    std::vector<D3D12_ROOT_PARAMETER1> params;
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.Num32BitValues = (UINT)consts.size();
    params.push_back(p);
    for (UINT i = 0; i < srvs.size(); ++i) {
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        q.Descriptor.ShaderRegister = i;
        q.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        params.push_back(q);
    }
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
    CHECK(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err));
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
    for (UINT i = 0; i < srvs.size(); ++i) list->SetGraphicsRootShaderResourceView(1 + i, srv_res[i]->GetGPUVirtualAddress());
    for (UINT i = 0; i < 2; ++i) list->SetGraphicsRootUnorderedAccessView(1 + (UINT)srvs.size() + i, uav[i]->GetGPUVirtualAddress());
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
    WaitForSingleObject(ev, 60000);
    CHECK(device->GetDeviceRemovedReason());

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
