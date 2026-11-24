#include "desktop_gpu.h"
#include "../src/cpu_srv_upload.h"
#include "cpu_srv_refresh_cs.h"
#include "descriptor_window_submissions_cs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT descriptor_count = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
constexpr std::uint32_t first_word = 0x13579bdf;
constexpr std::uint32_t second_word = 0x2468ace0;
constexpr std::uint32_t untouched = 0xa5a5a5a5;
constexpr UINT word_bytes = sizeof(std::uint32_t);
constexpr UINT input_stride_words = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT / word_bytes;
using Words = std::array<std::uint32_t, 2>;

// This fixture owns the record format. The record bytes contain an ID, not a D3D12 descriptor.
struct BindingRecord {
    std::uint32_t id;
    std::array<std::uint32_t, 7> reserved{};
};
static_assert(sizeof(BindingRecord) == 32);
constexpr std::array<BindingRecord, 2> records{BindingRecord{0x41}, BindingRecord{0x72}};

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(ID3D12DescriptorHeap* heap, UINT index, UINT stride)
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{index} * stride;
    return handle;
}

ComPtr<ID3D12RootSignature> create_root(ID3D12Device* device)
{
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 0, 1};
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    ComPtr<ID3DBlob> signature;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr),
          "Serialize descriptor-window root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)),
          "Create descriptor-window root signature");
    return root;
}

void compare_words(UINT64 fence_value, const Words& actual, const Words& expected)
{
    std::printf("descriptor_windows fence=%llu words=%08x,%08x expected=%08x,%08x\n",
                fence_value, actual[0], actual[1], expected[0], expected[1]);
    if (actual != expected) throw std::runtime_error("Descriptor-window readback differs from reference");
}

Words read_words(ID3D12Resource* readback)
{
    Words actual{};
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, sizeof(actual)};
    const D3D12_RANGE no_write{0, 0};
    check(readback->Map(0, &read_range, &mapped), "Map descriptor-window readback");
    std::memcpy(actual.data(), mapped, sizeof(actual));
    readback->Unmap(0, &no_write);
    return actual;
}
}

