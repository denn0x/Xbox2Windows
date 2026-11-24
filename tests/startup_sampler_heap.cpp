// Desktop readback for tools/startup/gfx2hlsl.py's run-time sampler heap indexing (#1143).
//
// The translated pixel shader samples r0 : register(t0) through qh[offset / 16], a bounded
// SamplerState table at register(s0, space1), with the byte offset taken from an entry constant
// (cbuffer entry : register(b0)). This draws one pixel with that shader and a source-built
// full-screen vertex shader, then reads the colour back.
//
// The texture is 2x1 R32_FLOAT {0.25, 0.75}, sampled at u = 1.25 by the fixture shader. The table
// holds point samplers: entry 0 wraps (0.25), entry 1 clamps (0.75), and missing entries use a
// transparent-black border (0.0). The reserved fallback has a fixture-specific border of 0.5.
#include "desktop_gpu.h"
#include "startup_stubs/translated_sampler_bindings.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT entry_constants = 4;  // ud0..ud3 of the fixture's manifest

std::vector<char> read_blob(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) throw std::runtime_error(std::string("Cannot read ") + path);
    if (bytes.empty()) throw std::runtime_error(std::string("Empty shader ") + path);
    return bytes;
}

x2w_startup::TranslatedSamplers read_samplers(const char* manifest)
{
    x2w_startup::TranslatedSamplers result;
    if (!manifest) {
        result.heap = true;
        result.reserved_fallback = true;
        return result;
    }
    std::ifstream file(manifest);
    if (!file) throw std::runtime_error("Cannot open sampler manifest");
    for (std::string line; std::getline(file, line);)
        if (!result.read_manifest_line(line.c_str())) throw std::runtime_error("Invalid sampler heap manifest");
    return result;
}

ComPtr<ID3D12DescriptorHeap> fixture_heap(ID3D12Device* device, bool clamp, bool reserved_fallback)
{
    using namespace x2w_startup;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto free_page = [](unsigned char* address) { VirtualFree(address, 0, MEM_RELEASE); };
    std::unique_ptr<unsigned char, decltype(free_page)> memory(static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 2 * SIZE_T{system.dwPageSize}, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)), free_page);
    if (!memory) throw std::runtime_error("Allocate fixture sampler pages");
    DWORD old = 0;
    auto* edge = memory.get() + system.dwPageSize;
    if (!VirtualProtect(edge, system.dwPageSize, PAGE_NOACCESS, &old))
        throw std::runtime_error("Protect fixture sampler page");
    const SamplerWords words[2]{{0, 0, 0, 0}, {clamp ? 2u : 0u, 0, 0, 0}};
    auto* begin = edge - sizeof(words);
    std::memcpy(begin, words, sizeof(words));
    const auto snapshot = snapshot_samplers({reinterpret_cast<std::uintptr_t>(begin), 3}, reserved_fallback);
    memory.reset();
    ComPtr<ID3D12DescriptorHeap> heap;
    NullSamplerCounts counts;
    auto reserved = border_sampler();
    for (auto& component : reserved.BorderColor) component = 0.5f;
    check(create_sampler_snapshot_heap(device, snapshot, [](const SamplerWords& input, D3D12_SAMPLER_DESC& out) {
        out = dynamic_sampler(input);
        return true;
    }, heap.GetAddressOf(), counts, &reserved), "Create and populate sampler snapshot");
    const UINT capacity = reserved_fallback ? translated_sampler_fallback : translated_sampler_count;
    if (counts.unbound != 0 || counts.unreadable != 1 || counts.invalid_token != 0 ||
        counts.out_of_range != capacity - 3) throw std::runtime_error("Unexpected null sampler counts");
    std::printf("null_sampler_unreadable=%u null_sampler_out_of_range=%u\n", counts.unreadable, counts.out_of_range);
    return heap;
}
}  // namespace

