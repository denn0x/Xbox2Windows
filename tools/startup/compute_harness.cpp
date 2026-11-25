// Offline harness for translated compute shaders (startup tools): runs one compiled compute shader with Dispatch on
// WARP ONLY (there is no hardware option: a hang of a translated shader on a GPU can take the desktop down), with
// the root layout the runtime uses for translated dispatches (src/startup_stubs/d3d12_compute.inl load_compute):
//   [0] 32-bit constants b0 (the manifest's entry values; at least one)
//   non-heap: [1] table: n SRVs t0..t(n-1), then n UAVs u0..u(n-1)
//   heap:     [1] table: t0, t1 space1 (heapmem, vinfo), [2] the n+n table, root flag CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
//             title view id i, kind k (gfx2hlsl HEAP kinds: 4 buffer SRV, 7 buffer UAV) at heap index 262152 + 8 i + k,
//             empty views at 262144 + k.
// The runtime binds heapmem and vinfo as ROOT SRVs; here they are bounded raw views in a table (a root SRV has no
// bounds check, and the translation's vfmt() calls vinfo.GetDimensions). Every buffer is one default-heap resource in
// UNORDERED_ACCESS state; SRVs and UAVs are raw (R32_TYPELESS) views of it. All resources are written back.
//
// usage: compute_harness <cfg.txt> [--debug]       (also: compute_harness --caps)
// cfg lines:
//   cs <cso>                       groups <x> <y> <z>          consts <n> <v0> ... <v(n-1)>
//   nres <n>                       (table views; every slot not given by a view line is a null raw view)
//   res <k> <file>                 buffer resource k with the file's bytes (size rounded up to 16)
//   view <i> srv|uav <k> <byte offset, 16-aligned> <bytes>     table slot i
//   heap <heapmem file> <vinfo file>
//   hview <id> <kind 4|7> <k> <byte offset, 16-aligned> <bytes>
//   out <prefix>                   writes <prefix>.res<k> for every resource
// Exit: 0 ok, 1 D3D12 failure, 2 bad cfg, 3 the GPU did not finish within 60 s (fatal; nothing written), 77 no SM 6.6
// when the cfg needs it.
//
// Build (x64 Developer prompt): cl /nologo /EHsc /O2 /std:c++17 compute_harness.cpp d3d12.lib dxgi.lib
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static ID3D12InfoQueue* g_info = nullptr;

static void dump_messages() {
    if (!g_info) return;
    UINT64 n = g_info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < 64; ++i) {
        SIZE_T size = 0;
        g_info->GetMessage(i, nullptr, &size);
        std::vector<char> buf(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        g_info->GetMessage(i, m, &size);
        std::printf("debug-layer: severity %d id %d: %.*s\n", (int)m->Severity, (int)m->ID, (int)m->DescriptionByteLength,
                    m->pDescription);
    }
    std::printf("debug-layer messages: %llu\n", (unsigned long long)n);
}

