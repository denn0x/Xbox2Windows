// Source-built XCL-12 fixture for a graphics command list recorded through the XCL-08 device.
// D3D12Xbox publishes no command-list creation or recording export. The fixture creates the
// device through d3d12_xs.dll!D3D12XboxCreateDevice and records with the documented desktop
// ID3D12Device::CreateCommandAllocator / CreateCommandList and ID3D12GraphicsCommandList methods
// on that device. It draws one source-built triangle into a 32x32 render target, copies the
// target into a readback buffer and compares all 1,024 RGBA pixels with a CPU reference.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "desktop_gpu.h"
#include "d3d12x_graphics_ps.h"
#include "d3d12x_graphics_vs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using XboxCreateDevice = HRESULT(WINAPI*)(IUnknown*, const void*, REFIID, void**);

constexpr UINT target_side = 32;
constexpr UINT target_pixels = target_side * target_side;
constexpr UINT target_row_bytes = target_side * 4;

// The pixel shader returns (0.2, 0.4, 0.6, 1.0). In an R8G8B8A8_UNORM target those floats
// convert to 51, 102, 153 and 255: 0xff996633 read as a little-endian 32-bit word.
constexpr std::uint32_t triangle_pixel = 0xff996633u;
constexpr std::uint32_t clear_pixel = 0x00000000u;
constexpr float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};

struct Vertex {
    float position[4];
};
static_assert(sizeof(Vertex) == 4 * sizeof(float));

// A clip-space triangle whose hypotenuse is x + y = 2. The 32x32 viewport pixel centers reach
// x + y = 1.9375, so every pixel center is strictly inside the triangle and the rasterizer
// covers all 1,024 pixels.
constexpr Vertex fullscreen_triangle[3] = {
    {{-1.0f, -1.0f, 0.0f, 1.0f}},
    {{3.0f, -1.0f, 0.0f, 1.0f}},
    {{-1.0f, 3.0f, 0.0f, 1.0f}},
};

struct AdapterChoice {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
};

bool contains_ignoring_case(const wchar_t* text, const wchar_t* part)
{
    const std::size_t length = std::wcslen(part);
    for (; *text; ++text) {
        if (_wcsnicmp(text, part, length) == 0) return true;
    }
    return false;
}

// Same selection convention as the other D3D12 fixtures: WARP on request, otherwise the first
// non-software adapter in high-performance order. XBOX2WINDOWS_ADAPTER filters by description.
int select_adapter(bool warp, AdapterChoice& choice)
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&choice.factory));
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateDXGIFactory2 failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    if (warp) {
        hr = choice.factory->EnumWarpAdapter(IID_PPV_ARGS(&choice.adapter));
        if (FAILED(hr) || !choice.adapter) {
            std::fprintf(stderr, "WARP adapter unavailable: 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        hr = choice.adapter->GetDesc1(&choice.desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "WARP adapter description failed: 0x%08lx\n",
                         static_cast<unsigned long>(hr));
            return 1;
        }
        return 0;
    }

    wchar_t wanted[128]{};
    const DWORD filter = GetEnvironmentVariableW(L"XBOX2WINDOWS_ADAPTER", wanted, 128);
    if (filter >= 128) {
        std::fputs("XBOX2WINDOWS_ADAPTER is longer than 127 characters\n", stderr);
        return 1;
    }
    for (UINT index = 0;; ++index) {
        hr = choice.factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(choice.adapter.ReleaseAndGetAddressOf()));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            if (filter) {
                std::fprintf(stderr, "No D3D12 hardware adapter matches XBOX2WINDOWS_ADAPTER=%ls\n", wanted);
                return 1;
            }
            std::fputs("No physical D3D12 adapter available\n", stderr);
            return 77;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "EnumAdapterByGpuPreference failed: 0x%08lx\n",
                         static_cast<unsigned long>(hr));
            return 1;
        }
        hr = choice.adapter->GetDesc1(&choice.desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "GetDesc1 failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        if (choice.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (filter && !contains_ignoring_case(choice.desc.Description, wanted)) {
            std::fprintf(stderr, "XBOX2WINDOWS_ADAPTER: skipping %ls\n", choice.desc.Description);
            continue;
        }
        return 0;
    }
}

