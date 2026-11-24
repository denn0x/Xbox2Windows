#include "bvh_fixture.h"
#include "DualUav.h"
#include "DynamicSamplerIndex.h"
#include "SampleMip.h"
#include <algorithm>

using namespace bvh_fixture;

namespace {
constexpr Word guard = 0xe7b934c1u;

ComPtr<ID3D12Resource> uav_buffer(ID3D12Device* device, UINT64 bytes)
{
    return make_buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}

Word texel(UINT level, UINT x, UINT y)
{
    const Word checker = ((x / 2 + y / 2 + level) & 1) ? 221 : 31;
    return checker | ((x + 13 * level) << 8) | ((y + 23 * level) << 16) | 0xff000000u;
}

// Write a source-owned 4x4 Morton tile in storage order. Pad incomplete edge tiles.
std::vector<Word> tiled_mip(UINT level, UINT width, UINT height)
{
    std::vector<Word> tiles;
    for (UINT top = 0; top < height; top += 4) {
        for (UINT left = 0; left < width; left += 4) {
            for (UINT n = 0; n < 16; ++n) {
                const UINT x = left + (n & 1) + ((n >> 1) & 2);
                const UINT y = top + ((n >> 1) & 1) + ((n >> 2) & 2);
                tiles.push_back(x < width && y < height ? texel(level, x, y) : guard);
            }
        }
    }
    return tiles;
}

constexpr UINT mipCount = 4;
constexpr UINT width = 13, height = 9;
UINT extent(UINT size, UINT level) { return (std::max)(1u, size >> level); }
}

