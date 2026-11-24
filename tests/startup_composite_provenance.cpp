// Composite provenance (#1628): which value a composite leaves in its destination.
//
// One command list, two passes. Pass 1 renders the composite source: cleared to a base colour, then a centre
// block and a corner block drawn with their own colours. The destination is cleared to a third colour. The list
// copies the source, the destination immediately before the composite draw and the destination immediately after
// it into readback buffers, and one fence wait covers all three. The composite copies exact source texels
// (Load at floor(position) * scale + offset), so each destination pixel is either an exact source texel or the
// retained destination colour, and a CPU model built from the two first readbacks predicts the third exactly.
//
// Cases: a drawn centre (the composite covers the centre and copies it), a clipped centre (the scissor keeps
// the composite off the centre, which retains the destination colour) and a UV that selects the source corner.
// The destination centres after the three cases are the source centre colour, the retained destination colour
// and the corner colour, so the readback distinguishes them. Two mutants, run against the unchanged model,
// must each mismatch: the corner case with its UV moved to the centre, and the drawn case with a write mask that
// drops green and blue.
#include "desktop_gpu.h"
#include "startup_composite_provenance_composite.h"
#include "startup_composite_provenance_fill.h"
#include "startup_composite_provenance_vs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT side = 64;
constexpr DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
using Color = std::array<float, 4>;
constexpr Color base{0.0f, 0.0f, 1.0f, 1.0f};        // source outside both blocks: blue
constexpr Color centre{0.0f, 1.0f, 0.0f, 1.0f};      // source centre block [16, 48)^2: green
constexpr Color corner{1.0f, 0.0f, 0.0f, 1.0f};      // source corner block [0, 8)^2: red
constexpr Color retained{1.0f, 0.0f, 1.0f, 1.0f};    // destination before the composite: magenta
constexpr D3D12_RECT centre_block{16, 16, 48, 48}, corner_block{0, 0, 8, 8};

struct Case {
    const char* name;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    std::int32_t scale[2], offset[2];
    UINT8 mask;
};
constexpr D3D12_VIEWPORT full_viewport{0, 0, side, side, 0, 1};
constexpr D3D12_RECT full_scissor{0, 0, static_cast<LONG>(side), static_cast<LONG>(side)};

struct Capture {
    std::vector<Color> source, before, after;
};

ComPtr<ID3D12RootSignature> create_root(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "Serialize composite root");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "Create composite root");
    return root;
}

ComPtr<ID3D12PipelineState> create_pso(ID3D12Device* device, ID3D12RootSignature* root, const void* ps,
                                       std::size_t ps_bytes, UINT8 mask)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {startup_composite_provenance_vs, sizeof(startup_composite_provenance_vs)};
    pd.PS = {ps, ps_bytes};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;  // blending stays disabled
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)), "Create composite PSO");
    return pipeline;
}