bool enable_debug_layer()
{
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) return false;
    debug->EnableDebugLayer();
    return true;
}

void print_environment(const AdapterChoice& choice, bool warp, bool debug)
{
    std::printf("backend=%s adapter=%ls vendor=%04x device=%04x\n", warp ? "warp" : "hardware",
                choice.desc.Description, choice.desc.VendorId, choice.desc.DeviceId);
    LARGE_INTEGER driver{};
    if (SUCCEEDED(choice.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
        std::printf("driver=%u.%u.%u.%u\n", HIWORD(driver.HighPart), LOWORD(driver.HighPart),
                    HIWORD(driver.LowPart), LOWORD(driver.LowPart));
    } else {
        std::puts("driver=unavailable via DXGI");
    }
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtl_get_version && rtl_get_version(&os) == 0) {
        std::printf("windows=%lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion,
                    os.dwBuildNumber);
    } else {
        std::puts("windows=unavailable");
    }
    std::printf("debug_layer=%s\n", debug ? "enabled" : "unavailable");
}

HMODULE load_shim(const char* path, XboxCreateDevice& create)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (length <= 0) {
        std::fputs("Cannot convert the d3d12_xs.dll path\n", stderr);
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), length)) {
        std::fputs("Cannot convert the d3d12_xs.dll path\n", stderr);
        return nullptr;
    }
    HMODULE module = LoadLibraryExW(wide.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryExW(%ls) failed: %lu\n", wide.c_str(),
                     static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }
    create = reinterpret_cast<XboxCreateDevice>(GetProcAddress(module, "D3D12XboxCreateDevice"));
    if (!create) {
        std::fputs("d3d12_xs.dll does not export D3D12XboxCreateDevice\n", stderr);
        FreeLibrary(module);
        return nullptr;
    }
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(create), &owner) ||
        owner != module) {
        std::fputs("D3D12XboxCreateDevice does not resolve inside d3d12_xs.dll\n", stderr);
        FreeLibrary(module);
        return nullptr;
    }
    return module;
}

ComPtr<ID3D12Device> create_shim_device(XboxCreateDevice create, IUnknown* adapter,
                                        const LUID& expected_luid)
{
    void* raw = nullptr;
    const HRESULT hr = create(adapter, nullptr, __uuidof(ID3D12Device), &raw);
    if (hr != S_OK || !raw) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return {};
    }
    ComPtr<ID3D12Device> device;
    device.Attach(static_cast<ID3D12Device*>(raw));
    const LUID actual = device->GetAdapterLuid();
    if (actual.LowPart != expected_luid.LowPart || actual.HighPart != expected_luid.HighPart ||
        device->GetNodeCount() != 1) {
        std::fputs("D3D12XboxCreateDevice returned the wrong adapter or node count\n", stderr);
        return {};
    }
    return device;
}

std::string state_name(D3D12_RESOURCE_STATES state)
{
    switch (state) {
    case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
    case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_DEST";
    case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_SOURCE";
    case D3D12_RESOURCE_STATE_GENERIC_READ: return "GENERIC_READ";
    case D3D12_RESOURCE_STATE_RENDER_TARGET: return "RENDER_TARGET";
    default: {
        char text[32]{};
        std::snprintf(text, sizeof(text), "STATE_0x%08x", static_cast<unsigned>(state));
        return text;
    }
    }
}

struct Tracked {
    std::string name;
    D3D12_RESOURCE_STATES state;
    bool fixed;
};

// Source-owned state model. It validates the sequence this fixture records; it is not a claim
// that the runtime rejects a wrong-state command when the debug layer is unavailable.
struct StateAudit {
    std::vector<Tracked> tracked;
    std::vector<std::string> failures;
    int transitions = 0;

