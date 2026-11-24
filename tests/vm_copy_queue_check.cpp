#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "xmem_test_device.h"
#include "raw_buffer_srv.h"
#include "raw_buffer_uav.h"
#include "vm_raw_srv_cs.h"
#include "vm_raw_uav_cs.h"

using Microsoft::WRL::ComPtr;
using namespace std::literals;

namespace {
constexpr UINT64 tile_bytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
constexpr UINT words = static_cast<UINT>(tile_bytes / sizeof(std::uint32_t));

bool check(HRESULT result, const char* stage)
{
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "%s failed: 0x%08lx\n", stage, static_cast<unsigned long>(result));
    return false;
}

D3D12_RESOURCE_DESC buffer_desc()
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 2 * tile_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

std::uint32_t pattern(UINT tile, UINT word)
{
    return (tile ? 0x7a81fc39u : 0x42b167d3u) ^ (word * 0x9e3779b1u);
}

struct Queue {
    ComPtr<ID3D12CommandQueue> commands;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> fence;

    bool init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        return check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&commands)), "CreateCommandQueue") &&
               check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator") &&
               check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    }

    void execute(ID3D12GraphicsCommandList* list)
    {
        ID3D12CommandList* lists[]{list};
        commands->ExecuteCommandLists(1, lists);
    }
};

bool record_copy(ID3D12Device* device, const Queue& queue, ID3D12Resource* dst, UINT64 dst_offset,
                 ID3D12Resource* src, UINT64 size, ComPtr<ID3D12GraphicsCommandList>& list)
{
    if (!check(device->CreateCommandList(0, queue.commands->GetDesc().Type, queue.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)), "CreateCommandList")) {
        return false;
    }
    // Buffers decay to COMMON between ExecuteCommandLists calls. Both engines
    // may promote this reserved buffer to their own copy state.
    list->CopyBufferRegion(dst, dst_offset, src, 0, size);
    return check(list->Close(), "Close command list");
}

bool wait_for_read(ID3D12Fence* fence, UINT64 value)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool done = check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion") &&
                      WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