// Draws pass 1 and the composite on one list, with the three readbacks, and waits on one fence.
Capture run(ID3D12Device* device, ID3D12RootSignature* root, const Case& c)
{
    const auto fill = create_pso(device, root, startup_composite_provenance_fill, sizeof(startup_composite_provenance_fill),
                                 D3D12_COLOR_WRITE_ENABLE_ALL);
    const auto composite = create_pso(device, root, startup_composite_provenance_composite,
                                      sizeof(startup_composite_provenance_composite), c.mask);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = side;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE source_clear{format}, destination_clear{format};
    std::memcpy(source_clear.Color, base.data(), sizeof(source_clear.Color));
    std::memcpy(destination_clear.Color, retained.data(), sizeof(destination_clear.Color));
    const auto source = resource(device, rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, &source_clear);
    const auto destination = resource(device, rd, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      &destination_clear);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> readbacks[3];
    for (auto& readback : readbacks)
        readback = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    ComPtr<ID3D12DescriptorHeap> rtvs;
    check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)), "Create composite RTV heap");
    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto source_rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    auto destination_rtv = source_rtv;
    destination_rtv.ptr += rtv_step;
    device->CreateRenderTargetView(source.Get(), nullptr, source_rtv);
    device->CreateRenderTargetView(destination.Get(), nullptr, destination_rtv);
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> srvs;
    check(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)), "Create composite SRV heap");
    device->CreateShaderResourceView(source.Get(), nullptr, srvs->GetCPUDescriptorHandleForHeapStart());

    ComputeCommands commands(device, fill.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = commands.list.Get();
    const auto copy_out = [&](ID3D12Resource* texture, ID3D12Resource* readback) {
        D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    };
    // Pass 1: the composite source.
    list->SetGraphicsRootSignature(root);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &full_viewport);
    list->ClearRenderTargetView(source_rtv, source_clear.Color, 0, nullptr);
    list->OMSetRenderTargets(1, &source_rtv, FALSE, nullptr);
    for (const auto& [colour, block] : {std::pair{centre, centre_block}, std::pair{corner, corner_block}}) {
        list->SetGraphicsRoot32BitConstants(0, 4, colour.data(), 0);
        list->RSSetScissorRects(1, &block);
        list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list, source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_out(source.Get(), readbacks[0].Get());
    transition(list, source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // The destination as the composite finds it.
    list->ClearRenderTargetView(destination_rtv, destination_clear.Color, 0, nullptr);
    transition(list, destination.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_out(destination.Get(), readbacks[1].Get());
    transition(list, destination.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The composite.
    list->SetPipelineState(composite.Get());
    ID3D12DescriptorHeap* heaps[] = {srvs.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(1, srvs->GetGPUDescriptorHandleForHeapStart());
    const std::int32_t uv[4] = {c.scale[0], c.scale[1], c.offset[0], c.offset[1]};
    list->SetGraphicsRoot32BitConstants(0, 4, uv, 4);
    list->RSSetViewports(1, &c.viewport);
    list->RSSetScissorRects(1, &c.scissor);
    list->OMSetRenderTargets(1, &destination_rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, destination.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_out(destination.Get(), readbacks[2].Get());
    commands.submit_and_wait(device);

    Capture capture;
    std::vector<Color>* outs[3] = {&capture.source, &capture.before, &capture.after};
    for (int i = 0; i < 3; ++i) {
        void* mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)}, none{0, 0};
        check(readbacks[i]->Map(0, &read, &mapped), "Map composite readback");
        outs[i]->resize(side * side);
        for (UINT y = 0; y < side; ++y)
            std::memcpy(outs[i]->data() + y * side,
                        static_cast<std::uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                        side * sizeof(Color));
        readbacks[i]->Unmap(0, &none);
    }
    return capture;
}

bool inside(const D3D12_RECT& r, UINT x, UINT y)
{
    return static_cast<LONG>(x) >= r.left && static_cast<LONG>(x) < r.right && static_cast<LONG>(y) >= r.top &&
           static_cast<LONG>(y) < r.bottom;
}

// The destination after the composite, predicted from the source and the destination before it.
std::vector<Color> model(const Capture& capture, const Case& c)
{
    std::vector<Color> out = capture.before;
    const D3D12_RECT viewport{static_cast<LONG>(c.viewport.TopLeftX), static_cast<LONG>(c.viewport.TopLeftY),
                              static_cast<LONG>(c.viewport.TopLeftX + c.viewport.Width),
                              static_cast<LONG>(c.viewport.TopLeftY + c.viewport.Height)};
    for (UINT y = 0; y < side; ++y)
        for (UINT x = 0; x < side; ++x) {
            if (!inside(viewport, x, y) || !inside(c.scissor, x, y)) continue;
            const UINT tx = static_cast<UINT>(static_cast<std::int32_t>(x) * c.scale[0] + c.offset[0]);
            const UINT ty = static_cast<UINT>(static_cast<std::int32_t>(y) * c.scale[1] + c.offset[1]);
            if (tx >= side || ty >= side) throw std::runtime_error("composite case reads outside the source");
            for (int ch = 0; ch < 4; ++ch)
                if (c.mask & (1u << ch)) out[y * side + x][ch] = capture.source[ty * side + tx][ch];
        }
    return out;
}

UINT mismatches(const std::vector<Color>& got, const std::vector<Color>& want)
{
    UINT count = 0;
    for (std::size_t i = 0; i < got.size(); ++i) count += std::memcmp(got[i].data(), want[i].data(), sizeof(Color)) != 0;
    return count;
}

std::string text(const Color& c)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "(%g,%g,%g,%g)", c[0], c[1], c[2], c[3]);
    return buffer;
}

Color at(const std::vector<Color>& image, UINT x, UINT y) { return image[y * side + x]; }
}  // namespace

