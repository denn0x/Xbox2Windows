#include "cpu/shim_gpu_device.h"
#include "desktop_gpu.h"
#include "startup_stubs/draw_dump_tracking.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using x2w_startup::DrawDump;
using x2w_startup::DrawReadbacks;

namespace {
void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

void record_copy(DrawReadbacks& tracker, ID3D12Device* device, ID3D12GraphicsCommandList* list,
                 ID3D12Resource* upload, UINT word, UINT cookie)
{
    DrawDump dump{};
    dump.buffer = make_buffer(device, sizeof(UINT), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    dump.key = cookie;
    auto* destination = dump.buffer.Get();
    require(tracker.record(list, std::move(dump)), "Could not record capture");
    list->CopyBufferRegion(destination, 0, upload, sizeof(UINT) * word, sizeof(UINT));
}

void run()
{
    auto device = shim_create_device();
    auto upload = make_buffer(device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    constexpr UINT words[] = {0x1c205ea7, 0x9df81342};
    D3D12_RANGE none{};
    void* mapped = nullptr;
    check(upload->Map(0, &none, &mapped), "Map input");
    std::memcpy(mapped, words, sizeof(words));
    upload->Unmap(0, nullptr);

    DrawReadbacks tracker;
    ComputeCommands work(device.Get(), nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ComputeCommands other(device.Get(), nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto* list = work.list.Get();
    auto* queue = work.queue.Get();
    record_copy(tracker, device.Get(), list, upload.Get(), 0, 10);
    check(list->Close(), "Close first recording");
    require(tracker.take_completed().empty(), "Unsubmitted recording published a capture");

    // This reset replaces a recording that was closed but never submitted.
    check(list->Reset(work.allocator.Get(), nullptr), "Reset unsubmitted list");
    tracker.discard(list);
    record_copy(tracker, device.Get(), list, upload.Get(), 1, 20);
    check(list->Close(), "Close replacement recording");
    check(other.list->Close(), "Close unrelated list");
    ID3D12CommandList* batch[] = {other.list.Get()};
    check(tracker.execute(other.queue.Get(), 1, batch), "Execute unrelated list");
    require(tracker.take_completed().empty(), "Unrelated queue published the replacement recording");

    ComPtr<ID3D12Fence> gate, done;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Create CPU gate");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "Create completion fence");
    check(queue->Wait(gate.Get(), 1), "Hold execution behind gate");
    batch[0] = list;
    check(tracker.execute(queue, 1, batch), "Submit capture list");
    const bool empty_before_signal = tracker.take_completed().empty();

    // Reset the command-list object with a fresh allocator while its previous
    // recording is in flight. The readback must outlive this recording reset.
    ComPtr<ID3D12CommandAllocator> next;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next)), "Create different allocator");
    check(list->Reset(next.Get(), nullptr), "Reset submitted list");
    tracker.discard(list);
    record_copy(tracker, device.Get(), list, upload.Get(), 0, 30);
    check(list->Close(), "Close yet-unsubmitted list");
    check(gate->Signal(1), "Open gate");
    check(queue->Signal(done.Get(), 1), "Signal completion");
    check(done->SetEventOnCompletion(1, nullptr), "Wait for execution");
    check(device->GetDeviceRemovedReason(), "Device after submission");
    require(empty_before_signal, "Capture published before queue could execute");
    auto completed = tracker.take_completed();
    require(completed.size() == 1 && completed.front().key == 20, "Expected only the submitted recording");
    auto* readback = completed.front().buffer.Get();
    D3D12_RANGE range{0, sizeof(UINT)};
    check(readback->Map(0, &range, &mapped), "Map completed copy");
    UINT actual = 0;
    std::memcpy(&actual, mapped, sizeof(actual));
    readback->Unmap(0, &none);
    require(actual == words[1], "Mapped value differs from the executed copy");
    require(tracker.take_completed().empty(), "Capture published more than once");
    check(list->Reset(next.Get(), nullptr), "Discard final unsubmitted list");
    tracker.discard(list);
    check(list->Close(), "Close empty list");
    std::printf("submitted=1 cancelled=2 published=1 readback=%08x\n", actual);
}

// Stands in for a queue so the test decides when, and with which value, the
// tracker's fence completes. Lists are passed through without being executed.
class ScriptedQueue final : public ID3D12CommandQueue {
public:
    explicit ScriptedQueue(ID3D12Device* device) : device_(device) {}
    HRESULT signal_result = S_OK;
    UINT executes = 0;
    UINT signals = 0;
    UINT64 signal_value = 0;
    ComPtr<ID3D12Fence> fence;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out) return E_POINTER;
        *out = id == __uuidof(IUnknown) || id == __uuidof(ID3D12CommandQueue) ? this : nullptr;
        return *out ? S_OK : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** out) override { return device_->QueryInterface(id, out); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
        const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
        D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*,
        const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT, ID3D12CommandList* const*) override { ++executes; }
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* target, UINT64 value) override
    {
        ++signals;
        fence = target;
        signal_value = value;
        return signal_result;
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64*, UINT64*) override { return E_NOTIMPL; }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return {D3D12_COMMAND_LIST_TYPE_DIRECT}; }