int run(bool warp)
{
    bool unmatched = false;
    const auto device = create_device(warp, unmatched);
    if (unmatched) return 1;
    if (!device) return 77;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (!check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
               "Check tiled resource support")) return 1;
    if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
        std::puts("Tiled resources are unavailable");
        return 77;
    }

    Queue copy, direct;
    ComPtr<ID3D12Resource> window, upload, readback;
    ComPtr<ID3D12Heap> pool;
    const auto desc = buffer_desc();
    D3D12_HEAP_PROPERTIES upload_heap{}, readback_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    upload_heap.CreationNodeMask = upload_heap.VisibleNodeMask = 1;
    readback_heap.CreationNodeMask = readback_heap.VisibleNodeMask = 1;
    D3D12_HEAP_DESC pool_desc{};
    pool_desc.SizeInBytes = 2 * tile_bytes;
    pool_desc.Properties = upload_heap;
    pool_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    pool_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (!copy.init(device.Get(), D3D12_COMMAND_LIST_TYPE_COPY) ||
        !direct.init(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) ||
        !check(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&window)), "CreateReservedResource") ||
        !check(device->CreateHeap(&pool_desc, IID_PPV_ARGS(&pool)), "CreateHeap") ||
        !check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)), "Create upload buffer") ||
        !check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)), "Create readback buffer")) return 1;

    const D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    if (!check(upload->Map(0, &no_read, &mapped), "Map upload buffer")) return 1;
    auto* data = static_cast<std::uint32_t*>(mapped);
    for (UINT tile = 0; tile < 2; ++tile) {
        for (UINT word = 0; word < words; ++word) data[tile * words + word] = pattern(tile, word);
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12GraphicsCommandList> write, read_before, read_after;
    if (!record_copy(device.Get(), copy, window.Get(), 0, upload.Get(), 2 * tile_bytes, write) ||
        !record_copy(device.Get(), direct, readback.Get(), 0, window.Get(), tile_bytes, read_before) ||
        !record_copy(device.Get(), direct, readback.Get(), tile_bytes, window.Get(), tile_bytes, read_after)) return 1;

    const D3D12_TILED_RESOURCE_COORDINATE start{};
    const D3D12_TILE_REGION_SIZE full{2, FALSE, 0, 0, 0};
    const UINT initial_heap_offset = 0;
    copy.commands->UpdateTileMappings(window.Get(), 1, &start, &full, pool.Get(), 1, nullptr,
                                      &initial_heap_offset, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
    copy.execute(write.Get());
    if (!check(copy.commands->Signal(copy.fence.Get(), 1), "Signal initial upload") ||
        !check(direct.commands->Wait(copy.fence.Get(), 1), "Wait on DIRECT for initial upload")) return 1;
    direct.execute(read_before.Get());
    // The early reader must see tile 0. Ordering it before the remap makes this
    // a negative control without an unsynchronized read or a timing assumption.
    if (!check(direct.commands->Signal(direct.fence.Get(), 1), "Signal early read") ||
        !check(copy.commands->Wait(direct.fence.Get(), 1), "Wait on COPY for early read")) return 1;

    // Move the initialized physical tile 1 within the same reserved buffer.
    // The second range removes its old mapping, so there is no concurrent alias.
    const D3D12_TILE_RANGE_FLAGS ranges[]{D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NULL};
    const UINT offsets[]{1, 0}, counts[]{1, 1};
    copy.commands->UpdateTileMappings(window.Get(), 1, &start, &full, pool.Get(), 2, ranges, offsets, counts,
                                      D3D12_TILE_MAPPING_FLAG_NONE);
    if (!check(copy.commands->Signal(copy.fence.Get(), 2), "Signal remap") ||
        !check(direct.commands->Wait(copy.fence.Get(), 2), "Wait on DIRECT for remap")) return 1;
    direct.execute(read_after.Get());
    if (!check(direct.commands->Signal(direct.fence.Get(), 2), "Signal final read") ||
        !wait_for_read(direct.fence.Get(), 2)) {
        std::fputs("DIRECT did not complete the ordered read\n", stderr);
        return 1;
    }

    const D3D12_RANGE read_range{0, 2 * tile_bytes};
    if (!check(readback->Map(0, &read_range, &mapped), "Map readback buffer")) return 1;
    const auto* samples = static_cast<const std::uint32_t*>(mapped);
    UINT bad_before = 0, bad_after = 0, missed_new_before_remap = 0;
    for (UINT word = 0; word < words; ++word) {
        bad_before += samples[word] != pattern(0, word);
        bad_after += samples[words + word] != pattern(1, word);
        missed_new_before_remap += samples[word] != pattern(1, word);
    }
    readback->Unmap(0, &no_read);
    std::printf("copy queue early mismatches=%u ordered mismatches=%u early vs remapped=%u/%u\n",
                bad_before, bad_after, missed_new_before_remap, words);
    return bad_before == 0 && bad_after == 0 && missed_new_before_remap == words &&
           check(device->GetDeviceRemovedReason(), "Device removal status") ? 0 : 1;
}

struct SectionPage {
    HANDLE section = nullptr;
    std::uint32_t* canonical = nullptr;
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> staging;

    ~SectionPage()
    {
        staging.Reset();
        heap.Reset();
        if (canonical) UnmapViewOfFile(canonical);
        if (section) CloseHandle(section);
    }

    HRESULT init(ID3D12Device3* device)
    {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(tile_bytes), nullptr);
        if (!section) return HRESULT_FROM_WIN32(GetLastError());
        canonical = static_cast<std::uint32_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, tile_bytes));
        if (!canonical) return HRESULT_FROM_WIN32(GetLastError());
        const HRESULT result = device->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap));
        if (FAILED(result)) return result;
        auto desc = buffer_desc();
        desc.Width = tile_bytes;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        return device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&staging));
    }
};

// Own the adjacent CPU views. Their addresses are in logical order while the
// section handles used to create them are in the opposite order.
struct SectionWindow {
    HANDLE process = GetCurrentProcess();
    std::uint8_t* base = nullptr;
    bool split = false;
    unsigned mapped = 0;