void check_dual_uav(ID3D12Device* device)
{
    constexpr UINT capacity = 192, bytes = capacity * sizeof(Word);
    Kernel kernel(device, {DualUav, sizeof(DualUav)});
    std::vector<Word> source(capacity);
    for (UINT i = 0; i < capacity; ++i) source[i] = 0x8459u * (i + 1);
    auto initial = upload(device, source);
    auto guards = upload(device, std::vector<Word>(capacity, guard));
    Word previousWitness = 0;
    for (UINT count : {0u, 1u, 63u, 64u, 65u, 129u, 128u}) {
        auto input = uav_buffer(device, bytes);
        auto output = uav_buffer(device, bytes);
        auto back = make_buffer(device, 2 * bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        ComputeCommands cmd(device, kernel.pipeline.Get());
        auto* list = cmd.list.Get();
        for (auto* buffer : {input.Get(), output.Get()})
            transition(list, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(input.Get(), initial.Get());
        list->CopyResource(output.Get(), guards.Get());
        for (auto* buffer : {input.Get(), output.Get()})
            transition(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(kernel.root.Get());
        list->SetComputeRootUnorderedAccessView(0, input->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(2, count, 0);
        list->Dispatch(capacity / 64, 1, 1);
        for (auto* buffer : {input.Get(), output.Get()})
            transition(list, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(back.Get(), 0, input.Get(), 0, bytes);
        list->CopyBufferRegion(back.Get(), bytes, output.Get(), 0, bytes);
        cmd.submit_and_wait(device);
        auto words = read(back.Get(), 2 * capacity);
        for (UINT i = 0; i < capacity; ++i) {
            equal(words[i], source[i], i, 0);
            equal(words[capacity+i], i < count ? ((source[i] + 17u) ^ i) : guard, i, 1);
        }
        if (count == 129) previousWitness = words[capacity+128];
        if (count == 128 && previousWitness == words[capacity+128])
            throw std::runtime_error("Root constant change missed witness word 128");
    }
    std::puts("dual_uav_cases=7 capacity_words=192 witness_index=128 input_and_guards=passed");
}

void check_texture_mips(ID3D12Device* device)
{
    Kernel shader(device, {SampleMip, sizeof(SampleMip)});
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.SampleDesc.Count = 1;
    desc.MipLevels = mipCount;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto texture = resource(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, mipCount> layouts{};
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&desc, 0, mipCount, 0, layouts.data(), nullptr, nullptr, &uploadBytes);
    std::vector<Word> staging((uploadBytes + 3) / 4, guard);
    std::vector<Word> expected;
    std::array<UINT,mipCount> starts{};
    for (UINT level = 0; level < mipCount; ++level) {
        const UINT w = extent(width, level), h = extent(height, level);
        auto tiles = tiled_mip(level, w, h);
        starts[level] = static_cast<UINT>(expected.size());
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const UINT tile = (y / 4) * ((w + 3) / 4) + x / 4;
                const UINT local = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2);
                staging[(layouts[level].Offset + y * layouts[level].Footprint.RowPitch) / 4 + x] =
                    tiles[16 * tile + local];
                expected.push_back(texel(level, x, y));
            }
        }
        expected.push_back(guard);
    }
    auto pixels = upload(device, staging);
    auto guards = upload(device, std::vector<Word>(expected.size(), guard));
    const UINT64 bytes = expected.size() * sizeof(Word);
    auto output = uav_buffer(device, bytes);
    auto back = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Create mip SRV heap");
    device->CreateShaderResourceView(texture.Get(), nullptr, heap->GetCPUDescriptorHandleForHeapStart());
    ComputeCommands cmd(device, shader.pipeline.Get());
    auto* list = cmd.list.Get();
    for (UINT level = 0; level < mipCount; ++level) {
        D3D12_TEXTURE_COPY_LOCATION dest{}, src{};
        dest.pResource = texture.Get();
        dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dest.SubresourceIndex = level;
        src.pResource = pixels.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[level];
        list->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
    }
    transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output.Get(), guards.Get());
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(shader.root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    for (UINT level = 0; level < mipCount; ++level) {
        UINT params[]{level, extent(width, level), extent(height, level), starts[level]};
        list->SetComputeRoot32BitConstants(2, 4, params, 0);
        list->Dispatch((params[1]+7)/8, (params[2]+7)/8, 1);
    }
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(back.Get(), output.Get());
    cmd.submit_and_wait(device);
    auto actual = read(back.Get(), expected.size());
    for (UINT i = 0; i < expected.size(); ++i) equal(actual[i], expected[i], i, 0);
    std::printf("mips=%u sampled_texels=%zu guard_words=%u\n", mipCount, expected.size()-mipCount, mipCount);
}

void check_dynamic_sampler_index(ID3D12Device* device)
{
    Kernel shader(device, {DynamicSamplerIndex, sizeof(DynamicSamplerIndex)});
    constexpr Word left = 0x3e800000u, right = 0x3f400000u; // 0.25f and 0.75f
    constexpr std::array<Word, 4> expected{left, right, left, left};
    constexpr UINT64 bytes = expected.size() * sizeof(Word);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    auto texture = resource(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
    std::vector<Word> texels((uploadBytes + sizeof(Word) - 1) / sizeof(Word));
    texels[0] = left;
    texels[1] = right;
    auto staging = upload(device, texels);
    auto output = uav_buffer(device, bytes);
    auto back = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> views, samplers;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&views)), "Create dynamic sampler SRV heap");
    device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heapDesc.NumDescriptors = 2;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&samplers)), "Create dynamic sampler heap");
    auto handle = samplers->GetCPUDescriptorHandleForHeapStart();
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    device->CreateSampler(&sampler, handle);
    handle.ptr += device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    device->CreateSampler(&sampler, handle);

    ComputeCommands cmd(device, shader.pipeline.Get());
    auto* list = cmd.list.Get();
    D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[]{views.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(shader.root.Get());
    list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(1, samplers->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    const UINT dispatches[][3]{{0, 0, 0}, {1, 0, 1}, {0, 1, 2}, {1, 1, 3}};
    for (const auto& params : dispatches) {
        list->SetComputeRoot32BitConstants(3, 3, params, 0);
        list->Dispatch(1, 1, 1);
    }
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(back.Get(), output.Get());
    cmd.submit_and_wait(device);

    const auto actual = read(back.Get(), expected.size());
    std::printf("dynamic_sampler_index normal=%08x,%08x forced_zero=%08x,%08x\n",
                actual[0], actual[1], actual[2], actual[3]);
    for (UINT i = 0; i < expected.size(); ++i) equal(actual[i], expected[i], i, 0);
}