#define CHECK(x)                                                                         \
    do {                                                                                 \
        HRESULT hr_ = (x);                                                               \
        if (FAILED(hr_)) {                                                               \
            std::fprintf(stderr, "%s failed: 0x%08lx (line %d)\n", #x, hr_, __LINE__); \
            dump_messages();                                                             \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

template <typename T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

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

struct View { UINT slot = 0; bool uav = false; UINT res = 0; UINT64 offset = 0, bytes = 0; };
struct HView { UINT id = 0, kind = 4, res = 0; UINT64 offset = 0, bytes = 0; };

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: compute_harness <cfg.txt> [--debug] | --caps\n");
        return 2;
    }
    const bool caps_only = std::string(argv[1]) == "--caps";
    bool debug = false;
    for (int i = 2; i < argc; ++i) debug |= std::string(argv[i]) == "--debug";
    std::string cs_path, out;
    UINT groups[3] = {1, 1, 1}, nres = 0;
    std::vector<UINT> consts;
    std::map<UINT, std::string> res_files;
    std::vector<View> views;
    std::vector<HView> hviews;
    std::string heapmem_path, vinfo_path;
    if (!caps_only) {
        std::ifstream cfg(argv[1]);
        if (!cfg) { std::fprintf(stderr, "cfg: cannot read %s\n", argv[1]); return 2; }
        std::string line;
        while (std::getline(cfg, line)) {
            std::istringstream s(line);
            std::string key;
            s >> key;
            if (key == "cs") std::getline(s >> std::ws, cs_path);
            else if (key == "groups") s >> groups[0] >> groups[1] >> groups[2];
            else if (key == "consts") { UINT n = 0; s >> n; consts.resize(n); for (auto& c : consts) s >> c; }
            else if (key == "nres") s >> nres;
            else if (key == "res") { UINT k; std::string p; s >> k; std::getline(s >> std::ws, p); res_files[k] = p; }
            else if (key == "view") { View v; std::string t; s >> v.slot >> t >> v.res >> v.offset >> v.bytes; v.uav = t == "uav"; views.push_back(v); }
            else if (key == "heap") s >> heapmem_path >> vinfo_path;
            else if (key == "hview") { HView v; s >> v.id >> v.kind >> v.res >> v.offset >> v.bytes; hviews.push_back(v); }
            else if (key == "out") std::getline(s >> std::ws, out);
            else if (!key.empty() && key[0] != '#') { std::fprintf(stderr, "cfg: unknown line '%s'\n", line.c_str()); return 2; }
        }
        if (cs_path.empty() || out.empty()) { std::fprintf(stderr, "cfg: needs cs and out\n"); return 2; }
        for (const auto& v : views)
            if (v.slot >= nres || !res_files.count(v.res) || (v.offset & 15)) { std::fprintf(stderr, "cfg: bad view slot %u\n", v.slot); return 2; }
        for (const auto& v : hviews)
            if (v.id >= 90000 || (v.kind != 4 && v.kind != 7) || !res_files.count(v.res) || (v.offset & 15)) {
                std::fprintf(stderr, "cfg: bad hview %u\n", v.id);
                return 2;
            }
        if (consts.empty()) consts.push_back(0);
    }
    const bool heap = !heapmem_path.empty();

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
    CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));  // WARP ONLY
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { std::fprintf(stderr, "not a software adapter: refusing\n"); return 1; }
    LARGE_INTEGER umd{};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    std::printf("adapter: %ls (vendor %04x device %04x) driver %u.%u.%u.%u\n", ad.Description, ad.VendorId, ad.DeviceId,
                HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
    Ref<ID3D12Device> device;
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    if (debug && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&g_info)))) g_info->SetMuteDebugOutput(FALSE);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    }
    std::printf("caps: wave ops %d lanes %u-%u, shader model 0x%x, resource binding tier %d\n", (int)o1.WaveOps,
                o1.WaveLaneCountMin, o1.WaveLaneCountMax, (unsigned)sm.HighestShaderModel, (int)opt.ResourceBindingTier);
    if (caps_only) return 0;
    if (heap && sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) { std::printf("capability skip: no SM 6.6\n"); return 77; }

    // root signature (d3d12_compute.inl load_compute layout, bounded space1 views: see the header)
    const UINT n = nres;
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, n, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, n, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, n};
    D3D12_DESCRIPTOR_RANGE1 space1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    std::vector<D3D12_ROOT_PARAMETER1> params;
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.Num32BitValues = (UINT)consts.size();
    params.push_back(p);
    UINT space1_param = 0, table_param = 0;
    if (heap) {
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        q.DescriptorTable = {1, &space1};
        space1_param = (UINT)params.size();
        params.push_back(q);
    }
    if (n) {
        D3D12_ROOT_PARAMETER1 q{};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        q.DescriptorTable = {2, ranges};
        table_param = (UINT)params.size();
        params.push_back(q);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1 = {(UINT)params.size(), params.data(), 0, nullptr,
                    heap ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED : D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Ref<ID3DBlob> rs_blob, rs_err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &rs_blob, &rs_err))) {
        std::fprintf(stderr, "root signature: %s\n", rs_err ? (const char*)rs_err->GetBufferPointer() : "?");
        return 1;
    }
    Ref<ID3D12RootSignature> root;
    CHECK(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    auto cs = read_file(cs_path);
    if (cs.empty()) { std::fprintf(stderr, "missing %s\n", cs_path.c_str()); return 2; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS = {cs.data(), cs.size()};
    Ref<ID3D12PipelineState> pso;
    HRESULT pso_hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    std::printf("pipeline: 0x%08lx\n", (unsigned long)pso_hr);
    if (FAILED(pso_hr)) { dump_messages(); return 1; }

    // resources
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD}, def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    std::map<UINT, Ref<ID3D12Resource>> res, staging, back;
    std::map<UINT, UINT64> res_size;
    auto upload = [&](const std::vector<char>& data, UINT64 at_least, Ref<ID3D12Resource>& r) -> HRESULT {
        auto d = buffer_desc(data.size() > at_least ? data.size() : at_least);
        HRESULT hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) return hr;
        void* m = nullptr;
        if (FAILED(hr = r->Map(0, nullptr, &m))) return hr;
        std::memset(m, 0, (size_t)d.Width);
        if (!data.empty()) std::memcpy(m, data.data(), data.size());
        r->Unmap(0, nullptr);
        return S_OK;
    };
    for (const auto& kv : res_files) {
        auto data = read_file(kv.second);
        if (data.empty()) { std::fprintf(stderr, "missing or empty %s\n", kv.second.c_str()); return 2; }
        auto d = buffer_desc(data.size(), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        res_size[kv.first] = d.Width;
        CHECK(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res[kv.first])));
        CHECK(upload(data, d.Width, staging[kv.first]));
        auto r = buffer_desc(d.Width);
        CHECK(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back[kv.first])));
    }
    Ref<ID3D12Resource> heapmem_res, vinfo_res;
    std::vector<char> heapmem_data, vinfo_data;
    if (heap) {
        heapmem_data = read_file(heapmem_path);
        vinfo_data = read_file(vinfo_path);
        CHECK(upload(heapmem_data, 16, heapmem_res));
        CHECK(upload(vinfo_data, 16, vinfo_res));
    }

    // one shader-visible heap: [0, 2n) the table, [2n, 2n + 2) space1, the view-id region from 262144
    const UINT region = 262144, heap_size = heap ? region + 8 + 8 * 90000 : (2 * n ? 2 * n : 1);
    Ref<ID3D12DescriptorHeap> dheap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heap_size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dheap)));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = dheap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T{i} * step; return h; };
    auto srv_view = [&](ID3D12Resource* r, UINT64 offset, UINT64 bytes, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.FirstElement = offset / 4;
        v.Buffer.NumElements = (UINT)((bytes + 3) / 4);
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(v.Buffer.NumElements ? r : nullptr, &v, h);
    };
    auto uav_view = [&](ID3D12Resource* r, UINT64 offset, UINT64 bytes, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        v.Buffer.FirstElement = offset / 4;
        v.Buffer.NumElements = (UINT)((bytes + 3) / 4);
        v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(v.Buffer.NumElements ? r : nullptr, nullptr, &v, h);
    };
    for (UINT i = 0; i < n; ++i) {
        srv_view(nullptr, 0, 0, at(i));
        uav_view(nullptr, 0, 0, at(n + i));
    }
    for (const auto& v : views) {
        if (v.offset + v.bytes > res_size[v.res]) { std::fprintf(stderr, "view %u past its resource\n", v.slot); return 2; }
        if (v.uav) uav_view(res[v.res], v.offset, v.bytes, at(n + v.slot));
        else srv_view(res[v.res], v.offset, v.bytes, at(v.slot));
    }
    if (heap) {
        srv_view(heapmem_res, 0, heapmem_data.size(), at(2 * n));
        srv_view(vinfo_res, 0, vinfo_data.size(), at(2 * n + 1));
        // empty views (kinds 4 and 7 only: the harness builds buffer heaps) and the listed ids' views
        for (UINT k = 0; k < 8; ++k) srv_view(nullptr, 0, 0, at(region + k));
        uav_view(nullptr, 0, 0, at(region + 7));
        for (const auto& v : hviews) {
            srv_view(nullptr, 0, 0, at(region + 8 + 8 * v.id + 4));
            uav_view(nullptr, 0, 0, at(region + 8 + 8 * v.id + 7));
        }
        for (const auto& v : hviews) {
            if (v.offset + v.bytes > res_size[v.res]) { std::fprintf(stderr, "hview %u past its resource\n", v.id); return 2; }
            if (v.kind == 7) uav_view(res[v.res], v.offset, v.bytes, at(region + 8 + 8 * v.id + 7));
            else srv_view(res[v.res], v.offset, v.bytes, at(region + 8 + 8 * v.id + 4));
        }
    }
    std::printf("bound: %u table slots (%zu views), %zu resources, heap %s (%zu views)\n", n, views.size(), res.size(),
                heap ? "yes" : "no", hviews.size());

    D3D12_COMMAND_QUEUE_DESC qdesc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)));
    Ref<ID3D12CommandAllocator> alloc;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    Ref<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
    for (auto& kv : res) {
        list->CopyBufferRegion(kv.second, 0, staging[kv.first], 0, res_size[kv.first]);
        auto b = transition(kv.second, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &b);
    }
    ID3D12DescriptorHeap* heaps[] = {dheap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root);
    list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0, (UINT)consts.size(), consts.data(), 0);
    if (heap) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = dheap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += UINT64{2 * n} * step;
        list->SetComputeRootDescriptorTable(space1_param, g);
    }
    if (n) list->SetComputeRootDescriptorTable(table_param, dheap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(groups[0], groups[1], groups[2]);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &ub);
    for (auto& kv : res) {
        auto b = transition(kv.second, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b);
        list->CopyBufferRegion(back[kv.first], 0, kv.second, 0, res_size[kv.first]);
    }
    CHECK(list->Close());
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    Ref<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK(queue->Signal(fence, 1));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(fence->SetEventOnCompletion(1, ev));
    const DWORD t0 = GetTickCount();
    const DWORD waited = WaitForSingleObject(ev, 60000);
    std::printf("gpu wait: %s after %lu ms, fence %llu\n", waited == WAIT_OBJECT_0 ? "done" : "TIMEOUT",
                (unsigned long)(GetTickCount() - t0), (unsigned long long)fence->GetCompletedValue());
    std::fflush(stdout);
    if (waited != WAIT_OBJECT_0) {
        std::fprintf(stderr, "the GPU did not finish within 60 s (fatal)\n");
        ExitProcess(3);
    }
    CHECK(device->GetDeviceRemovedReason());
    for (auto& kv : back) {
        void* m = nullptr;
        D3D12_RANGE r{0, (SIZE_T)res_size[kv.first]};
        CHECK(kv.second->Map(0, &r, &m));
        std::ofstream f(out + ".res" + std::to_string(kv.first), std::ios::binary);
        f.write((const char*)m, (std::streamsize)res_size[kv.first]);
        kv.second->Unmap(0, nullptr);
    }
    dump_messages();
    std::printf("ok\n");
    return 0;
}