    Tracked* find(const char* name)
    {
        for (auto& entry : tracked) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void track(const char* name, D3D12_RESOURCE_STATES state, bool fixed)
    {
        if (find(name)) throw std::runtime_error("duplicate tracked resource");
        tracked.push_back({name, state, fixed});
    }

    bool require(const char* name, D3D12_RESOURCE_STATES expected, const char* operation)
    {
        const Tracked* entry = find(name);
        if (!entry) {
            failures.push_back(std::string("wrong_state unknown_resource=") + name +
                               " operation=" + operation);
            return false;
        }
        if (entry->state != expected) {
            failures.push_back("wrong_state " + entry->name + "=" + state_name(entry->state) +
                               " expected=" + state_name(expected) + " operation=" + operation);
            return false;
        }
        return true;
    }

    bool transition(const char* name, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                    const char* operation)
    {
        Tracked* entry = find(name);
        if (!entry) {
            failures.push_back(std::string("wrong_state unknown_resource=") + name +
                               " operation=" + operation);
            return false;
        }
        if (entry->fixed) {
            failures.push_back("wrong_state " + entry->name +
                               " fixed_state=" + state_name(entry->state) +
                               " transition_to=" + state_name(after) + " operation=" + operation);
            return false;
        }
        if (entry->state != before) {
            failures.push_back("wrong_state " + entry->name + "=" + state_name(entry->state) +
                               " expected=" + state_name(before) + " operation=" + operation);
            return false;
        }
        entry->state = after;
        ++transitions;
        return true;
    }

    void print(const char* label) const
    {
        for (const auto& entry : tracked) {
            std::printf("%s state %s=%s%s\n", label, entry.name.c_str(),
                        state_name(entry.state).c_str(), entry.fixed ? " fixed" : "");
        }
        std::printf("%s state_audit transitions=%d failures=%zu\n", label, transitions,
                    failures.size());
    }
};

ComPtr<ID3D12RootSignature> create_root_signature(ID3D12Device* device)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0.NumParameters = 0;
    desc.Desc_1_0.pParameters = nullptr;
    desc.Desc_1_0.NumStaticSamplers = 0;
    desc.Desc_1_0.pStaticSamplers = nullptr;
    desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT serialize = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(serialize)) {
        if (error) {
            std::fprintf(stderr, "Root signature error: %.*s\n",
                         static_cast<int>(error->GetBufferSize()),
                         static_cast<const char*>(error->GetBufferPointer()));
        }
        check(serialize, "Serialize graphics-list root signature");
    }
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)),
          "Create graphics-list root signature");
    return root;
}

ComPtr<ID3D12PipelineState> create_pipeline(ID3D12Device* device, ID3D12RootSignature* root)
{
    const D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {d3d12x_graphics_vs, sizeof(d3d12x_graphics_vs)};
    desc.PS = {d3d12x_graphics_ps, sizeof(d3d12x_graphics_ps)};
    desc.InputLayout = {elements, 1};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)),
          "Create graphics-list pipeline");
    std::printf("pso vs_bytes=%zu ps_bytes=%zu rtv_format=%u topology=%u cull=%u\n",
                sizeof(d3d12x_graphics_vs), sizeof(d3d12x_graphics_ps),
                static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM),
                static_cast<unsigned>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE),
                static_cast<unsigned>(D3D12_CULL_MODE_NONE));
    return pipeline;
}

struct Fixture {
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> vertex_buffer;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
};

Fixture create_fixture(ID3D12Device* device)
{
    Fixture fixture;

    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = target_side;
    target_desc.Height = target_side;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::memcpy(clear.Color, clear_color, sizeof(clear_color));
    fixture.target = resource(device, target_desc, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COMMON, &clear);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    check(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&fixture.rtv_heap)),
          "Create graphics-list RTV heap");
    fixture.rtv = fixture.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(fixture.target.Get(), nullptr, fixture.rtv);

    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &fixture.footprint, nullptr, nullptr,
                                  nullptr);
    if (fixture.footprint.Footprint.Width != target_side ||
        fixture.footprint.Footprint.Height != target_side ||
        fixture.footprint.Footprint.Depth != 1 ||
        fixture.footprint.Footprint.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        fixture.footprint.Footprint.RowPitch < target_row_bytes) {
        std::fprintf(stderr, "unexpected copyable footprint %ux%ux%u format=%u row_pitch=%u\n",
                     fixture.footprint.Footprint.Width, fixture.footprint.Footprint.Height,
                     fixture.footprint.Footprint.Depth,
                     static_cast<unsigned>(fixture.footprint.Footprint.Format),
                     fixture.footprint.Footprint.RowPitch);
        throw std::runtime_error("graphics-list copyable footprint does not match the target");
    }
    const UINT64 readback_bytes =
        fixture.footprint.Offset +
        static_cast<UINT64>(fixture.footprint.Footprint.RowPitch) * target_side;
    fixture.readback = make_buffer(device, readback_bytes, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST);

    fixture.vertex_buffer = make_buffer(device, sizeof(fullscreen_triangle),
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(fixture.vertex_buffer->Map(0, &no_read, &mapped), "Map graphics-list vertex buffer");
    std::memcpy(mapped, fullscreen_triangle, sizeof(fullscreen_triangle));
    const D3D12_RANGE written{0, sizeof(fullscreen_triangle)};
    fixture.vertex_buffer->Unmap(0, &written);

    fixture.root = create_root_signature(device);
    fixture.pipeline = create_pipeline(device, fixture.root.Get());
    std::printf("fixture target=%ux%u format=%u row_pitch=%u offset=%llu readback_bytes=%llu "
                "vertices=3 vertex_bytes=%zu\n",
                target_side, target_side, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM),
                fixture.footprint.Footprint.RowPitch,
                static_cast<unsigned long long>(fixture.footprint.Offset),
                static_cast<unsigned long long>(readback_bytes), sizeof(fullscreen_triangle));
    return fixture;
}

