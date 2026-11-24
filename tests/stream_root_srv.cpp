#include "desktop_gpu.h"
#include "stream_root_srv_vs.h"
#include "stream_root_srv_ps.h"

#include <array>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind, typename T>
struct alignas(void*) Field {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind = Kind;
    T value{};
};

struct GraphicsStream {
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Field<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
};

enum class Case { baseline, wrong_stride, wrong_binding };

void draw(ID3D12Device* device, ID3D12RootSignature* root, ID3D12PipelineState* pipeline, Case scenario)
{
    struct Vertex { float x, y; float color[4]; };
    struct Upload { UINT stride; UINT padding[3]; Vertex vertices[3]; };
    const Upload input{scenario == Case::wrong_stride ? 0u : UINT(sizeof(Vertex)), {},
        {{-1, -1, {0, 1, 0, 1}}, {-1, 3, {1, 0, 0, 1}}, {3, -1, {0, 0, 1, 1}}}};
    constexpr UINT alternate_offset = 256;
    static_assert(sizeof(input) <= alternate_offset);
    auto alternate = input;
    alternate.vertices[0].color[0] = 1;
    alternate.vertices[0].color[1] = 0;
    auto upload = make_buffer(device, alternate_offset + sizeof(input), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{};
    const D3D12_RANGE no_reads{};
    check(upload->Map(0, &no_reads, &mapped), "Map vertex data");
    std::memcpy(mapped, &input, sizeof(input));
    std::memcpy(static_cast<BYTE*>(mapped) + alternate_offset, &alternate, sizeof(alternate));
    upload->Unmap(0, nullptr);

    constexpr UINT side = 8;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = side;
    texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{texture.Format, {1, 0, 1, 1}};
    auto target = resource(device, texture, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create RTV heap");
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, handle);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    ComputeCommands commands(device, pipeline, D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = commands.list.Get();
    list->SetGraphicsRootSignature(root);
    const UINT offset = scenario == Case::wrong_binding ? alternate_offset : 0;
    list->SetGraphicsRootShaderResourceView(0, upload->GetGPUVirtualAddress() + offset);
    const D3D12_VIEWPORT viewport{0, 0, side, side, 0, 1};
    const D3D12_RECT scissor{0, 0, side, side};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    list->ClearRenderTargetView(handle, clear.Color, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    commands.submit_and_wait(device);

    const std::array<BYTE, 4> green{0, 255, 0, 255}, red{255, 0, 0, 255}, magenta{255, 0, 255, 255};
    auto expected = green;
    if (scenario == Case::wrong_stride) expected = magenta;
    if (scenario == Case::wrong_binding) expected = red;
    check(readback->Map(0, nullptr, &mapped), "Map stream draw pixels");
    UINT mismatches{};
    for (UINT y = 0; y < side; ++y) {
        const auto* row = static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < side; ++x)
            mismatches += std::memcmp(row + x * 4, expected.data(), expected.size()) != 0;
    }
    readback->Unmap(0, &no_reads);
    std::printf("root_srv_offset=%u stride=%u pixels=%u mismatches=%u\n", offset, input.stride, side * side, mismatches);
    if (mismatches) throw std::runtime_error("Graphics stream root SRV pixels differ");
}
}

void check_stream_root_srv(ID3D12Device* device)
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameter.Descriptor = {0, 3};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter};
    ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "Serialize stream root");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "Create stream root");
    GraphicsStream stream{};
    stream.root.value = root.Get();
    stream.vs.value = {stream_root_srv_vs, sizeof(stream_root_srv_vs)};
    stream.ps.value = {stream_root_srv_ps, sizeof(stream_root_srv_ps)};
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.formats.value.NumRenderTargets = 1;
    stream.formats.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.raster.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.raster.value.DepthClipEnable = TRUE;
    stream.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ComPtr<ID3D12Device2> device2;
    check(device->QueryInterface(IID_PPV_ARGS(&device2)), "Query pipeline stream device");
    ComPtr<ID3D12PipelineState> pipeline;
    check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create graphics stream");
    draw(device, root.Get(), pipeline.Get(), Case::baseline);
    draw(device, root.Get(), pipeline.Get(), Case::wrong_stride);
    draw(device, root.Get(), pipeline.Get(), Case::wrong_binding);
}
