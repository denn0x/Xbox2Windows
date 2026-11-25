// LOCAL indirect (X2W_RANGE_ALIAS, board msgs/064900 proposal): can a reserved buffer's 64 KiB tiles be mapped onto
// heaps opened over pagefile file mappings (ID3D12Device3::OpenExistingHeapFromFileMapping, the kind section_heap()
// opens over xmem's chunk sections)? The stand-in's X2W_RANGE_ALIAS gate relies on the answer.
//
//   tile_mapping_shared_heap_check <warp|hardware> <plain|shared> [nodebug]
//
// Both modes reserve a 3-tile buffer and map its tiles out of order onto two 4-tile heaps:
//   tile 0 -> heap 0 tile 2, tile 1 -> heap 1 tile 0, tile 2 -> heap 0 tile 1.
// plain (positive control): the heaps come from CreateHeap (DEFAULT, buffers only). An upload buffer with a pattern
//   is copied into the reserved buffer, then each heap is read back through a buffer placed on it: every dword must
//   be where the mapping puts it. Pass: exact, device not removed.
// shared: the heaps come from OpenExistingHeapFromFileMapping over two 256 KiB pagefile sections (kept small: on the
//   RX 9070 XT such an opened section keeps its commit charge until reboot, xmem_gpu_pool.h), the pattern is written
//   through CPU views and read back through the reserved buffer. Expected (measured 2026-11-25 on WARP and the
//   RX 9070 XT, with and without the debug layer): the runtime refuses ("UpdateTileMappings does not work with
//   cross-adapter shared heaps", debug id 493) and removes the device (reason 0x887a0001). Pass: the heap flags carry
//   D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER and the device is removed. If the mapping ever works instead, the test FAILS
//   on purpose, saying so: X2W_RANGE_ALIAS's gate (d3d12_range_alias.inl) then refuses something that would work.
// Exit 0 pass, 77 no adapter / no tiled resources, other values fail.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT64 page = 65536;
constexpr struct { int heap; UINT tile; } mapping[3] = {{0, 2}, {1, 0}, {0, 1}};

void drain(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages() && i < 8; ++i) {
        SIZE_T bytes = 0;
        queue->GetMessage(i, nullptr, &bytes);
        auto* message = static_cast<D3D12_MESSAGE*>(std::malloc(bytes));
        if (message && SUCCEEDED(queue->GetMessage(i, message, &bytes)))
            std::printf("debug sev=%d id=%d %s\n", message->Severity, message->ID, message->pDescription);
        std::free(message);
    }
    queue->ClearStoredMessages();
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    return {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
}

