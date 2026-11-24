#include "desktop_gpu.h"
#include "descriptor_window_submissions_cs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT page_size = 256;
constexpr UINT logical_pages = 3;
constexpr UINT physical_pages = 4;
constexpr UINT unmapped = physical_pages;
constexpr UINT first_slot = page_size - 1;
constexpr UINT word_bytes = sizeof(std::uint32_t);
constexpr UINT input_stride = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT / word_bytes;
using Words = std::array<std::uint32_t, 4>;
constexpr Words sentinels{0xa00100ff, 0xa0010100, 0xb00200ff, 0xb0020100};

struct LogicalHeap {
    std::array<UINT, logical_pages> pages{unmapped, unmapped, unmapped};
};

UINT map_slot(LogicalHeap& heap, UINT slot, UINT& pages_used)
{
    auto& page = heap.pages.at(slot / page_size);
    if (page == unmapped) {
        if (pages_used == physical_pages) throw std::runtime_error("Descriptor paging exhausted the shared heap");
        page = pages_used++;
    }
    return page * page_size + slot % page_size;
}

UINT lookup_slot(const LogicalHeap& heap, UINT slot)
{
    const auto page = heap.pages.at(slot / page_size);
    if (page == unmapped) throw std::runtime_error("Descriptor paging lookup reached an unpublished page");
    return page * page_size + slot % page_size;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(ID3D12DescriptorHeap* heap, UINT slot, UINT stride)
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{slot} * stride;
    return handle;
}

UINT copy_across_pages(ID3D12Device* device, ID3D12DescriptorHeap* shared, ID3D12DescriptorHeap* staging,
                       LogicalHeap& logical, UINT& pages_used, UINT source, UINT stride)
{
    UINT logical_slot = first_slot;
    UINT remaining = 2;
    UINT chunks = 0;
    while (remaining) {
        const UINT chunk = (std::min)(remaining, page_size - logical_slot % page_size);
        const UINT dest = map_slot(logical, logical_slot, pages_used);
        device->CopyDescriptorsSimple(chunk, cpu_slot(shared, dest, stride), cpu_slot(staging, source, stride),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ++chunks;
        logical_slot += chunk;
        source += chunk;
        remaining -= chunk;
    }
    return chunks;
}

ComPtr<ID3D12RootSignature> create_root(ID3D12Device* device)
{
    // The existing source-built shader expects one raw SRV, one root UAV and a DWORD byte offset.
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 0, 1};
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    ComPtr<ID3DBlob> signature;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr),
          "Serialize descriptor paging root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)),
          "Create descriptor paging root signature");
    return root;
}
}

void check_descriptor_heap_paging(ID3D12Device* device, bool swapped)
{
    const auto root = create_root(device);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {descriptor_window_submissions_cs, sizeof(descriptor_window_submissions_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)), "Create descriptor paging PSO");

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = page_size * physical_pages;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> shared;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&shared));
    std::printf("descriptor_paging shared_heap_request=%u create_hresult=0x%08lx\n",
                desc.NumDescriptors, static_cast<unsigned long>(hr));
    check(hr, "Create shader-visible descriptor paging heap");

    desc.NumDescriptors = static_cast<UINT>(sentinels.size());
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> staging;
    check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&staging)), "Create CPU-only descriptor paging heap");
    const UINT stride = device->GetDescriptorHandleIncrementSize(desc.Type);

    // Raw SRV offsets must respect the D3D12 raw-buffer alignment.
    std::array<std::uint32_t, sentinels.size() * input_stride> input{};
    for (UINT i = 0; i < sentinels.size(); ++i) input[i * input_stride] = sentinels[i];
    const auto upload = make_buffer(device, sizeof(input), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const D3D12_RANGE no_access{0, 0};
    const D3D12_RANGE written{0, sizeof(input)};
    void* mapped = nullptr;
    check(upload->Map(0, &no_access, &mapped), "Map descriptor paging inputs");
    std::memcpy(mapped, input.data(), sizeof(input));
    upload->Unmap(0, &written);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (UINT i = 0; i < sentinels.size(); ++i) {
        srv.Buffer.FirstElement = i * input_stride;
        device->CreateShaderResourceView(upload.Get(), &srv, cpu_slot(staging.Get(), i, stride));
    }

    // The logical heaps expose six pages in total. Touch their first pages in A, B order so
    // the second page for each heap falls in a nonadjacent physical region during publication.
    std::array<LogicalHeap, 2> heaps;
    UINT pages_used = 0;
    for (auto& heap : heaps) map_slot(heap, first_slot, pages_used);
    const UINT initial_pages = pages_used;
    const UINT chunks_a = copy_across_pages(device, shared.Get(), staging.Get(), heaps[0], pages_used, 0, stride);
    const UINT chunks_b = copy_across_pages(device, shared.Get(), staging.Get(), heaps[1], pages_used, 2, stride);
    const std::array<UINT, 4> physical{lookup_slot(heaps[0], first_slot), lookup_slot(heaps[0], first_slot + 1),
                                      lookup_slot(heaps[1], first_slot), lookup_slot(heaps[1], first_slot + 1)};
    std::printf("descriptor_paging initial_pages=%u resident_pages=%u copy_chunks=%u,%u physical=%u,%u,%u,%u\n",
                initial_pages, pages_used, chunks_a, chunks_b, physical[0], physical[1], physical[2], physical[3]);
    if (initial_pages != 2 || pages_used != 4 || chunks_a != 2 || chunks_b != 2 ||
        physical != std::array<UINT, 4>{255, 512, 511, 768} ||
        heaps[0].pages[2] != unmapped || heaps[1].pages[2] != unmapped) {
        throw std::runtime_error("Descriptor paging did not split the logical ranges across separate pages");
    }

    const auto output = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ComputeCommands commands(device, pipeline.Get());
    auto* list = commands.list.Get();
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap* visible[]{shared.Get()};
    list->SetDescriptorHeaps(1, visible);
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    for (UINT i = 0; i < sentinels.size(); ++i) {
        const UINT logical_heap = (i / 2) ^ static_cast<UINT>(swapped);
        const UINT slot = lookup_slot(heaps[logical_heap], first_slot + i % 2);
        auto gpu = shared->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64{slot} * stride;
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRoot32BitConstant(2, i * word_bytes, 0);
        list->Dispatch(1, 1, 1);
    }
    transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    commands.submit_and_wait(device);

    Words actual{};
    const D3D12_RANGE read{0, sizeof(actual)};
    check(readback->Map(0, &read, &mapped), "Map descriptor paging readback");
    std::memcpy(actual.data(), mapped, sizeof(actual));
    readback->Unmap(0, &no_access);
    const Words expected = swapped ? Words{sentinels[2], sentinels[3], sentinels[0], sentinels[1]} : sentinels;
    std::printf("descriptor_paging swapped=%u words=%08x,%08x,%08x,%08x\n", static_cast<unsigned>(swapped),
                actual[0], actual[1], actual[2], actual[3]);
    for (UINT i = 0; i < actual.size(); ++i) {
        if (actual[i] == expected[i]) continue;
        std::fprintf(stderr, "Descriptor paging word %u expected %08x, got %08x\n", i, expected[i], actual[i]);
        throw std::runtime_error("Descriptor paging readback differs from reference");
    }
}
