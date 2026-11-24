// Checks src/startup_stubs/present_view_format.h. No argument: the format table, without a device.
// "warp" or "hardware": the presenter's read of typeless textures, as a compute readback of the
// texels through the mapped view against a CPU decode of the bytes that were uploaded.
// --mutate identity: the table with the old behaviour (the typeless format unchanged); it must fail.
// --mutate srgb: the device run with an sRGB view of the RGBA8 texture; it must fail on a texel.
#include "startup_stubs/present_view_format.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
int failures;
bool identity, srgb;

DXGI_FORMAT map_format(DXGI_FORMAT format) { return identity ? format : x2w_startup::present_view_format(format); }

void expect(const char* name, DXGI_FORMAT format, DXGI_FORMAT wanted)
{
    if (map_format(format) != wanted) {
        std::printf("FAIL %s: format %d -> %d, expected %d\n", name, format, map_format(format), wanted);
        ++failures;
    }
}

void table()
{
    constexpr DXGI_FORMAT none = DXGI_FORMAT_UNKNOWN;
    expect("rgba8", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM);
    expect("bgra8", DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM);
    expect("bgrx8", DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM);
    expect("rgb10a2", DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM);
    for (DXGI_FORMAT typed : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_BC1_UNORM,
                              static_cast<DXGI_FORMAT>(191) /* DXGI_FORMAT_A4B4G4R4_UNORM */})
        expect("typed passes", typed, typed);
    for (DXGI_FORMAT ambiguous : {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                  DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R16G16_TYPELESS,
                                  DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R8G8_TYPELESS,
                                  DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC2_TYPELESS,
                                  DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC6H_TYPELESS,
                                  DXGI_FORMAT_BC7_TYPELESS})
        expect("ambiguous refused", ambiguous, none);
    expect("unknown refused", DXGI_FORMAT_UNKNOWN, none);
}

void expect_supported(const char* name, const D3D12_RESOURCE_DESC& desc, bool wanted)
{
    const bool got = x2w_startup::present_surface_supported(desc);
    if (got != wanted) {
        std::printf("FAIL %s: supported=%d, expected %d (dim=%d samples=%u flags=0x%x fmt=%d)\n",
                    name, got ? 1 : 0, wanted ? 1 : 0, desc.Dimension, desc.SampleDesc.Count, desc.Flags, desc.Format);
        ++failures;
    }
}

void surface_support()
{
    D3D12_RESOURCE_DESC tex2d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    expect_supported("2d rgba8 unorm", tex2d, true);

    D3D12_RESOURCE_DESC rt = tex2d;
    rt.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    rt.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    expect_supported("2d typeless rt", rt, true);

    D3D12_RESOURCE_DESC deny = tex2d;
    deny.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    expect_supported("2d deny_srv", deny, false);

    D3D12_RESOURCE_DESC dsv = tex2d;
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    expect_supported("2d dsv deny_srv", dsv, false);

    D3D12_RESOURCE_DESC buf{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    expect_supported("buffer refused", buf, false);

    D3D12_RESOURCE_DESC tex1d = tex2d;
    tex1d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    tex1d.Height = 1;
    expect_supported("1d refused", tex1d, false);

    D3D12_RESOURCE_DESC tex3d = tex2d;
    tex3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    expect_supported("3d refused", tex3d, false);

    D3D12_RESOURCE_DESC msaa = tex2d;
    msaa.SampleDesc.Count = 4;
    expect_supported("msaa4 refused", msaa, false);

    D3D12_RESOURCE_DESC zero_samples = tex2d;
    zero_samples.SampleDesc.Count = 0;
    expect_supported("samples0 refused", zero_samples, false);

    D3D12_RESOURCE_DESC zero_width = tex2d;
    zero_width.Width = 0;
    expect_supported("width0 refused", zero_width, false);

    D3D12_RESOURCE_DESC zero_height = tex2d;
    zero_height.Height = 0;
    expect_supported("height0 refused", zero_height, false);

    D3D12_RESOURCE_DESC unknown_fmt = tex2d;
    unknown_fmt.Format = DXGI_FORMAT_UNKNOWN;
    expect_supported("unknown fmt refused", unknown_fmt, false);

    D3D12_RESOURCE_DESC ambig_fmt = tex2d;
    ambig_fmt.Format = DXGI_FORMAT_R32_TYPELESS;
    expect_supported("ambiguous fmt refused", ambig_fmt, false);
}

void check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed");
}