struct GraphicsCommands {
    GraphicsCommands(ID3D12Device* device, ID3D12PipelineState* pipeline)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "Create graphics queue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)),
              "Create graphics allocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), pipeline,
                                        IID_PPV_ARGS(&list)),
              "Create graphics command list");
    }

    void submit_and_wait(ID3D12Device* device)
    {
        check(list->Close(), "Close graphics command list");
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "Create graphics fence");
        ID3D12CommandList* submitted[]{list.Get()};
        queue->ExecuteCommandLists(1, submitted);
        check(queue->Signal(fence.Get(), 1), "Signal graphics fence");
        check(fence->SetEventOnCompletion(1, nullptr), "Wait for graphics");
        check(device->GetDeviceRemovedReason(), "Graphics completion");
    }

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

struct RecordOptions {
    bool transition_render_target = true;
    bool draw = true;
    bool scissor = true;
};

struct RecordResult {
    int transitions = 0;
    int failures = 0;
};

RecordResult record(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root,
                    ID3D12PipelineState* pipeline, const Fixture& fixture,
                    const RecordOptions& options)
{
    StateAudit audit;
    audit.track("render_target", D3D12_RESOURCE_STATE_COMMON, false);
    audit.track("vertex_buffer", D3D12_RESOURCE_STATE_GENERIC_READ, true);
    audit.track("readback", D3D12_RESOURCE_STATE_COPY_DEST, true);

    const auto guard = [&audit](bool ok) {
        if (ok) return;
        const std::string failure = audit.failures.back();
        audit.print("wrong_state");
        throw std::runtime_error(failure);
    };

    list->SetGraphicsRootSignature(root);
    list->SetPipelineState(pipeline);

    if (options.transition_render_target) {
        guard(audit.transition("render_target", D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                               "ResourceBarrier(COMMON->RENDER_TARGET)"));
        transition(list, fixture.target.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    guard(audit.require("render_target", D3D12_RESOURCE_STATE_RENDER_TARGET,
                        "ClearRenderTargetView(rtv)"));
    list->ClearRenderTargetView(fixture.rtv, clear_color, 0, nullptr);

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target_side),
                                  static_cast<float>(target_side), 0.0f, 1.0f};
    const D3D12_RECT scissor = options.scissor
        ? D3D12_RECT{0, 0, static_cast<LONG>(target_side), static_cast<LONG>(target_side)}
        : D3D12_RECT{0, 0, 0, 0};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    guard(audit.require("render_target", D3D12_RESOURCE_STATE_RENDER_TARGET,
                        "OMSetRenderTargets(1, rtv, FALSE, nullptr)"));
    list->OMSetRenderTargets(1, &fixture.rtv, FALSE, nullptr);

    guard(audit.require("vertex_buffer", D3D12_RESOURCE_STATE_GENERIC_READ,
                        "IASetVertexBuffers(0, 1, vb)"));
    const D3D12_VERTEX_BUFFER_VIEW vertex_view{fixture.vertex_buffer->GetGPUVirtualAddress(),
                                               sizeof(fullscreen_triangle), sizeof(Vertex)};
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vertex_view);

    if (options.draw) {
        guard(audit.require("render_target", D3D12_RESOURCE_STATE_RENDER_TARGET,
                            "DrawInstanced(3, 1, 0, 0)"));
        list->DrawInstanced(3, 1, 0, 0);
    }

    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    guard(audit.transition("render_target", D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COPY_SOURCE,
                           "ResourceBarrier(RENDER_TARGET->COPY_SOURCE)"));
    transition(list, fixture.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);

    guard(audit.require("render_target", D3D12_RESOURCE_STATE_COPY_SOURCE,
                        "CopyTextureRegion(target->readback)"));
    guard(audit.require("readback", D3D12_RESOURCE_STATE_COPY_DEST,
                        "CopyTextureRegion(target->readback)"));
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = fixture.target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = fixture.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = fixture.footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    guard(audit.transition("render_target", D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_COMMON,
                           "ResourceBarrier(COPY_SOURCE->COMMON)"));
    transition(list, fixture.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);

    audit.print("record");
    std::printf("record draw=%s scissor=%s transitions=%d against=%u pixels\n",
                options.draw ? "3 vertices" : "omitted", options.scissor ? "32x32" : "0x0",
                audit.transitions, target_pixels);
    if (!audit.failures.empty()) throw std::runtime_error("graphics-list state audit failed");
    return {audit.transitions, static_cast<int>(audit.failures.size())};
}