private:
    ID3D12Device* device_;
};

DrawDump keyed(std::uint64_t key)
{
    DrawDump dump{};
    dump.key = key;
    return dump;
}

unsigned fill(DrawReadbacks& tracker, ID3D12CommandList* list)
{
    unsigned recorded = 0;
    while (tracker.record(list, keyed(0xd0))) ++recorded;
    return recorded;
}

// The capture budget is 48 recorded or published copies. Lists are compared
// by address only, so four bytes of storage stand in for four lists.
void run_budget(bool mutate_removed)
{
    auto device = shim_create_device();
    ScriptedQueue queue(device.Get());
    char storage[4]{};
    ID3D12CommandList* lists[4];
    for (int i = 0; i < 4; ++i) lists[i] = reinterpret_cast<ID3D12CommandList*>(&storage[i]);
    auto* a = lists[0];
    auto* b = lists[1];
    auto* c = lists[2];
    auto* d = lists[3];

    DrawReadbacks tracker;
    require(tracker.record(a, keyed(0xa1)) && tracker.record(b, keyed(0xb1)), "Could not record a1 and b1");
    require(tracker.record(c, keyed(0xc0)), "Could not record c0");
    tracker.discard(c);
    require(tracker.record(c, keyed(0xc1)), "Could not record c1");

    ID3D12CommandList* both[] = {a, b};
    check(tracker.execute(&queue, 2, both), "Execute a and b");
    require(queue.executes == 1 && queue.signals == 1 && queue.signal_value == 1, "Expected one execute and one signal of 1");
    require(tracker.take_completed().empty(), "Batch published before its fence completed");
    check(queue.fence->Signal(1), "Complete the batch fence");
    auto done = tracker.take_completed();
    require(done.size() == 2, "Expected both lists of the batch to complete");
    require(done[0].key == 0xa1 && done[0].index == 1, "Expected a1 as capture 1");
    require(done[1].key == 0xb1 && done[1].index == 2, "Expected b1 as capture 2");

    // c1 is pending and two captures are published: 45 of 48 slots remain.
    for (int i = 0; i < 45; ++i)
        require(!tracker.full() && tracker.record(d, keyed(0xd0)), "Budget closed before 48 captures");
    require(tracker.full() && !tracker.record(d, keyed(0xd0)), "Budget open after 48 captures");
    tracker.discard(d);

    // D3D12 reports device removal as UINT64_MAX. c1 is then dropped without
    // being published, and it no longer counts against the budget.
    ID3D12CommandList* third[] = {c};
    check(tracker.execute(&queue, 1, third), "Execute c");
    require(queue.signals == 2, "Expected a second fence signal");
    check(queue.fence->Signal(UINT64_MAX), "Report device removal");
    const auto removed = tracker.take_completed();
    require(removed.size() == (mutate_removed ? 1u : 0u), "Capture published after device removal");
    const unsigned after_removal = fill(tracker, d);
    require(after_removal == 46, "Expected 46 slots after device removal");
    tracker.discard(d);

    // A failed fence signal retains the copy within the budget: a later reset
    // of the list does not release it and it is never published.
    queue.signal_result = DXGI_ERROR_DEVICE_REMOVED;
    require(tracker.record(a, keyed(0xa2)), "Could not record a2");
    ID3D12CommandList* first[] = {a};
    require(tracker.execute(&queue, 1, first) == DXGI_ERROR_DEVICE_REMOVED, "Signal failure was not returned");
    tracker.discard(a);
    require(tracker.take_completed().empty(), "Capture published without a signalled fence");
    const unsigned after_failure = fill(tracker, d);
    require(after_failure == 45, "Expected 45 slots with a retained copy");
    std::printf("batch=2 budget=45 removed=%zu after_removal=%u after_failure=%u\n", removed.size(), after_removal, after_failure);
}
}  // namespace

int main(int argc, char** argv)
{
    bool mutate_removed = false;
    if (argc == 3 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "removed") == 0) mutate_removed = true;
    else if (argc != 1) { std::fputs("usage: startup_dump_fence_check [--mutate removed]\n", stderr); return 2; }
    try { run(); run_budget(mutate_removed); }
    catch (const std::exception& err) { std::fprintf(stderr, "%s\n", err.what()); return 1; }
    catch (...) { return 1; }
    return 0;
}