    bool init(const std::array<SectionPage*, 2>& page_order)
    {
        base = static_cast<std::uint8_t*>(VirtualAlloc2(process, nullptr, 2 * tile_bytes,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base) return false;
        split = VirtualFree(base, tile_bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
        if (!split) return false;
        for (auto* page : page_order) {
            auto* address = base + mapped * tile_bytes;
            if (MapViewOfFile3(page->section, process, address, 0, tile_bytes, MEM_REPLACE_PLACEHOLDER,
                               PAGE_READWRITE, nullptr, 0) != address) return false;
            ++mapped;
        }
        return true;
    }

    ~SectionWindow()
    {
        for (unsigned i = 0; i < mapped; ++i) {
            UnmapViewOfFile2(process, base + i * tile_bytes, MEM_PRESERVE_PLACEHOLDER);
        }
        if (base) VirtualFree(base, 0, MEM_RELEASE);
        if (split) VirtualFree(base + tile_bytes, 0, MEM_RELEASE);
    }
};

int raw_srv(bool warp)
{
    constexpr UINT test_words = 1024;
    constexpr UINT64 bytes = test_words * sizeof(UINT32);
    constexpr UINT64 begin = tile_bytes - bytes / 2;
    constexpr UINT shift = 4;
    bool unmatched = false;
    const auto device = create_device(warp, unmatched);
    if (unmatched) return 1;
    if (!device) return 77;

    Queue direct;
    if (!direct.init(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT)) return 1;
    const auto create_buffer = [&](D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state, ID3D12Resource** result) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        auto desc = buffer_desc();
        desc.Width = size;
        desc.Flags = flags;
        return check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
            nullptr, IID_PPV_ARGS(result)), "Create raw SRV fixture buffer");
    };
    ComPtr<ID3D12Resource> input, output, readback;
    if (!create_buffer(D3D12_HEAP_TYPE_UPLOAD, 2 * tile_bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ, input.GetAddressOf()) ||
        !create_buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output.GetAddressOf()) ||
        !create_buffer(D3D12_HEAP_TYPE_READBACK, 2 * bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, readback.GetAddressOf())) return 1;
    const D3D12_RANGE empty{};
    void* mapped = nullptr;
    if (!check(input->Map(0, &empty, &mapped), "Map SRV input")) return 1;
    const auto sample = [](UINT word) { return pattern(word / words, word % words); };
    for (UINT i = 0; i < 2 * words; ++i) static_cast<UINT32*>(mapped)[i] = sample(i);
    input->Unmap(0, nullptr);

    const UINT64 gpu = input->GetGPUVirtualAddress();
    D3D12_SHADER_RESOURCE_VIEW_DESC correct{}, displaced{}, rejected{};
    if (!check(make_raw_buffer_srv(gpu, 2 * tile_bytes, gpu + begin - 16, shift, test_words, correct),
             "Resolve cross-page view") ||
        !check(make_raw_buffer_srv(gpu, 2 * tile_bytes, gpu + begin, shift, test_words, displaced),
             "Resolve displaced view") ||
        make_raw_buffer_srv(gpu, 2 * tile_bytes, gpu + begin + 4, 0, test_words, rejected) != E_INVALIDARG ||
        make_raw_buffer_srv(gpu, 2 * tile_bytes, gpu + 2 * tile_bytes, 0, 1, rejected) != E_INVALIDARG ||
        make_raw_buffer_srv(gpu, 2 * tile_bytes, gpu + 2 * tile_bytes - 16, 0, 5, rejected) != E_INVALIDARG)
        return 1;

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    if (!check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create SRV heap")) return 1;
    const UINT stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(input.Get(), &correct, cpu);
    cpu.ptr += stride;
    device->CreateShaderResourceView(input.Get(), &displaced, cpu);

    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    const D3D12_ROOT_SIGNATURE_DESC root_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> root;
    if (!check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr),
            "Serialize SRV root signature") ||
        !check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&root)), "Create SRV root signature")) return 1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {vm_raw_srv_cs, sizeof(vm_raw_srv_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)), "Create SRV pipeline") ||
        !check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct.allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list)), "Create SRV command list")) return 1;
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    for (UINT i = 0; i < 2; ++i) {
        list->SetComputeRootDescriptorTable(0, table);
        list->Dispatch(test_words / 64, 1, 1);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback.Get(), i * bytes, output.Get(), 0, bytes);
        auto restore = barrier;
        restore.Transition.StateBefore = barrier.Transition.StateAfter;
        restore.Transition.StateAfter = barrier.Transition.StateBefore;
        list->ResourceBarrier(1, &restore);
        table.ptr += stride;
    }
    if (!check(list->Close(), "Close SRV command list")) return 1;
    direct.execute(list.Get());
    if (!check(direct.commands->Signal(direct.fence.Get(), 1), "Signal SRV command completion") ||
        !wait_for_read(direct.fence.Get(), 1)) return 1;
    const D3D12_RANGE read_range{0, 2 * bytes};
    if (!check(readback->Map(0, &read_range, &mapped), "Map SRV readback")) return 1;
    const auto* actual = static_cast<const UINT32*>(mapped);
    UINT mismatches = 0, shifted_mismatches = 0, wrong_slot_mismatches = 0;
    for (UINT i = 0; i < test_words; ++i) {
        const UINT word = static_cast<UINT>(begin / sizeof(UINT32)) + i;
        mismatches += actual[i] != sample(word);
        shifted_mismatches += actual[test_words + i] != sample(word + shift);
        wrong_slot_mismatches += actual[test_words + i] != sample(word);
    }
    readback->Unmap(0, &empty);
    std::printf("SRV cross-page mismatches=%u shifted mismatches=%u negative=%u/%u\n",
        mismatches, shifted_mismatches, wrong_slot_mismatches, test_words);
    return mismatches == 0 && shifted_mismatches == 0 && wrong_slot_mismatches == test_words &&
        check(device->GetDeviceRemovedReason(), "Device removal status") ? 0 : 1;
}

