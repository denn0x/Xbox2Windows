// Draws through device slot 57 pipelines whose state block A carries hand-written DB_DEPTH_CONTROL,
// PA_SU_SC_MODE_CNTL and CB_TARGET_MASK words (pipeline_state_decode.h), into an 8x4 RGBA target with
// a D32 depth target, and writes each case's pixels to <outdir>/<case>. Whether the decoded state is
// applied is decided by the X2W_DRAW_STATE_* switches the driver script sets; this program only draws.
// No title data.
//
// usage: startup_draw_state <vs_near> <vs_far> <ps_green> <ps_red> <root out> <warp|hardware> [<outdir>]
// Without <outdir>: creates the stream pipelines only (their shader requests feed the offline translator).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "startup_stream_setup.h"

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {

using Word = std::uint64_t;
template <class T> Word ptr_arg(T* p) { return reinterpret_cast<Word>(p); }

bool read_bytes(const char* path, std::vector<char>& out)
{
    std::ifstream file(path, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(file), {});
    return !out.empty();
}

int pick_adapter(bool hardware, ComPtr<IDXGIAdapter>& adapter)
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 4;
    if (!hardware) return SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ? 0 : 4;
    ComPtr<IDXGIFactory6> factory6;
    if (FAILED(factory.As(&factory6))) return 77;
    for (UINT n = 0;; ++n) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory6->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) return 77;
        if (FAILED(hr)) return 4;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) return 4;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
            adapter = candidate;
            return 0;
        }
    }
}

struct alignas(8) StageSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag;
    D3D12_SHADER_BYTECODE shader;
};
struct alignas(8) RootSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE tag;
    ID3D12RootSignature* signature;
};
struct PlainStream {
    StageSubobject ps;
    RootSubobject root;
    StageSubobject vs;
};

void* create_stream_pso(void* device, void* root, const std::vector<char>& vs, const std::vector<char>& ps)
{
    using CreateStream = HRESULT(WINAPI*)(void*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
    PlainStream stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps.data(), ps.size()}},
                       {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, static_cast<ID3D12RootSignature*>(root)},
                       {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {vs.data(), vs.size()}}};
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    void* pso = nullptr;
    return SUCCEEDED(reinterpret_cast<CreateStream>((*static_cast<void***>(device))[86])(device, &desc, __uuidof(ID3D12PipelineState), &pso))
               ? pso : nullptr;
}

// Slot 57 description as the stand-in reads it (tests/startup_stream_consumer.cpp): 64-bit words 0 (root
// signature), 1-2 (state block A and its size), 5-6 and 9-10 (vertex and pixel shader). The shaders are
// read at the first draw, so their storage outlives the pipeline here.
struct Slot57 {
    std::vector<unsigned char> desc = std::vector<unsigned char>(0x300);
    std::vector<unsigned char> state_a = std::vector<unsigned char>(0x200);
};

void* create_slot57(void* device, void* root, const std::vector<char>& vs, const std::vector<char>& ps,
                    const std::vector<std::pair<unsigned, std::uint32_t>>& words, Slot57& keep)
{
    for (const auto& [offset, word] : words) std::memcpy(keep.state_a.data() + offset, &word, sizeof(word));
    const Word d[12] = {ptr_arg(root), ptr_arg(keep.state_a.data()), keep.state_a.size(), 0, 0, ptr_arg(vs.data()), vs.size(), 0, 0,
                        ptr_arg(ps.data()), ps.size(), 0};
    std::memcpy(keep.desc.data(), d, sizeof(d));
    using Create = HRESULT(WINAPI*)(void*, const void*, REFIID, void**);
    void* pso = nullptr;
    return SUCCEEDED(reinterpret_cast<Create>((*static_cast<void***>(device))[57])(device, keep.desc.data(), __uuidof(ID3D12PipelineState), &pso))
               ? pso : nullptr;
}