std::array<std::uint32_t, target_pixels> read_back(Fixture& fixture)
{
    std::array<std::uint32_t, target_pixels> pixels{};
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(fixture.footprint.Offset +
        static_cast<UINT64>(fixture.footprint.Footprint.RowPitch) * target_side)};
    check(fixture.readback->Map(0, &read_range, &mapped), "Map graphics-list readback");
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + fixture.footprint.Offset;
    for (UINT y = 0; y < target_side; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            bytes + static_cast<std::size_t>(y) * fixture.footprint.Footprint.RowPitch);
        for (UINT x = 0; x < target_side; ++x) {
            pixels[y * target_side + x] = row[x];
        }
    }
    const D3D12_RANGE no_write{0, 0};
    fixture.readback->Unmap(0, &no_write);
    return pixels;
}

std::uint32_t fnv1a(const std::array<std::uint32_t, target_pixels>& pixels)
{
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t pixel : pixels) {
        hash ^= pixel;
        hash *= 16777619u;
    }
    return hash;
}

struct Comparison {
    UINT mismatches = 0;
    UINT first = UINT32_MAX;
    UINT first_x = 0;
    UINT first_y = 0;
    std::uint32_t hash = 0;
};

Comparison compare(const std::array<std::uint32_t, target_pixels>& pixels,
                   std::uint32_t expected)
{
    Comparison result;
    result.hash = fnv1a(pixels);
    for (UINT i = 0; i < target_pixels; ++i) {
        if (pixels[i] == expected) continue;
        if (!result.mismatches) {
            result.first = i;
            result.first_x = i % target_side;
            result.first_y = i / target_side;
        }
        ++result.mismatches;
    }
    return result;
}

void print_comparison(const char* label, const Comparison& comparison, std::uint32_t expected,
                      const std::array<std::uint32_t, target_pixels>& pixels)
{
    if (comparison.mismatches) {
        std::printf("%s pixels=%u expected=0x%08x hash=0x%08x mismatches=%u first=%u "
                    "first_x=%u first_y=%u\n",
                    label, target_pixels, expected, comparison.hash, comparison.mismatches,
                    comparison.first, comparison.first_x, comparison.first_y);
        std::printf("%s first_mismatch pixel=(%u,%u) expected=0x%08x actual=0x%08x\n", label,
                    comparison.first_x, comparison.first_y, expected,
                    pixels[comparison.first]);
    } else {
        std::printf("%s pixels=%u expected=0x%08x hash=0x%08x mismatches=0 first=none\n", label,
                    target_pixels, expected, comparison.hash);
    }
}

bool all_equal(const std::array<std::uint32_t, target_pixels>& pixels, std::uint32_t expected)
{
    for (std::uint32_t pixel : pixels) {
        if (pixel != expected) return false;
    }
    return true;
}