void check_descriptor_window_submissions(ID3D12Device* device, DescriptorWindowCase scenario)
{
    const bool stale_window = scenario == DescriptorWindowCase::stale_window;
    const bool stale_record = scenario == DescriptorWindowCase::stale_record;
    const bool use_records = scenario != DescriptorWindowCase::direct && !stale_window;
    const auto root = create_root(device);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {descriptor_window_submissions_cs, sizeof(descriptor_window_submissions_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)),
          "Create descriptor-window compute pipeline");

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = descriptor_count;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    const HRESULT heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    std::printf("descriptor_windows heap_request=%u create_hresult=0x%08lx\n", descriptor_count,
                static_cast<unsigned long>(heap_hr));
    check(heap_hr, "Create shader-visible descriptor-window heap");
    const UINT stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);

    constexpr UINT guard_index = input_stride_words + 1;
    std::array<std::uint32_t, guard_index + 2> initial{};
    initial[0] = first_word;
    initial[input_stride_words] = second_word;
    initial[guard_index] = initial[guard_index + 1] = untouched;
    const auto upload = make_buffer(device, sizeof(initial), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(upload->Map(0, &no_read, &mapped), "Map descriptor-window inputs");
    std::memcpy(mapped, initial.data(), sizeof(initial));
    const D3D12_RANGE input_written{0, sizeof(initial)};
    upload->Unmap(0, &input_written);
    const auto output = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    constexpr std::array<UINT, 2> windows{0, descriptor_count - 1};
    ComPtr<ID3D12DescriptorHeap> staging;
    if (use_records) {
        D3D12_DESCRIPTOR_HEAP_DESC staging_desc{};
        staging_desc.Type = heap_desc.Type;
        staging_desc.NumDescriptors = static_cast<UINT>(records.size());
        check(device->CreateDescriptorHeap(&staging_desc, IID_PPV_ARGS(&staging)), "Create CPU descriptor staging heap");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (UINT i = 0; i < windows.size(); ++i) {
        srv.Buffer.FirstElement = i * input_stride_words;
        const auto dst = use_records ? cpu_slot(staging.Get(), i, stride) : cpu_slot(heap.Get(), windows[i], stride);
        device->CreateShaderResourceView(upload.Get(), &srv, dst);
    }

    unsigned descriptor_copies = 0;
    auto materialize = [&](const BindingRecord& record, UINT dst) {
        for (UINT i = 0; i < records.size(); ++i) {
            if (record.id != records[i].id) continue;
            device->CopyDescriptorsSimple(1, cpu_slot(heap.Get(), dst, stride), cpu_slot(staging.Get(), i, stride),
                                          heap_desc.Type);
            ++descriptor_copies;
            std::printf("descriptor_records materialize slot=%u id=%08x copies=%u\n", dst, record.id, descriptor_copies);
            return true;
        }
        return false;
    };
    BindingRecord live_second = records[0];
    const BindingRecord cached_second = live_second;
    if (use_records && (!materialize(records[0], windows[0]) || !materialize(live_second, windows[1]))) {
        throw std::runtime_error("Failed to seed descriptor records");
    }

    ComputeCommands commands(device, pipeline.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create descriptor-window fence");
    auto* list = commands.list.Get();
    transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(output.Get(), 0, upload.Get(), guard_index * word_bytes, sizeof(Words));
    const auto second_expected = stale_window || stale_record ? first_word : second_word;
    const std::array<Words, 2> expected{Words{first_word, untouched}, Words{first_word, second_expected}};
    unsigned submissions = 0;
    auto before = D3D12_RESOURCE_STATE_COPY_DEST;
    for (UINT i = 0; i < windows.size(); ++i) {
        if (i != 0) {
            if (use_records) {
                if (scenario == DescriptorWindowCase::unknown_record) {
                    const BindingRecord unknown{0xff};
                    std::memcpy(&live_second, &unknown, sizeof(live_second));
                    if (materialize(live_second, windows[1]) || descriptor_copies != 2 ||
                        submissions != 1 || fence->GetCompletedValue() != 1 || read_words(readback.Get()) != expected[0]) {
                        throw std::runtime_error("Unknown descriptor record changed state before retry");
                    }
                    std::puts("descriptor_records rejected id=000000ff copies=2 submissions=1 fence=1 readback=preserved");
                }
                std::memcpy(&live_second, &records[1], sizeof(live_second));
                std::printf("descriptor_records copied id=%08x\n", live_second.id);
                if (live_second.id != records[1].id ||
                    !materialize(stale_record ? cached_second : live_second, windows[1]) || descriptor_copies != 3) {
                    throw std::runtime_error("Failed to materialize copied descriptor record");
                }
            }
            check(commands.allocator->Reset(), "Reset descriptor-window allocator");
            check(list->Reset(commands.allocator.Get(), pipeline.Get()), "Reset descriptor-window list");
            before = D3D12_RESOURCE_STATE_COMMON;
        }
        transition(list, output.Get(), before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const UINT slot = windows[stale_window ? 0 : i];
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64{slot} * stride;
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(2, i * word_bytes, 0);
        list->Dispatch(1, 1, 1);
        transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(readback.Get(), output.Get());
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        const UINT64 fence_value = UINT64{i} + 1;
        std::printf("descriptor_windows submit=%llu slot=%u\n", fence_value, slot);
        check(list->Close(), "Close descriptor-window list");
        ID3D12CommandList* submitted[]{list};
        commands.queue->ExecuteCommandLists(1, submitted);
        ++submissions;
        check(commands.queue->Signal(fence.Get(), fence_value), "Signal descriptor-window fence");
        check(fence->SetEventOnCompletion(fence_value, nullptr), "Wait for descriptor-window fence");
        check(device->GetDeviceRemovedReason(), "Complete descriptor-window submission");
        if (fence->GetCompletedValue() != fence_value) throw std::runtime_error("Unexpected descriptor-window fence value");

        compare_words(fence_value, read_words(readback.Get()), expected[i]);
    }
}

void check_cpu_srv_refresh(ID3D12Device* device, bool stale)
{
    const auto root = create_root(device);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.CS = {cpu_srv_refresh_cs, sizeof(cpu_srv_refresh_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create CPU-SRV pipeline");
    ComputeCommands commands(device, pipeline.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    ComPtr<ID3D12Fence> retire;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retire)), "Create CPU-SRV fence");
    constexpr UINT first = 16;
    constexpr UINT bytes = first + 2 * sizeof(UINT16);
    const auto upload = make_buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto output = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto readback = make_buffer(device, sizeof(Words), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create CPU-SRV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Format = DXGI_FORMAT_R16_UINT;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = first / sizeof(UINT16);
    srv.Buffer.NumElements = 2;
    device->CreateShaderResourceView(upload.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());

    constexpr std::array<UINT16, 2> original{0x1398, 0x2467}, replacement{0x8765, 0xfe01};
    auto source = original;
    check(cpu_srv::refresh(upload.Get(), first, source.data(), sizeof(source), nullptr, 0), "Refresh initial SRV");
    for (UINT i = 0; i < 2; ++i) {
        auto* list = commands.list.Get();
        if (i == 1) {
            source = replacement;
            if (cpu_srv::refresh(upload.Get(), first, source.data(), sizeof(source), retire.Get(), 2) !=
                    DXGI_ERROR_WAS_STILL_DRAWING ||
                cpu_srv::refresh(upload.Get(), first + 1, source.data(), sizeof(source), retire.Get(), 1) != E_INVALIDARG ||
                cpu_srv::refresh(upload.Get(), SIZE_MAX, source.data(), sizeof(source), retire.Get(), 1) != E_INVALIDARG)
                throw std::runtime_error("CPU-SRV accepted an outstanding fence or an out-of-range refresh");
            auto* inaccessible = VirtualAlloc(nullptr, 1, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
            if (!inaccessible) throw std::runtime_error("Cannot allocate unreadable CPU-SRV control");
            const HRESULT rejected = cpu_srv::refresh(upload.Get(), first, inaccessible, sizeof(source), retire.Get(), 1);
            VirtualFree(inaccessible, 0, MEM_RELEASE);
            if (SUCCEEDED(rejected)) throw std::runtime_error("CPU-SRV accepted unreadable input");
            if (!stale)
                check(cpu_srv::refresh(upload.Get(), first, source.data(), sizeof(source), retire.Get(), 1),
                      "Refresh SRV after fence");
            check(commands.allocator->Reset(), "Reset CPU-SRV allocator");
            check(list->Reset(commands.allocator.Get(), pipeline.Get()), "Reset CPU-SRV list");
        }
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(2, 0, 0);
        list->Dispatch(1, 1, 1);
        transition(list, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(readback.Get(), output.Get());
        transition(list, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        check(list->Close(), "Close CPU-SRV list");
        ID3D12CommandList* batch[]{list};
        commands.queue->ExecuteCommandLists(1, batch);
        check(commands.queue->Signal(retire.Get(), i + 1), "Signal CPU-SRV fence");
        check(retire->SetEventOnCompletion(i + 1, nullptr), "Wait for CPU-SRV fence");
        check(device->GetDeviceRemovedReason(), "Complete CPU-SRV read");
        const auto expected = (i == 1 && !stale) ? replacement : original;
        compare_words(i + 1, read_words(readback.Get()), {expected[0], expected[1]});
    }
}