int raw_uav(bool warp)
{
    // Two backing buffers with distinct sentinels. Each logical window maps a
    // 2040-byte range that starts inside its buffer, so the views' first
    // elements differ and the last 8 bytes of each window cannot hold a vector.
    constexpr UINT range_words = 1024;
    constexpr UINT64 range_bytes = range_words * sizeof(UINT32);
    constexpr UINT64 window_bytes = 2040;
    const RawUavWindow windows[2]{
        {0x100000000ull, window_bytes, 256, range_bytes},
        {0x200000000ull, window_bytes, 512, range_bytes},
    };
    struct Request {
        UINT64 address;
        UINT bytes;
        std::array<UINT32, 4> value;
    };
    constexpr UINT scalar_offset = window_bytes - 4;
    constexpr UINT vector_offset = window_bytes - 24;
    const Request scalar{windows[0].logical_base + scalar_offset, 4, {0xc0ffee01u, 0, 0, 0}};
    const Request vector{windows[1].logical_base + vector_offset, 16,
                         {0x0badf00du, 0x13579bdfu, 0x2468ace0u, 0xfeedc0deu}};
    const Request refused[]{
        {windows[1].logical_base + window_bytes - 8, 16, {}},  // aligned, but 8 bytes past the window
        {windows[0].logical_base + window_bytes, 4, {}},       // first byte after the window
        {windows[0].logical_base + 8, 16, {}},                 // 4-byte aligned vector
        {windows[1].logical_base + 2, 4, {}},                  // misaligned scalar
        {0x180000000ull, 4, {}},                               // between the windows
        {windows[1].logical_base - 4, 4, {}},                  // just before a window
        {windows[0].logical_base, 8, {}},                      // undeclared store width
    };
    for (const Request& request : refused) {
        RawUavStore unused{};
        if (resolve_raw_uav_store(windows, 2, request.address, request.bytes, unused) != E_INVALIDARG) {
            std::fprintf(stderr, "accepted a %u-byte store at 0x%llx\n", request.bytes,
                         static_cast<unsigned long long>(request.address));
            return 1;
        }
    }
    constexpr UINT64 four_gib = 1ull << 32;
    const RawUavWindow wide{0, four_gib + 16, 0, four_gib + 16};
    const RawUavWindow unaligned{2, 16, 0, 16};
    const RawUavWindow wrapping{UINT64_MAX - 15, 32, 0, 32};
    const RawUavWindow overlapping[]{windows[0], {windows[0].logical_base + 16, 32, 0, 32}};
    struct WindowControl {
        const RawUavWindow* set;
        std::size_t count;
        UINT64 address;
    };
    const WindowControl window_refusals[]{
        {&wide, 1, four_gib},
        {&unaligned, 1, 4},
        {&wrapping, 1, wrapping.logical_base},
        {overlapping, std::size(overlapping), overlapping[1].logical_base},
    };
    for (const auto& control : window_refusals) {
        RawUavStore unused{17, 0xcafe, {}};
        if (resolve_raw_uav_store(control.set, control.count, control.address, 4, unused) != E_INVALIDARG ||
            unused.window != 17 || unused.offset != 0xcafe) {
            std::fprintf(stderr, "invalid raw UAV window changed the result at 0x%llx\n",
                         static_cast<unsigned long long>(control.address));
            return 1;
        }
    }
    RawUavStore scalar_store{}, vector_store{};
    if (!check(resolve_raw_uav_store(windows, 2, scalar.address, scalar.bytes, scalar_store), "Resolve scalar store") ||
        !check(resolve_raw_uav_store(windows, 2, vector.address, vector.bytes, vector_store), "Resolve vector store") ||
        scalar_store.window != 0 || vector_store.window != 1) return 1;

    bool unmatched = false;
    const auto device = create_device(warp, unmatched);
    if (unmatched) return 1;
    if (!device) return 77;
    Queue direct;
    if (!direct.init(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT)) return 1;
    const auto create_buffer = [&](D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state, ID3D12Resource** result) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        auto desc = buffer_desc();
        desc.Width = size;
        desc.Flags = flags;
        return check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
            nullptr, IID_PPV_ARGS(result)), "Create raw UAV fixture buffer");
    };
    std::array<ComPtr<ID3D12Resource>, 2> backing;
    ComPtr<ID3D12Resource> upload, readback;
    for (auto& buffer : backing) {
        if (!create_buffer(D3D12_HEAP_TYPE_DEFAULT, range_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST, buffer.GetAddressOf())) return 1;
    }
    // Phase 0 binds each window to its own buffer; phase 1 swaps the bindings.
    if (!create_buffer(D3D12_HEAP_TYPE_UPLOAD, 2 * range_bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ, upload.GetAddressOf()) ||
        !create_buffer(D3D12_HEAP_TYPE_READBACK, 4 * range_bytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, readback.GetAddressOf())) return 1;
    const auto sentinel = [](UINT range, UINT word) { return pattern(range, word) ^ 0x5bd1e995u; };
    const D3D12_RANGE empty{};
    void* mapped = nullptr;
    if (!check(upload->Map(0, &empty, &mapped), "Map UAV sentinels")) return 1;
    for (UINT range = 0; range < 2; ++range) {
        for (UINT word = 0; word < range_words; ++word)
            static_cast<UINT32*>(mapped)[range * range_words + word] = sentinel(range, word);
    }
    upload->Unmap(0, nullptr);

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> heap;
    if (!check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create UAV heap")) return 1;
    const UINT stride = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i) {
        RawUavStore view{};
        if (!check(resolve_raw_uav_store(windows, 2, windows[i].logical_base, 4, view), "Resolve window view"))
            return 1;
        device->CreateUnorderedAccessView(backing[i].Get(), nullptr, &view.view, cpu);
        cpu.ptr += stride;
    }

    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 8};
    const D3D12_ROOT_SIGNATURE_DESC root_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> root;
    if (!check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr),
            "Serialize UAV root signature") ||
        !check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&root)), "Create UAV root signature")) return 1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {vm_raw_uav_cs, sizeof(vm_raw_uav_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)), "Create UAV pipeline") ||
        !check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct.allocator.Get(), pipeline.Get(),
            IID_PPV_ARGS(&list)), "Create UAV command list")) return 1;

    const auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Transition = {backing[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        }
        list->ResourceBarrier(2, barriers);
    };
    const auto table = heap->GetGPUDescriptorHandleForHeapStart();
    const auto dispatch = [&](const Request& request, const RawUavStore& store, UINT binding) {
        const UINT32 constants[8]{store.offset, request.bytes / 4u, 0, 0,
                                  request.value[0], request.value[1], request.value[2], request.value[3]};
        list->SetComputeRootDescriptorTable(0, {table.ptr + binding * stride});
        list->SetComputeRoot32BitConstants(1, 8, constants, 0);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &uav);
    };
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    for (UINT phase = 0; phase < 2; ++phase) {
        for (UINT i = 0; i < 2; ++i)
            list->CopyBufferRegion(backing[i].Get(), 0, upload.Get(), i * range_bytes, range_bytes);
        transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(scalar, scalar_store, static_cast<UINT>(scalar_store.window) ^ phase);
        dispatch(vector, vector_store, static_cast<UINT>(vector_store.window) ^ phase);
        transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT i = 0; i < 2; ++i)
            list->CopyBufferRegion(readback.Get(), (2 * phase + i) * range_bytes, backing[i].Get(), 0, range_bytes);
        transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (!check(list->Close(), "Close UAV command list")) return 1;
    direct.execute(list.Get());
    if (!check(direct.commands->Signal(direct.fence.Get(), 1), "Signal UAV command completion") ||
        !wait_for_read(direct.fence.Get(), 1)) return 1;

    // Sentinels everywhere except the words each store reaches through a binding.
    using Ranges = std::array<std::array<UINT32, range_words>, 2>;
    const auto fresh = [&] {
        Ranges ranges{};
        for (UINT r = 0; r < 2; ++r) {
            for (UINT word = 0; word < range_words; ++word) ranges[r][word] = sentinel(r, word);
        }
        return ranges;
    };
    const auto place = [&](Ranges& ranges, const Request& request, UINT expected_offset, UINT binding) {
        const UINT first = static_cast<UINT>(windows[binding].resource_offset / sizeof(UINT32)) + expected_offset / 4u;
        for (UINT lane = 0; lane < request.bytes / 4u; ++lane) ranges[binding][first + lane] = request.value[lane];
    };
    const D3D12_RANGE read_range{0, 4 * range_bytes};
    if (!check(readback->Map(0, &read_range, &mapped), "Map UAV readback")) return 1;
    const auto* actual = static_cast<const UINT32*>(mapped);
    UINT mismatches[2]{}, changed[2]{};
    for (UINT phase = 0; phase < 2; ++phase) {
        Ranges expected = fresh();
        place(expected, scalar, scalar_offset, phase);
        place(expected, vector, vector_offset, 1u ^ phase);
        for (UINT r = 0; r < 2; ++r) {
            for (UINT word = 0; word < range_words; ++word) {
                const UINT32 value = actual[(2 * phase + r) * range_words + word];
                mismatches[phase] += value != expected[r][word];
                changed[phase] += value != sentinel(r, word);
            }
        }
    }
    // With swapped bindings, the words each store was meant for keep their sentinels.
    Ranges intended = fresh();
    place(intended, scalar, scalar_offset, 0);
    place(intended, vector, vector_offset, 1);
    UINT intended_words = 0, intended_kept = 0;
    for (UINT r = 0; r < 2; ++r) {
        for (UINT word = 0; word < range_words; ++word) {
            if (intended[r][word] == sentinel(r, word)) continue;
            ++intended_words;
            intended_kept += actual[(2 + r) * range_words + word] == sentinel(r, word);
        }
    }
    readback->Unmap(0, &empty);
    std::printf("UAV windows: bound mismatches=%u changed=%u/%u; swapped mismatches=%u changed=%u, "
                "intended words kept=%u/%u; refused=%zu\n",
        mismatches[0], changed[0], 2 * range_words, mismatches[1], changed[1], intended_kept, intended_words,
        std::size(refused) + std::size(window_refusals));
    return mismatches[0] == 0 && changed[0] == 5 && mismatches[1] == 0 && changed[1] == 5 &&
        intended_words == 5 && intended_kept == intended_words &&
        check(device->GetDeviceRemovedReason(), "Device removal status") ? 0 : 1;
}

