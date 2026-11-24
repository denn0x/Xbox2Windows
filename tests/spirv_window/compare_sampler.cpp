// D3D12 readback for the #670 comparison-sampler fixture (tests/spirv_window/shared_sampler.spvasm).
// t0 holds depth texels, t1 ordinary texels with a second mip. s2 is a point-clamp LESS_EQUAL
// comparison sampler and the ordinary point-clamp sampler goes to the register the lowering pass
// was given. Outputs land in a raw UAV at u4; every word of it, including the 13 guards, is
// compared with the reference.
//
// Descriptor controls change one static sampler at a time so each register is shown to drive only
// its own outputs:
//   comparison-greater  s2 uses GREATER instead of LESS_EQUAL; only words 0 and 1 should change.
//   ordinary-min-lod1   the ordinary sampler clamps MinLOD to 1, selecting t1's 1x1 mip (0.625);
//                       only word 2 should change.
//
// A "multi" prefix (multi, multi-<control>) runs the two-sampler module shared_sampler_multi.spvasm: comparison
// samplers s2 and s6, each also feeding one ordinary sample, whose two lowered samplers sit at the two
// registers given as "<first>,<second>" (first for the sampler declared first, cmpA at s2). Outputs are
// word 0 SampleCmp(cmpA, texel 0.5, ref 0.25), word 1 SampleCmp(cmpB, texel 0.75, ref 0.875), word 2
// ordinary A (0.375) and word 3 ordinary B (0.875). One static sampler changes per control, each moving
// only its own word: comparison-a-greater (word 0), comparison-b-greater (word 1), ordinary-a-min-lod1
// (word 2, to 0.625) and ordinary-b-min-lod1 (word 3, to 0.625); the other 12 words are guards.
//
// An "array-" prefix (array, array-comparison-greater, array-ordinary-min-lod1) makes t0 and t1
// two-layer Texture2DArray resources for the arrayed module (shared_sampler_array.spvasm). Layer 0
// holds the texels above; layer 1 holds a different top-left texel in each texture (depth 0.875, so
// both comparisons pass; ordinary 0.3125; mip 1 0.5625), so a read of the wrong layer shows.
//
// The "gather" variant reads back the OpImageGather and OpImageDrefGather modules. Its reference file holds the
// 16 expected words followed by the case's texels as 12 floats: t0's four depth texels (rows top to bottom, left
// to right), then t1's eight R, G values in the same order. t1 is then a one-mip R32G32_FLOAT 2x2 texture, so
// the host holds no texel values of its own for the case and the Python side models the words it expects.
//
// A "volume" prefix (volume, volume-comparison-greater, volume-ordinary-min-lod1) keeps t0 a Texture2D and
// makes t1 a 2x2x2 Texture3D for the module with a 3D value image, which samples at w = 0.75 (slice 1).
// Slice 0 holds the texels above; slice 1 holds 0.3125 at its top-left texel. Mip 1 is 1x1x1 (0.625).
#include "../desktop_gpu.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT comparison_register = 2;
constexpr UINT second_comparison_register = 6;
constexpr UINT output_words = 16;
constexpr UINT guard = 0xc5c5c5c5u;
constexpr UINT gather_texel_floats = 12;

std::vector<char> read_file(const char* path, const char* what)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Cannot open ") + what + ": " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
}