// Returns the number of failed expectations (0 = pass).
int check_startup_composite_provenance(ID3D12Device* device)
{
    const auto root = create_root(device);
    const Case cases[] = {
        {"drawn centre", full_viewport, full_scissor, {1, 1}, {0, 0}, D3D12_COLOR_WRITE_ENABLE_ALL},
        {"clipped centre", full_viewport, {0, 0, static_cast<LONG>(side), 16}, {1, 1}, {0, 0}, D3D12_COLOR_WRITE_ENABLE_ALL},
        {"uv selects corner", full_viewport, full_scissor, {0, 0}, {2, 2}, D3D12_COLOR_WRITE_ENABLE_ALL},
    };
    const Color expected_centre[] = {centre, retained, corner};
    int failures = 0;
    std::vector<Color> centres;
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const auto& c = cases[i];
        const auto capture = run(device, root.Get(), c);
        const auto want = model(capture, c);
        const UINT bad = mismatches(capture.after, want);
        const Color after_centre = at(capture.after, 32, 32);
        const bool inputs = at(capture.source, 32, 32) == centre && at(capture.source, 2, 2) == corner &&
                            at(capture.source, 60, 60) == base && at(capture.before, 32, 32) == retained;
        const bool centre_ok = after_centre == expected_centre[i];
        failures += (bad != 0) + !inputs + !centre_ok;
        centres.push_back(after_centre);
        std::printf("startup_composite_provenance %s: viewport=(%g,%g %gx%g) scissor=(%ld,%ld)-(%ld,%ld) blend=off "
                    "write_mask=0x%x uv=texel*(%d,%d)+(%d,%d)\n", c.name, c.viewport.TopLeftX, c.viewport.TopLeftY,
                    c.viewport.Width, c.viewport.Height, c.scissor.left, c.scissor.top, c.scissor.right, c.scissor.bottom,
                    c.mask, c.scale[0], c.scale[1], c.offset[0], c.offset[1]);
        std::printf("  source centre=%s corner=%s base=%s; destination before centre=%s; after centre=%s corner=%s; "
                    "mismatches=%u/%u%s\n", text(at(capture.source, 32, 32)).c_str(), text(at(capture.source, 2, 2)).c_str(),
                    text(at(capture.source, 60, 60)).c_str(), text(at(capture.before, 32, 32)).c_str(),
                    text(after_centre).c_str(), text(at(capture.after, 2, 2)).c_str(), bad, side * side,
                    bad || !inputs || !centre_ok ? "  FAIL" : "");
    }
    const bool distinct = centres.size() == 3 && centres[0] != centres[1] && centres[1] != centres[2] && centres[0] != centres[2];
    failures += !distinct;
    // Mutants run against the unchanged model of their case; each must mismatch.
    struct Mutant { const char* name; Case run_as; std::size_t model_case; };
    Case uv_moved = cases[2];
    uv_moved.offset[0] = uv_moved.offset[1] = 32;
    Case masked = cases[0];
    masked.mask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA;
    const Mutant mutants[] = {{"uv moved to the source centre", uv_moved, 2}, {"write mask red|alpha", masked, 0}};
    for (const auto& m : mutants) {
        const auto capture = run(device, root.Get(), m.run_as);
        const UINT bad = mismatches(capture.after, model(capture, cases[m.model_case]));
        failures += bad == 0;
        std::printf("startup_composite_provenance mutant %s: mismatches=%u/%u%s\n", m.name, bad, side * side,
                    bad ? "" : "  FAIL (mutant matched)");
    }
    std::printf("startup_composite_provenance cases=%zu distinct_centres=%d mutants=%zu failures=%d\n", std::size(cases),
                distinct ? 1 : 0, std::size(mutants), failures);
    return failures;
}