int provenance(bool warp)
{
    constexpr UINT span_words = 1024;
    constexpr UINT half_words = span_words / 2;
    constexpr UINT64 span_bytes = span_words * sizeof(std::uint32_t);
    constexpr UINT64 half_bytes = span_bytes / 2;
    constexpr UINT64 logical_offset = tile_bytes - half_bytes;
    constexpr std::uint32_t changed = 0xa5a5a5a5u;
    bool unmatched = false;
    const auto base = create_device(warp, unmatched);
    if (unmatched) return 1;
    ComPtr<ID3D12Device3> device;
    if (!base || FAILED(base.As(&device))) return 77;

    std::array<SectionPage, 2> pages;
    for (unsigned token = 0; token < pages.size(); ++token) {
        const HRESULT result = pages[token].init(device.Get());
        if (result == E_NOTIMPL || result == DXGI_ERROR_UNSUPPORTED) {
            std::puts("Section-backed D3D12 buffers are unavailable");
            return 77;
        }
        if (!check(result, "Create a D3D12 buffer over a CPU section")) return 1;
        for (UINT word = 0; word < words; ++word) pages[token].canonical[word] = pattern(token, word);
    }
    const std::array<SectionPage*, 2> mapped_pages{&pages[1], &pages[0]};
    SectionWindow window;
    if (!window.init(mapped_pages)) {
        std::fprintf(stderr, "Map permuted CPU pages failed: %lu\n", GetLastError());
        return 1;
    }
    auto* source = reinterpret_cast<volatile std::uint32_t*>(window.base + logical_offset);
    std::array<std::uint32_t, span_words> expected;
    for (UINT i = 0; i < half_words; ++i) {
        expected[i] = pattern(1, words - half_words + i);
        expected[half_words + i] = pattern(0, i);
    }
    for (UINT i = 0; i < span_words; ++i) {
        if (source[i] != expected[i]) {
            std::fprintf(stderr, "CPU source provenance mismatch at word %u\n", i);
            return 1;
        }
    }

    Queue direct;
    ComPtr<ID3D12Resource> destination, readback;
    auto desc = buffer_desc();
    desc.Width = span_bytes;
    D3D12_HEAP_PROPERTIES heap{};
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (!direct.init(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT) ||
        !check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&destination)), "Create GPU destination")) return 1;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (!check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)), "Create GPU readback")) return 1;

    UINT64 serial = 0;
    const auto capture = [&](bool upload, std::uint32_t change_mask, const char* label) {
        ComPtr<ID3D12GraphicsCommandList> transfer, read;
        if (upload) {
            if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct.allocator.Get(),
                                                  nullptr, IID_PPV_ARGS(&transfer)), "Create transfer list")) return false;
            transfer->CopyBufferRegion(destination.Get(), 0, mapped_pages[0]->staging.Get(), logical_offset, half_bytes);
            transfer->CopyBufferRegion(destination.Get(), half_bytes, mapped_pages[1]->staging.Get(), 0, half_bytes);
            if (!check(transfer->Close(), "Close transfer list")) return false;
            direct.execute(transfer.Get());
        }
        if (!record_copy(device.Get(), direct, readback.Get(), 0, destination.Get(), span_bytes, read)) return false;
        direct.execute(read.Get());
        if (!check(direct.commands->Signal(direct.fence.Get(), ++serial), "Signal page readback") ||
            !wait_for_read(direct.fence.Get(), serial)) return false;
        void* data = nullptr;
        const D3D12_RANGE range{0, span_bytes}, no_write{};
        if (!check(readback->Map(0, &range, &data), "Map page readback")) return false;
        const auto* result = static_cast<const std::uint32_t*>(data);
        UINT mismatches = 0;
        for (UINT i = 0; i < span_words; ++i) mismatches += result[i] != (expected[i] ^ change_mask);
        readback->Unmap(0, &no_write);
        std::printf("%s: %u/%u mismatches\n", label, mismatches, span_words);
        return mismatches == 0;
    };

    if (!capture(true, 0, "cross-page upload in mapped order")) return 1;
    for (UINT i = 0; i < span_words; ++i) source[i] = expected[i] ^ changed;
    if (pages[1].canonical[words - half_words] != (expected[0] ^ changed) ||
        pages[0].canonical[0] != (expected[half_words] ^ changed)) return 1;
    return capture(false, 0, "GPU keeps its prior snapshot after CPU writes") &&
           capture(true, changed, "re-upload observes the updated CPU pages") ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const std::string_view mode(argv[1]);
    if (mode == "raw_srv_warp"sv || mode == "raw_srv_hardware"sv) return raw_srv(mode == "raw_srv_warp"sv);
    if (mode == "raw_uav_warp"sv || mode == "raw_uav_hardware"sv) return raw_uav(mode == "raw_uav_warp"sv);
    if (mode == "provenance_warp"sv || mode == "provenance_hardware"sv) return provenance(mode == "provenance_warp"sv);
    if (mode != "warp"sv && mode != "hardware"sv) return 1;
    return run(mode == "warp"sv);
}