struct Case {
    const char* name;
    DXGI_FORMAT typeless;
    DXGI_FORMAT view;
    std::vector<unsigned> texel;  // 16 texels, 4 bytes each, little-endian
    float (*decode)(unsigned, int);
};

float decode_rgba8(unsigned t, int c) { return static_cast<float>((t >> (8 * c)) & 255) / 255.0f; }
float decode_bgra8(unsigned t, int c) { return decode_rgba8(t, c == 0 ? 2 : c == 2 ? 0 : c); }
float decode_rgb10a2(unsigned t, int c) { return c == 3 ? static_cast<float>(t >> 30) / 3.0f : static_cast<float>((t >> (10 * c)) & 1023) / 1023.0f; }

bool readback(ID3D12Device* device, const Case& item)
{
    constexpr UINT side = 4, pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, out_bytes = side * side * 16;
    const auto heap = [](D3D12_HEAP_TYPE type) { D3D12_HEAP_PROPERTIES p{}; p.Type = type; return p; };
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    const auto make_buffer = [&](D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> r;
        buffer.Width = bytes;
        buffer.Flags = flags;
        const D3D12_HEAP_PROPERTIES properties = heap(type);
        check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
        return r;
    };
    auto upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, pitch * side, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto out = make_buffer(D3D12_HEAP_TYPE_DEFAULT, out_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto back = make_buffer(D3D12_HEAP_TYPE_READBACK, out_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    unsigned char* mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
    for (UINT y = 0; y < side; ++y) std::memcpy(mapped + y * pitch, &item.texel[y * side], side * 4);
    upload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, item.typeless, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> texture;
    const D3D12_HEAP_PROPERTIES defaults = heap(D3D12_HEAP_TYPE_DEFAULT);
    check(device->CreateCommittedResource(&defaults, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "texture");

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> heaps;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heaps)), "heap");
    auto cpu = heaps->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = srgb && item.view == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : item.view;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.Get(), &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = out_bytes / 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateUnorderedAccessView(out.Get(), nullptr, &uav, cpu);

    static const char source[] =
        "Texture2D<float4> src : register(t0); RWByteAddressBuffer dst : register(u0);\n"
        "[numthreads(4, 4, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
        "{ dst.Store4((id.y * 4 + id.x) * 16, asuint(src.Load(int3(id.xy, 0)))); }\n";
    ComPtr<ID3DBlob> code, signature, errors;
    check(D3DCompile(source, sizeof(source) - 1, "view", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors), "compile");
    const D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "serialize");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipe{};
    pipe.pRootSignature = root.Get();
    pipe.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipe, IID_PPV_ARGS(&pipeline)), "pipeline");

    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)), "list");
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    from.PlacedFootprint.Footprint = {item.typeless, side, side, 1, pitch};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    const auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    };
    transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* bound[] = {heaps.Get()};
    list->SetDescriptorHeaps(1, bound);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heaps->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    transition(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, out.Get(), 0, out_bytes);
    check(list->Close(), "close");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), 1), "signal");
    check(fence->SetEventOnCompletion(1, done), "event");
    WaitForSingleObject(done, 10000);
    CloseHandle(done);
    if (fence->GetCompletedValue() < 1) throw std::runtime_error("readback timed out");

    const float* words = nullptr;
    check(back->Map(0, nullptr, reinterpret_cast<void**>(const_cast<float**>(&words))), "map readback");
    int bad = 0;
    for (int i = 0; i < 16 && bad < 3; ++i)
        for (int c = 0; c < 4; ++c) {
            const float expected = item.decode(item.texel[i], c);
            if (!(std::fabs(words[i * 4 + c] - expected) <= 1e-4f)) {
                std::printf("FAIL %s texel %d channel %d: read %f, expected %f\n", item.name, i, c, words[i * 4 + c], expected);
                ++bad;
                break;
            }
        }
    back->Unmap(0, nullptr);
    return bad == 0;
}