ComPtr<ID3D12Resource> committed(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state)
{
    const D3D12_HEAP_PROPERTIES properties{type};
    const D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

UINT pattern(int heap, UINT64 byte) { return (static_cast<UINT>(heap + 1) << 28) | static_cast<UINT>(byte); }
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const bool hardware = !std::strcmp(argv[1], "hardware");
    const bool shared = !std::strcmp(argv[2], "shared");
    if ((!hardware && std::strcmp(argv[1], "warp")) || (!shared && std::strcmp(argv[2], "plain"))) return 2;
    const bool debug_layer = !(argc > 3 && !std::strcmp(argv[3], "nodebug"));
    ComPtr<ID3D12Debug> debug;
    if (debug_layer && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter> adapter;
    if (hardware) {
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) return 77;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(hr) || FAILED(candidate->GetDesc1(&desc))) return 3;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                break;
            }
        }
    } else if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        return 3;
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    LARGE_INTEGER driver{};
    adapter->GetDesc(&adapter_desc);
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    std::printf("adapter=%ls driver=%u.%u.%u.%u mode=%s debug layer %s\n", adapter_desc.Description, HIWORD(driver.HighPart),
                LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart), argv[2], debug_layer ? "on" : "off");
    ComPtr<ID3D12Device3> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return 77;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    std::printf("tiled resources tier %d\n", static_cast<int>(options.TiledResourcesTier));
    if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_1) return 77;

    ComPtr<ID3D12Heap> heaps[2];
    unsigned char* views[2] = {};
    for (int i = 0; i < 2; ++i) {
        HRESULT hr;
        if (shared) {
            const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4 * page, nullptr);
            views[i] = section ? static_cast<unsigned char*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 4 * page)) : nullptr;
            if (!views[i]) return 4;
            for (UINT64 b = 0; b < 4 * page; b += 4) *reinterpret_cast<UINT*>(views[i] + b) = pattern(i, b);
            hr = device->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heaps[i]));
        } else {
            const D3D12_HEAP_DESC desc{4 * page, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
            hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heaps[i]));
        }
        const D3D12_HEAP_DESC got = SUCCEEDED(hr) ? heaps[i]->GetDesc() : D3D12_HEAP_DESC{};
        std::printf("heap %d (%s) -> 0x%08lx size 0x%llx flags 0x%x%s\n", i, shared ? "OpenExistingHeapFromFileMapping" : "CreateHeap",
                    static_cast<unsigned long>(hr), got.SizeInBytes, got.Flags,
                    got.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER ? " (SHARED_CROSS_ADAPTER)" : "");
        if (FAILED(hr)) return 5;
        if (shared && !(got.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)) {
            std::printf("FAIL: an opened file-mapping heap without SHARED_CROSS_ADAPTER: the gate's assumption is wrong\n");
            return 6;
        }
    }
    const D3D12_RESOURCE_DESC reserved_desc = buffer_desc(3 * page, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> reserved;
    HRESULT hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved));
    std::printf("CreateReservedResource -> 0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return 7;
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return 8;
    for (UINT t = 0; t < 3; ++t) {
        const D3D12_TILED_RESOURCE_COORDINATE coordinate{t, 0, 0, 0};
        const D3D12_TILE_REGION_SIZE region{1, FALSE, 0, 0, 0};
        const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
        const UINT start = mapping[t].tile, count = 1;
        queue->UpdateTileMappings(reserved.Get(), 1, &coordinate, &region, heaps[mapping[t].heap].Get(), 1, &flags, &start, &count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
    }
    const HRESULT removed_after_mapping = device->GetDeviceRemovedReason();
    std::printf("after UpdateTileMappings: device removed reason 0x%08lx\n", static_cast<unsigned long>(removed_after_mapping));
    drain(device.Get());
    if (shared && removed_after_mapping != S_OK) {
        std::printf("shared: tile mapping onto opened file-mapping heaps refused (device removed 0x%08lx): as measured; PASS\n",
                    static_cast<unsigned long>(removed_after_mapping));
        return 0;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 9;
    ComPtr<ID3D12Resource> readback[2];
    ComPtr<ID3D12Resource> upload, placed[2];
    if (shared) {
        readback[0] = committed(device.Get(), D3D12_HEAP_TYPE_READBACK, 3 * page, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!readback[0]) return 10;
        list->CopyBufferRegion(readback[0].Get(), 0, reserved.Get(), 0, 3 * page);
    } else {
        upload = committed(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 3 * page, D3D12_RESOURCE_STATE_GENERIC_READ);
        UINT* words = nullptr;
        if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&words)))) return 10;
        for (UINT t = 0; t < 3; ++t)
            for (UINT64 b = 0; b < page; b += 4) words[(t * page + b) / 4] = pattern(mapping[t].heap, mapping[t].tile * page + b);
        upload->Unmap(0, nullptr);
        list->CopyBufferRegion(reserved.Get(), 0, upload.Get(), 0, 3 * page);
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};  // the reserved buffer's writes reach the heaps
        barrier.Aliasing = {nullptr, nullptr};
        list->ResourceBarrier(1, &barrier);
        for (int i = 0; i < 2; ++i) {
            const D3D12_RESOURCE_DESC desc = buffer_desc(4 * page);
            if (FAILED(device->CreatePlacedResource(heaps[i].Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed[i])))) return 11;
            readback[i] = committed(device.Get(), D3D12_HEAP_TYPE_READBACK, 4 * page, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!readback[i]) return 11;
            list->CopyBufferRegion(readback[i].Get(), 0, placed[i].Get(), 0, 4 * page);
        }
    }
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    for (const ULONGLONG deadline = GetTickCount64() + 30000; fence->GetCompletedValue() < 1;) {
        if (GetTickCount64() >= deadline || device->GetDeviceRemovedReason() != S_OK) break;
        Sleep(1);
    }
    drain(device.Get());
    const HRESULT removed = device->GetDeviceRemovedReason();
    std::printf("device removed reason 0x%08lx\n", static_cast<unsigned long>(removed));
    if (removed != S_OK) return 12;
    int bad = 0;
    for (UINT t = 0; t < 3; ++t) {
        const int heap = mapping[t].heap;
        const UINT* words = nullptr;
        const D3D12_RANGE all{0, (shared ? 3 : 4) * page};
        ID3D12Resource* source = readback[shared ? 0 : heap].Get();
        if (FAILED(source->Map(0, &all, reinterpret_cast<void**>(const_cast<UINT**>(&words))))) return 13;
        for (UINT64 b = 0; b < page; b += 4) {
            const UINT got = shared ? words[(t * page + b) / 4] : words[(mapping[t].tile * page + b) / 4];
            const UINT want = pattern(heap, mapping[t].tile * page + b);
            if (got != want && bad++ < 4) std::printf("tile %u (heap %d tile %u) +0x%llx: %08x, want %08x\n", t, heap, mapping[t].tile, b, got, want);
        }
        source->Unmap(0, nullptr);
    }
    if (shared) {
        std::printf("FAIL: tile mapping onto opened file-mapping heaps WORKED (%s, %d bad dwords). Revisit X2W_RANGE_ALIAS's gate.\n",
                    bad ? "with wrong data" : "exact", bad);
        return 14;
    }
    std::printf("plain: 3 tiles mapped out of order onto 2 CreateHeap heaps, written through the reserved buffer: %s (%d bad dwords)\n",
                bad ? "MISMATCH" : "exact", bad);
    std::printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 15 : 0;
}