void check_spirv_compare_sampler(ID3D12Device* device, const char* shader_path, const char* ordinary_register,
                                 const char* reference_path, const char* descriptors)
{
    const std::string_view variant = descriptors ? descriptors : "baseline";
    // "multi" alone is the two-sampler baseline; "multi-<v>" is control v on it.
    const bool multi = variant == "multi" || variant.substr(0, 6) == "multi-";
    std::vector<UINT> ordinary_registers;
    for (const char* cursor = ordinary_register;;) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 10);
        if (end == cursor || value > 15 || (*end && *end != ','))
            throw std::runtime_error("Ordinary sampler registers must be numbers 0-15 separated by commas");
        ordinary_registers.push_back(static_cast<UINT>(value));
        if (!*end) break;
        cursor = end + 1;
    }
    if (ordinary_registers.size() != (multi ? 2u : 1u))
        throw std::runtime_error("Ordinary sampler registers: one for a single module, two for multi");
    for (UINT i = 0; i < ordinary_registers.size(); ++i) {
        if (ordinary_registers[i] == comparison_register || (multi && ordinary_registers[i] == second_comparison_register))
            throw std::runtime_error("Ordinary sampler register must differ from the comparison samplers");
        for (UINT j = 0; j < i; ++j)
            if (ordinary_registers[i] == ordinary_registers[j])
                throw std::runtime_error("Ordinary sampler registers must be distinct");
    }
    // "array" alone is the arrayed baseline; "array-<v>" is variant v on the arrayed textures.
    const bool arrayed = variant == "array" || variant.substr(0, 6) == "array-";
    const bool volume = variant == "volume" || variant.substr(0, 7) == "volume-";
    const bool gather = variant == "gather";
    const std::string_view samplers_variant = variant == "array" || variant == "multi" || variant == "volume" || gather ? "baseline"
                                              : arrayed || multi || volume ? variant.substr(variant.find('-') + 1) : variant;
    if (multi) {
        if (samplers_variant != "baseline" && samplers_variant != "comparison-a-greater" &&
            samplers_variant != "comparison-b-greater" && samplers_variant != "ordinary-a-min-lod1" &&
            samplers_variant != "ordinary-b-min-lod1")
            throw std::runtime_error("Multi descriptor variant must be multi or multi-<comparison-a-greater, "
                                     "comparison-b-greater, ordinary-a-min-lod1 or ordinary-b-min-lod1>");
    } else if (samplers_variant != "baseline" && samplers_variant != "comparison-greater" &&
               samplers_variant != "ordinary-min-lod1") {
        throw std::runtime_error("Descriptor variant must be baseline, comparison-greater or ordinary-min-lod1, "
                                 "optionally as array, array-<variant>, volume or volume-<variant>, or a multi variant");
    }
    const UINT layers = arrayed || volume ? 2 : 1;
    const auto shader = read_file(shader_path, "comparison-sampler shader");
    const auto reference_bytes = read_file(reference_path, "comparison-sampler reference");
    if (reference_bytes.size() != output_words * sizeof(UINT) + (gather ? gather_texel_floats * sizeof(float) : 0))
        throw std::runtime_error(gather ? "Gather case file must hold 16 words and 12 texel floats"
                                        : "Comparison-sampler reference must hold 16 words");
    std::array<UINT, output_words> expected;
    std::memcpy(expected.data(), reference_bytes.data(), sizeof(expected));
    std::array<float, gather_texel_floats> case_texels{};
    if (gather) std::memcpy(case_texels.data(), reference_bytes.data() + sizeof(expected), sizeof(case_texels));

    D3D12_DESCRIPTOR_RANGE textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &textures};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor = {4, 0};
    // Per module: a comparison sampler followed by its ordinary sampler, once (single) or twice (multi).
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    const UINT sampler_count = multi ? 4 : 2;
    for (UINT i = 0; i < sampler_count; ++i) {
        auto& s = samplers[i];
        const UINT pair = i / 2;
        const bool comparison = i % 2 == 0;
        s.Filter = comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxAnisotropy = 1;
        s.ComparisonFunc = comparison ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = comparison ? (pair == 0 ? comparison_register : second_comparison_register)
                                      : ordinary_registers[pair];
    }
    if (samplers_variant == "comparison-greater" || samplers_variant == "comparison-a-greater")
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER;
    if (samplers_variant == "ordinary-min-lod1" || samplers_variant == "ordinary-a-min-lod1") samplers[1].MinLOD = 1.0f;
    if (samplers_variant == "comparison-b-greater") samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER;
    if (samplers_variant == "ordinary-b-min-lod1") samplers[3].MinLOD = 1.0f;
    D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters, sampler_count, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> root_bytes, error;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_bytes, &error),
          "Serialize comparison-sampler root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_bytes->GetBufferPointer(), root_bytes->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "Create comparison-sampler root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "Create comparison-sampler pipeline");

    // 2x2 R32_FLOAT textures. UV (0.25, 0.25) with point sampling selects the top-left texel:
    // depth 0.5 (compared with 0.25 and 0.75) and ordinary value 0.375. t1's 1x1 mip holds 0.625.
    // Layer 1 of the arrayed textures changes only those three: depth 0.875, ordinary 0.3125, mip 1 0.5625.
    constexpr UINT row_pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT texture_bytes = 2 * row_pitch;
    static_assert(texture_bytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0, "texture placement");
    // Upload layout: mip 0 of each (texture, layer), then t1's mip 1 per layer, then the output guards.
    const UINT mip1_offset = 2 * layers * texture_bytes;
    const UINT guards_offset = mip1_offset + layers * texture_bytes;
    float texels[2][2][2][2] = {
        {{{0.5f, 0.75f}, {0.25f, 1.0f}}, {{0.875f, 0.75f}, {0.25f, 1.0f}}},
        {{{0.375f, 0.875f}, {0.625f, 0.125f}}, {{0.3125f, 0.875f}, {0.625f, 0.125f}}}};
    const float mip1[2] = {0.625f, 0.5625f};
    // The gather case replaces t0's texels and gives t1 two channels (R, G) per texel, all from the case file.
    float value_rg[2][4]{};
    if (gather)
        for (UINT row = 0; row < 2; ++row) {
            for (UINT column = 0; column < 2; ++column) {
                texels[0][0][row][column] = case_texels[row * 2 + column];
                value_rg[row][column * 2] = case_texels[4 + row * 4 + column * 2];
                value_rg[row][column * 2 + 1] = case_texels[4 + row * 4 + column * 2 + 1];
            }
        }
    const auto mip0_offset = [&](UINT texture, UINT layer) { return (texture * layers + layer) * texture_bytes; };
    std::array<UINT, output_words> initial;
    initial.fill(guard);
    auto upload = make_buffer(device, guards_offset + sizeof(initial), D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = make_buffer(device, sizeof(initial), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = make_buffer(device, sizeof(initial), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RANGE empty{};
    BYTE* mapped = nullptr;
    check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "Map comparison-sampler upload");
    for (UINT i = 0; i < 2; ++i)
        for (UINT layer = 0; layer < layers; ++layer)
            for (UINT row = 0; row < 2; ++row) {
                BYTE* destination = mapped + mip0_offset(i, layer) + row * row_pitch;
                if (gather && i == 1) std::memcpy(destination, value_rg[row], sizeof(value_rg[row]));
                else std::memcpy(destination, texels[i][layer][row], sizeof(texels[i][layer][row]));
            }
    for (UINT layer = 0; layer < layers; ++layer)
        std::memcpy(mapped + mip1_offset + layer * texture_bytes, &mip1[layer], sizeof(mip1[layer]));
    std::memcpy(mapped + guards_offset, initial.data(), sizeof(initial));
    upload->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create comparison-sampler descriptors");
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);

    ComputeCommands commands(device, pipeline.Get());
    auto& list = commands.list;
    std::array<ComPtr<ID3D12Resource>, 2> inputs;
    for (UINT i = 0; i < 2; ++i) {
        D3D12_RESOURCE_DESC texture_desc{};
        const bool is_volume = volume && i == 1;
        const UINT depth = arrayed || is_volume ? layers : 1;
        texture_desc.Dimension = is_volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = texture_desc.Height = 2;
        texture_desc.DepthOrArraySize = static_cast<UINT16>(depth);
        texture_desc.MipLevels = i == 0 || gather ? 1 : 2;
        const DXGI_FORMAT format = gather && i == 1 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        texture_desc.Format = format;
        texture_desc.SampleDesc.Count = 1;
        inputs[i] = resource(device, texture_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        transition(list.Get(), inputs[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        // A 3D resource has one subresource per mip: its slices follow each other in the upload at one
        // slice pitch (2 rows), which equals texture_bytes here.
        for (UINT layer = 0; layer < (is_volume ? 1 : depth); ++layer) {
            for (UINT mip = 0; mip < texture_desc.MipLevels; ++mip) {
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = inputs[i].Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = mip + layer * texture_desc.MipLevels;
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = upload.Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                const UINT size = mip == 0 ? 2 : 1;
                from.PlacedFootprint = {mip == 0 ? mip0_offset(i, layer) : mip1_offset + layer * texture_bytes,
                                        {format, size, size, is_volume && mip == 0 ? 2u : 1u, row_pitch}};
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
        }
        transition(list.Get(), inputs[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // The default view of a two-layer resource is a Texture2DArray over both layers; of a one-layer
        // resource, a Texture2D.
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (is_volume) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D.MipLevels = texture_desc.MipLevels;
        } else if (arrayed) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = texture_desc.MipLevels;
            view.Texture2DArray.ArraySize = layers;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = texture_desc.MipLevels;
        }
        device->CreateShaderResourceView(inputs[i].Get(), &view, cpu);
        cpu.ptr += stride;
    }
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(output.Get(), 0, upload.Get(), guards_offset, sizeof(initial));
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, sizeof(initial));
    commands.submit_and_wait(device);

    D3D12_RANGE read{0, sizeof(initial)};
    check(readback->Map(0, &read, reinterpret_cast<void**>(&mapped)), "Map comparison-sampler readback");
    std::array<UINT, output_words> actual;
    std::memcpy(actual.data(), mapped, sizeof(actual));
    readback->Unmap(0, &empty);
    UINT mismatches = 0;
    for (UINT i = 0; i < output_words; ++i) {
        if (actual[i] == expected[i]) continue;
        std::fprintf(stderr, "word=%u expected=%08x actual=%08x\n", i, expected[i], actual[i]);
        ++mismatches;
    }
    std::string registers = "s" + std::to_string(comparison_register);
    if (multi) registers += ",s" + std::to_string(second_comparison_register);
    for (const UINT ordinary : ordinary_registers) registers += ",s" + std::to_string(ordinary);
    std::printf("descriptors=%.*s outputs=%08x,%08x,%08x,%08x sampler_registers=%s compared_words=%u mismatches=%u\n",
                static_cast<int>(variant.size()), variant.data(), actual[0], actual[1], actual[2], actual[3],
                registers.c_str(), output_words, mismatches);
    if (mismatches) throw std::runtime_error("Comparison-sampler readback differs from CPU reference");
}