bool run_positive(ID3D12Device* device)
{
    Fixture fixture = create_fixture(device);
    GraphicsCommands commands(device, nullptr);
    const RecordResult record_result = record(commands.list.Get(), fixture.root.Get(),
                                              fixture.pipeline.Get(), fixture, RecordOptions{});
    if (record_result.failures != 0 || record_result.transitions != 3) {
        std::fprintf(stderr, "positive state audit: transitions=%d failures=%d expected=3/0\n",
                     record_result.transitions, record_result.failures);
        return false;
    }
    commands.submit_and_wait(device);
    const auto pixels = read_back(fixture);
    const Comparison comparison = compare(pixels, triangle_pixel);
    print_comparison("positive", comparison, triangle_pixel, pixels);
    if (comparison.mismatches != 0) return false;
    std::puts("positive draw=matched");
    return true;
}

bool run_image_control(ID3D12Device* device, const char* label, const RecordOptions& options)
{
    Fixture fixture = create_fixture(device);
    GraphicsCommands commands(device, nullptr);
    record(commands.list.Get(), fixture.root.Get(), fixture.pipeline.Get(), fixture, options);
    commands.submit_and_wait(device);
    const auto pixels = read_back(fixture);
    const Comparison comparison = compare(pixels, triangle_pixel);
    print_comparison(label, comparison, triangle_pixel, pixels);
    const bool clear = all_equal(pixels, clear_pixel);
    const bool ok = comparison.mismatches == target_pixels && comparison.first == 0 && clear;
    std::printf("negative %s expected_mismatches=%u mismatches=%u first=%u clear=%s\n", label,
                target_pixels, comparison.mismatches, comparison.first, clear ? "yes" : "no");
    return ok;
}

bool run_wrong_state(ID3D12Device* device)
{
    Fixture fixture = create_fixture(device);
    GraphicsCommands commands(device, nullptr);
    bool rejected = false;
    std::string diagnostic;
    try {
        RecordOptions options;
        options.transition_render_target = false;
        record(commands.list.Get(), fixture.root.Get(), fixture.pipeline.Get(), fixture, options);
    } catch (const std::exception& error) {
        rejected = true;
        diagnostic = error.what();
    }
    commands.list->Close();
    const bool named =
        diagnostic.find("wrong_state render_target=COMMON expected=RENDER_TARGET "
                        "operation=ClearRenderTargetView(rtv)") == 0;
    std::printf("negative wrong_state controls=1 rejected=%s named=%s diagnostic=\"%s\"\n",
                rejected ? "yes" : "no", named ? "yes" : "no", diagnostic.c_str());
    return rejected && named;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <path-to-d3d12_xs.dll> <warp|hardware>\n", argv[0]);
        return 2;
    }
    const std::string_view backend = argv[2];
    if (backend != "warp" && backend != "hardware") {
        std::fputs("unknown backend\n", stderr);
        return 2;
    }
    const bool warp = backend == "warp";

    XboxCreateDevice create = nullptr;
    HMODULE shim = load_shim(argv[1], create);
    if (!shim) return 1;

    AdapterChoice choice;
    const int selected = select_adapter(warp, choice);
    if (selected) {
        FreeLibrary(shim);
        return selected;
    }
    const bool debug = enable_debug_layer();
    print_environment(choice, warp, debug);

    ComPtr<ID3D12Device> device =
        create_shim_device(create, choice.adapter.Get(), choice.desc.AdapterLuid);
    int result = 1;
    if (device) {
        try {
            const bool positive = run_positive(device.Get());

            RecordOptions omitted_draw;
            omitted_draw.draw = false;
            const bool no_draw = run_image_control(device.Get(), "omitted_draw", omitted_draw);

            RecordOptions empty_scissor;
            empty_scissor.scissor = false;
            const bool clipped = run_image_control(device.Get(), "empty_scissor", empty_scissor);

            const bool wrong_state = run_wrong_state(device.Get());
            result = positive && no_draw && clipped && wrong_state ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "d3d12x_graphics_list_check: %s\n", error.what());
            result = 1;
        }
    }
    device.Reset();
    FreeLibrary(shim);
    std::printf("d3d12x_graphics_list_check %s: %s\n", argv[2],
                result == 0 ? "PASS" : "FAIL");
    return result;
}