float check_startup_sampler_heap(ID3D12Device* device, const char* vertex_path, const char* pixel_path,
                                 std::uint32_t byte_offset, const char* manifest, const char* binding_case)
{
    const auto vs = read_blob(vertex_path);
    const auto ps = read_blob(pixel_path);
    const auto bindings = read_samplers(manifest);
    const std::string_view scenario = binding_case ? binding_case : "";
    const bool rebind = scenario == "rebind" || scenario == "stale";
    if ((!scenario.empty() && !rebind) || (rebind && (!bindings.heap || byte_offset != 16)))
        throw std::runtime_error("Invalid sampler rebinding case");
    ComPtr<ID3D12RootSignature> root;
    check(x2w_startup::create_translated_root(device, entry_constants, 1, bindings, root.GetAddressOf()),
          "Create translated sampler root signature");
    std::printf("startup_sampler_heap static_samplers=%zu dynamic_table=%u\n",
                bindings.statics.size(), bindings.heap ? 1u : 0u);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)), "Create sampler heap PSO");

    // 2x1 R32_FLOAT {0.25, 0.75}.
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    const auto texture = resource(device, td, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &upload_size);
    const auto upload = make_buffer(device, upload_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    check(upload->Map(0, &none, &mapped), "Map sampler heap texels");
    const float texels[2] = {0.25f, 0.75f};
    std::memcpy(static_cast<std::uint8_t*>(mapped) + footprint.Offset, texels, sizeof(texels));
    upload->Unmap(0, nullptr);

    // One float4 pixel per draw. Both rebinding draws are recorded before
    // submission, so the first snapshot must survive the heap change.
    D3D12_RESOURCE_DESC rd = td;
    rd.Width = rebind ? 2 : 1;
    rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R32G32B32A32_FLOAT, {-1.0f, -1.0f, -1.0f, -1.0f}};
    const auto target = resource(device, rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_footprint{};
    UINT64 readback_size = 0;
    device->GetCopyableFootprints(&rd, 0, 1, 0, &target_footprint, nullptr, nullptr, &readback_size);
    const auto readback = make_buffer(device, readback_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> views, rtvs;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views)), "Create sampler heap view heap");
    device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
    const auto sampler_heap = bindings.heap ? fixture_heap(device, true, bindings.reserved_fallback)
                                            : ComPtr<ID3D12DescriptorHeap>{};
    const auto next_heap = rebind ? fixture_heap(device, false, bindings.reserved_fallback)
                                 : ComPtr<ID3D12DescriptorHeap>{};
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "Create sampler heap RTV heap");
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    ComputeCommands commands(device, pipeline.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = commands.list.Get();
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    list->SetGraphicsRootSignature(root.Get());
    // ud0 is the rectangle-list switch (0: plain triangles), ud1 the sampler heap byte offset.
    const std::uint32_t constants[entry_constants] = {0, byte_offset, 0, 0};
    list->SetGraphicsRoot32BitConstants(0, entry_constants, constants, 0);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto draw_pixel = [&](LONG x, ID3D12DescriptorHeap* heap) {
        x2w_startup::bind_translated_heaps(list, views.Get(), heap);
        list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
        if (heap) list->SetGraphicsRootDescriptorTable(x2w_startup::sampler_root_parameter(1), heap->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{static_cast<float>(x), 0, 1, 1, 0, 1};
        const D3D12_RECT scissor{x, 0, x + 1, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->DrawInstanced(3, 1, 0, 0);
    };
    draw_pixel(0, sampler_heap.Get());
    if (rebind) draw_pixel(1, scenario == "stale" ? sampler_heap.Get() : next_heap.Get());
    transition(list, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION out{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    out.PlacedFootprint = target_footprint;
    D3D12_TEXTURE_COPY_LOCATION in{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&out, 0, 0, 0, &in, nullptr);
    commands.submit_and_wait(device);

    float color[8]{};
    const SIZE_T read_bytes = static_cast<SIZE_T>(rd.Width) * sizeof(float) * 4;
    const D3D12_RANGE read{0, read_bytes};
    check(readback->Map(0, &read, &mapped), "Map sampler heap readback");
    std::memcpy(color, static_cast<std::uint8_t*>(mapped) + target_footprint.Offset, read_bytes);
    readback->Unmap(0, &none);
    if (rebind && color[0] != 0.75f) throw std::runtime_error("First recorded sampler heap changed before execution");
    const float red = color[rebind ? 4 : 0];
    std::uint32_t bits;
    std::memcpy(&bits, &red, sizeof(bits));
    std::printf("startup_sampler_heap offset=%u entry=%u red=%08x (%g)\n", byte_offset, byte_offset / 16, bits, red);
    return red;
}