void verify_surface_guards(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES defaults{};
    defaults.Type = D3D12_HEAP_TYPE_DEFAULT;

    const auto create = [&](const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, const char* label) {
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&defaults, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)), label);
        return resource;
    };

    const D3D12_RESOURCE_DESC dsv_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE};
    auto dsv = create(dsv_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, "dsv deny_srv");
    expect_supported("warp dsv deny_srv", dsv->GetDesc(), false);

    const D3D12_RESOURCE_DESC buf_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    auto buf = create(buf_desc, D3D12_RESOURCE_STATE_COMMON, "buffer");
    expect_supported("warp buffer", buf->GetDesc(), false);

    const D3D12_RESOURCE_DESC msaa_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {4, 0},
                                        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto msaa = create(msaa_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, "msaa4");
    expect_supported("warp msaa4", msaa->GetDesc(), false);

    const D3D12_RESOURCE_DESC rt_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                      D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto rt = create(rt_desc, D3D12_RESOURCE_STATE_COMMON, "valid rt");
    const D3D12_RESOURCE_DESC actual_rt = rt->GetDesc();
    expect_supported("warp valid rt", actual_rt, true);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "guard srv heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = x2w_startup::present_view_format(actual_rt.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(rt.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());
    check(device->GetDeviceRemovedReason(), "device removed reason");
}

ComPtr<ID3D12Device> create_device(bool warp)
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> adapter;
    if (warp) {
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "warp adapter");
    } else {
        for (UINT i = 0; factory->EnumAdapters(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC desc{};
            adapter->GetDesc(&desc);
            if (desc.VendorId != 0x1414) break;  // not the software adapter
            adapter.Reset();
        }
    }
    ComPtr<ID3D12Device> device;
    if (adapter && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return device;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* adapter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mutate") && i + 1 < argc) {
            ++i;
            identity = !std::strcmp(argv[i], "identity");
            srgb = !std::strcmp(argv[i], "srgb");
        } else {
            adapter = argv[i];
        }
    }
    if (!adapter) {
        table();
        surface_support();
        if (failures) {
            std::printf("startup_present_view: %d FAILED\n", failures);
            return 1;
        }
        std::printf("startup_present_view: PASS (4 mapped, 6 typed, 18 refused, 2 invalid, 13 surface checks)\n");
        return 0;
    }
    try {
        auto device = create_device(!std::strcmp(adapter, "warp"));
        if (!device) return std::strcmp(adapter, "warp") ? 77 : 1;
        std::vector<unsigned> rgba(16), rgb10(16);
        for (unsigned i = 0; i < 16; ++i) {
            // Byte 0..3 are r, g, b, a for the RGBA family; a 128 value sits in each of the first channels.
            rgba[i] = ((i * 16 + 3) | (255 - i * 16) << 8 | ((i * 37) & 255) << 16 | ((i * 53 + 7) & 255) << 24) & 0xffffffffu;
            rgb10[i] = ((i * 61 + 5) & 1023) | ((i * 97 + 11) & 1023) << 10 | ((i * 131 + 17) & 1023) << 20 | (i & 3) << 30;
        }
        rgba[5] = 0x80808080u;
        const Case cases[] = {
            {"rgba8", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, rgba, decode_rgba8},
            {"bgra8", DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, rgba, decode_bgra8},
            {"rgb10a2", DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, rgb10, decode_rgb10a2},
        };
        for (const Case& item : cases) {
            Case run = item;
            run.view = map_format(item.typeless);
            if (!readback(device.Get(), run)) ++failures;
        }
        verify_surface_guards(device.Get());
    } catch (const std::exception& error) {
        std::printf("startup_present_view: %s\n", error.what());
        return 1;
    }
    if (failures) return 1;
    std::printf("startup_present_view_%s: PASS (3 families read back)\n", adapter);
    return 0;
}