// The depth target is cleared on the desktop device (list slot 51 is the stand-in's own subject).
int clear_depth_desktop(ID3D12Device* desktop, ID3D12Resource* depth_tex, float value)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(desktop->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)))) return 40;
    const D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE, {}};
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    desktop->CreateDepthStencilView(depth_tex, &dsv_desc, handle);
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(desktop->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(desktop->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(desktop->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(desktop->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 41;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {depth_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE};
    list->ResourceBarrier(1, &b);
    list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    if (FAILED(list->Close())) return 42;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) return 43;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return 44;
    const bool ok = SUCCEEDED(fence->SetEventOnCompletion(1, ev)) && WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
    CloseHandle(ev);
    return ok ? 0 : 45;
}

int execute_guest_list(void* queue, void* list, void* fence, UINT64 fence_value)
{
    if (FAILED(static_cast<HRESULT>(invoke(list, 9)))) return 50;
    void* lists[] = {list};
    invoke(queue, 10, 1, ptr_arg(lists));
    if (FAILED(static_cast<HRESULT>(invoke(queue, 14, ptr_arg(fence), fence_value)))) return 51;
    const auto deadline = GetTickCount64() + 10000;
    while (invoke(fence, 8) < fence_value && GetTickCount64() < deadline) Sleep(1);
    return invoke(fence, 8) >= fence_value ? 0 : 53;
}

// State block A offsets (vertex layout) and register fields used here.
constexpr unsigned a_shader_mask = 0x78, a_mode = 0xa0, a_depth = 0xb8, a_target_mask = 0xcc;
constexpr std::uint32_t z_less_write = 1u << 1 | 1u << 2 | 1u << 4;  // Z_ENABLE, Z_WRITE_ENABLE, ZFUNC FRAG_LESS
constexpr std::uint32_t cull_front = 1u << 0, cull_back = 1u << 1, face_cw = 1u << 2;

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 7 && argc != 8) return 2;
    std::vector<char> vs_near, vs_far, ps_green, ps_red;
    if (!read_bytes(argv[1], vs_near) || !read_bytes(argv[2], vs_far) || !read_bytes(argv[3], ps_green) || !read_bytes(argv[4], ps_red)) return 3;
    const bool hardware = std::strcmp(argv[6], "hardware") == 0;
    ComPtr<IDXGIAdapter> adapter;
    if (const int rc = pick_adapter(hardware, adapter)) return rc;
    std::array<unsigned char, 64> parameters{};
    void* guest = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &guest))) return 5;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return 6;
    std::ofstream(argv[5], std::ios::binary).write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    void* root = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 16, 0, ptr_arg(blob->GetBufferPointer()), blob->GetBufferSize(),
                                           ptr_arg(&__uuidof(ID3D12RootSignature)), ptr_arg(&root))))) return 7;
    if (argc == 7) {
        // Request pass: the same shader pairs through slot 86 publish their translation requests.
        return create_stream_pso(guest, root, vs_near, ps_green) && create_stream_pso(guest, root, vs_far, ps_red) ? 0 : 9;
    }
    const std::string outdir = argv[7];

    // Every case keeps all channels writable unless it sets the target mask itself.
    const std::vector<std::pair<unsigned, std::uint32_t>> rgba = {{a_shader_mask, 0xF}, {a_target_mask, 0xF}};
    auto with = [&](std::vector<std::pair<unsigned, std::uint32_t>> extra) {
        auto words = rgba;
        words.insert(words.end(), extra.begin(), extra.end());
        return words;
    };
    struct Case {
        const char* name;
        std::vector<std::pair<unsigned, std::uint32_t>> words;
        bool far_second;    // also draw the far red triangle after the near green one, same state
        UINT guest_clear = 0;  // list slot 51 with these flags and guest_depth, after the desktop pre-clear
        float desktop_depth = 1.0f;
        float guest_depth = 1.0f;
    };
    // The depth target is one resource for all cases and the shim never forgets a cleared target, so the cases that
    // must see it uncleared run first.
    const Case cases[] = {
        // Run 100: depth test on a target no guest clear initialised. The shim keeps the test off: the near triangle
        // draws over a desktop depth of 0. With X2W_DRAW_STATE_DEPTH_UNCLEARED=1 (negative control) LESS fails.
        {"uncleared", with({{a_depth, z_less_write}}), false, 0, 0.0f},
        // A stencil-only clear on a D32 target does not initialise depth: same as uncleared.
        {"guest_clear_stencil_only", with({{a_depth, z_less_write}}), false, D3D12_CLEAR_FLAG_STENCIL, 0.0f},
        {"depth", with({{a_depth, z_less_write}}), true, D3D12_CLEAR_FLAG_DEPTH},
        // Negative control: bits 30-31 make the decoder refuse the word, so the draw keeps the state of before.
        {"depth_refused", with({{a_depth, z_less_write | 1u << 30}}), true, D3D12_CLEAR_FLAG_DEPTH},
        {"no_state", with({}), true},
        {"cull_back", with({{a_mode, cull_back}}), false},
        {"cull_front", with({{a_mode, cull_front}}), false},
        {"cull_back_face1", with({{a_mode, cull_back | face_cw}}), false},
        {"mask_rg", {{a_shader_mask, 0xF}, {a_target_mask, 0x3}}, false},
        // List slot 51: a forwarded depth clear to 1 lets the near triangle pass LESS over a depth of 0.
        {"guest_clear", with({{a_depth, z_less_write}}), false, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 1.0f},
        // Negative control: a forwarded clear to 0 over a desktop depth of 1 makes LESS fail, so the value is forwarded.
        {"guest_clear0", with({{a_depth, z_less_write}}), false, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0.0f},
    };
    std::vector<Slot57> keep(2 * std::size(cases));
    std::vector<std::pair<void*, void*>> psos;
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        void* near_pso = create_slot57(guest, root, vs_near, ps_green, cases[i].words, keep[2 * i]);
        void* far_pso = create_slot57(guest, root, vs_far, ps_red, cases[i].words, keep[2 * i + 1]);
        if (!near_pso || !far_pso) return 10;
        psos.emplace_back(near_pso, far_pso);
    }

    ComPtr<ID3D12Device> desktop;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 20;
    constexpr UINT width = 8, height = 4;
    StreamTarget state{};
    state.count = 1;
    state.attachments[0].row_bytes = width * 4;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const float clear_color[] = {0.0f, 0.0f, 1.0f, 1.0f};
    const D3D12_RESOURCE_DESC rtv_tex_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const D3D12_CLEAR_VALUE rtv_clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.0f, 0.0f, 1.0f, 1.0f}};
    if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rtv_tex_desc, D3D12_RESOURCE_STATE_COMMON, &rtv_clear,
                                                IID_PPV_ARGS(&state.attachments[0].texture)))) return 21;
    ComPtr<ID3D12Resource> depth_tex;
    const D3D12_RESOURCE_DESC dsv_tex_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0},
                                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    // No optimized clear value: the cases clear to 0 and to 1.
    if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &dsv_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&depth_tex)))) return 21;
    const struct { D3D12_DESCRIPTOR_HEAP_DESC desc; Word backing, bytes; } rtv_heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, 0, 0},
        dsv_heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1}, 0, 0};
    void* dsv_heap = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 14, ptr_arg(&rtv_heap_desc), ptr_arg(&__uuidof(ID3D12DescriptorHeap)), ptr_arg(&state.rtv_heap)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 14, ptr_arg(&dsv_heap_desc), ptr_arg(&__uuidof(ID3D12DescriptorHeap)), ptr_arg(&dsv_heap))))) return 22;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{static_cast<SIZE_T>(invoke(state.rtv_heap, 9))};
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv{static_cast<SIZE_T>(invoke(dsv_heap, 9))};
    const D3D12_RENDER_TARGET_VIEW_DESC rtv_view{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D, {}};
    const D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE, {}};
    invoke(guest, 20, ptr_arg(state.attachments[0].texture.Get()), ptr_arg(&rtv_view), rtv.ptr);
    invoke(guest, 21, ptr_arg(depth_tex.Get()), ptr_arg(&dsv_view), dsv.ptr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    if (FAILED(static_cast<HRESULT>(invoke(guest, 8, ptr_arg(&qd), ptr_arg(&__uuidof(ID3D12CommandQueue)), ptr_arg(&state.queue)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, ptr_arg(&__uuidof(ID3D12CommandAllocator)), ptr_arg(&state.allocator)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 73, ptr_arg(list_desc), ptr_arg(state.allocator), 0, ptr_arg(&__uuidof(ID3D12GraphicsCommandList)),
                                           ptr_arg(&state.list)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 36, 0, 0, ptr_arg(&__uuidof(ID3D12Fence)), ptr_arg(&state.fence))))) return 23;
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT rect{0, 0, 4, static_cast<LONG>(height)};

    UINT64 fence_value = 0;
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        if (const int rc = clear_depth_desktop(desktop.Get(), depth_tex.Get(), cases[i].desktop_depth)) return rc;
        if (FAILED(static_cast<HRESULT>(invoke(state.allocator, 8)))) return 24;
        if (FAILED(static_cast<HRESULT>(invoke(state.list, 139, ptr_arg(state.allocator))))) return 25;
        invoke(state.list, 52, rtv.ptr, ptr_arg(clear_color));
        invoke(state.list, 21, 1, ptr_arg(&viewport));
        invoke(state.list, 22, 1, ptr_arg(&rect));
        invoke(state.list, 20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        invoke(state.list, 50, 1, ptr_arg(&rtv), FALSE, ptr_arg(&dsv));
        if (cases[i].guest_clear) {
            // Typed call: the depth value is a float argument (xmm3), which invoke cannot pass.
            using ClearDepth = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
            reinterpret_cast<ClearDepth>((*static_cast<void***>(state.list))[51])(state.list, dsv, static_cast<D3D12_CLEAR_FLAGS>(cases[i].guest_clear),
                                                                                   cases[i].guest_depth, 0, 0, nullptr);
        }
        invoke(state.list, 25, ptr_arg(psos[i].first));
        invoke(state.list, 12, 3, 1);
        if (cases[i].far_second) {
            invoke(state.list, 25, ptr_arg(psos[i].second));
            invoke(state.list, 12, 3, 1);
        }
        if (const int rc = execute_guest_list(state.queue, state.list, state.fence, ++fence_value)) return rc;
        if (const int rc = read_stream_target(desktop.Get(), state, (outdir + "/" + cases[i].name).c_str())) return rc;
    }
    return 0;
}
