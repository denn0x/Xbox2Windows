// OMSetRenderTargets (list slot 50) with a single descriptor range, handles that hold no view, and the
// outcome counters behind the x2w-targets report line.
//
//   startup_rtv_bind <warp|hardware>     (X2W_D3D12_PERMISSIVE=1 X2W_D3D12_LIST_NOOP=1 X2W_D3D12_RENDER=1 X2W_D3D12_DEBUG=1)
//
// Through D3D12XboxCreateDevice the probe makes an RTV heap of three descriptors over two 1x1 RGBA8 targets,
// views 0 and 1 created, view 2 left empty, and records slot 50 calls on one list. After each call
// x2w_d3d12_target_bind_stats must show exactly one new outcome, the expected one:
//
//   1. one handle, array form                      bound
//   2. one handle, single range, count 1           single-range bound
//   3. one handle, single range, count 2           single-range bound (view 1 follows view 0 at the RTV stride)
//   4. two handles, array form                     bound
//   5. single range, count 3                       refused: unresolved (view 2 holds nothing)
//   6. single range, count 4                       refused: range past heap, or unresolved when the heap has no
//                                                  title-side range to measure against
//   7. view 2 alone                                refused: unresolved
//   8. single range, count 0, null handles         refused: empty
//   9. count 9                                     refused: count over 8
//  10. view 0 copied into slot 2 (CopyDescriptorsSimple), then slot 2 bound    bound
//
// Then the list is closed and submitted; the desktop device must not be removed and the debug layer must
// store no errors. Draws are not covered.
//
// Exit codes: 0 pass, 77 no physical adapter, other values name the failing step.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);
extern "C" __declspec(dllimport) unsigned x2w_d3d12_target_bind_stats(unsigned* counts, unsigned capacity);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0, Word e = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e);
}
bool ok(Word hr) { return SUCCEEDED(static_cast<HRESULT>(hr)); }

// TargetBind order in d3d12_xs_device.cpp.
enum Outcome { bound, single_bound, not_recording, over_8, unreadable, past_heap, wrong_kind, unresolved, empty,
               depth_kind, depth_unknown, depth_plane, outcome_count };
const char* const names[] = {"bound", "single-range bound", "not recording", "count over 8", "handles unreadable",
                             "range past heap", "wrong kind", "unresolved", "empty", "depth wrong kind",
                             "depth unknown", "depth one plane"};

struct Stats {
    unsigned n[outcome_count] = {};
    static Stats read()
    {
        Stats s;
        x2w_d3d12_target_bind_stats(s.n, outcome_count);
        return s;
    }
};

// Exactly one new outcome since `before`, and it is `want` (or `also`, when given).
bool step(const char* name, Outcome want, Stats& before, int also = -1)
{
    const Stats now = Stats::read();
    unsigned total = 0;
    int got = -1;
    for (int i = 0; i < outcome_count; ++i) {
        const unsigned added = now.n[i] - before.n[i];
        total += added;
        if (added) got = i;
    }
    before = now;
    const bool good = total == 1 && (got == want || got == also);
    std::printf("%s: %u new outcome(s), last %s (expected %s%s%s)\n", name, total, got >= 0 ? names[got] : "none", names[want],
                also >= 0 ? " or " : "", also >= 0 ? names[also] : "");
    return good;
}

UINT64 debug_errors(ID3D12Device* desktop)
{
    ComPtr<ID3D12InfoQueue> info;
    if (FAILED(desktop->QueryInterface(IID_PPV_ARGS(&info)))) return 0;
    UINT64 errors = 0;
    for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
        std::vector<char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info->GetMessage(i, message, &bytes))) continue;
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;
            std::printf("debug layer: %.*s\n", static_cast<int>(message->DescriptionByteLength), message->pDescription);
        }
    }
    return errors;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter> adapter;
    if (std::strcmp(argv[1], "hardware") == 0) {
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) return 77;
            if (FAILED(hr)) return 3;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) return 3;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                adapter = candidate;
                break;
            }
        }
    } else if (std::strcmp(argv[1], "warp") != 0 || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        return 3;
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(adapter->GetDesc(&adapter_desc))) return 3;
    std::printf("adapter=%ls\n", adapter_desc.Description);

    std::array<unsigned char, 64> parameters{};
    void* device = nullptr;
    ComPtr<ID3D12Device> desktop;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &device))) return 4;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 4;

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                          D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}};
    ComPtr<ID3D12Resource> targets[2];
    for (auto& target : targets)
        if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_COMMON,
                                                    &clear, IID_PPV_ARGS(&target)))) return 5;

    // The RTV heap: views 0 and 1 over the two targets, slot 2 empty; and a one-descriptor staging heap.
    const auto make_heap = [&](UINT count, void** out) {
        const struct {
            D3D12_DESCRIPTOR_HEAP_DESC desc;
            Word backing, bytes;
        } desc{{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count}, 0, 0};
        return ok(invoke(device, 14, arg(&desc), arg(&__uuidof(ID3D12DescriptorHeap)), arg(out)));
    };
    void *rtv_heap = nullptr, *staging_heap = nullptr;
    if (!make_heap(3, &rtv_heap) || !make_heap(1, &staging_heap)) return 6;
    // The title's stride: the stand-in forwards GetDescriptorHandleIncrementSize (device slot 15).
    const UINT stride = static_cast<UINT>(invoke(device, 15, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    std::printf("RTV stride as the title sees it: %u (desktop %u)\n", stride,
                desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    const D3D12_CPU_DESCRIPTOR_HANDLE first{static_cast<SIZE_T>(invoke(rtv_heap, 9))};
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> rtv{{first, {first.ptr + stride}, {first.ptr + 2 * stride}}};
    const D3D12_CPU_DESCRIPTOR_HANDLE staging{static_cast<SIZE_T>(invoke(staging_heap, 9))};
    D3D12_RENDER_TARGET_VIEW_DESC view{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D, {}};
    invoke(device, 20, arg(targets[0].Get()), arg(&view), rtv[0].ptr);
    invoke(device, 20, arg(targets[1].Get()), arg(&view), rtv[1].ptr);
    invoke(device, 20, arg(targets[0].Get()), arg(&view), staging.ptr);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    void *queue = nullptr, *fence = nullptr, *allocator = nullptr, *list = nullptr;
    if (!ok(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue))) ||
        !ok(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))) ||
        !ok(invoke(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator))) ||
        !ok(invoke(device, 73, arg(list_desc), arg(allocator), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list))))
        return 7;
    if (!ok(invoke(list, 139, arg(allocator), 0))) return 8;

    const D3D12_CPU_DESCRIPTOR_HANDLE pair[] = {rtv[0], rtv[1]};
    Stats seen = Stats::read();
    invoke(list, 50, 1, arg(&rtv[0]), FALSE, 0);
    if (!step("1 one handle", bound, seen)) return 10;
    invoke(list, 50, 1, arg(&rtv[0]), TRUE, 0);
    if (!step("2 single range, count 1", single_bound, seen)) return 11;
    invoke(list, 50, 2, arg(&rtv[0]), TRUE, 0);
    if (!step("3 single range, count 2", single_bound, seen)) return 12;
    invoke(list, 50, 2, arg(pair), FALSE, 0);
    if (!step("4 two handles", bound, seen)) return 13;
    invoke(list, 50, 3, arg(&rtv[0]), TRUE, 0);
    if (!step("5 single range reaching the empty view", unresolved, seen)) return 14;
    invoke(list, 50, 4, arg(&rtv[0]), TRUE, 0);
    if (!step("6 single range past the heap end", past_heap, seen, unresolved)) return 15;
    invoke(list, 50, 1, arg(&rtv[2]), FALSE, 0);
    if (!step("7 the empty view alone", unresolved, seen)) return 16;
    invoke(list, 50, 0, 0, TRUE, 0);
    if (!step("8 single range, count 0, null handles", empty, seen)) return 17;
    invoke(list, 50, 9, arg(&rtv[0]), TRUE, 0);
    if (!step("9 count above 8", over_8, seen)) return 18;
    invoke(device, 24, 1, rtv[2].ptr, staging.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    invoke(list, 50, 1, arg(&rtv[2]), FALSE, 0);
    if (!step("10 copied view in the empty slot", bound, seen)) return 19;

    if (!ok(invoke(list, 9))) return 20;
    void* lists[] = {list};
    invoke(queue, 10, 1, arg(lists));
    if (!ok(invoke(queue, 14, arg(fence), 1))) return 21;
    const auto deadline = GetTickCount64() + 10000;
    while (invoke(fence, 8) < 1) {
        if (GetTickCount64() >= deadline) return 22;
        Sleep(1);
    }
    const HRESULT removed = desktop->GetDeviceRemovedReason();
    const UINT64 errors = debug_errors(desktop.Get());
    std::printf("device removed reason: 0x%08lx, debug layer errors: %llu\n", static_cast<unsigned long>(removed),
                static_cast<unsigned long long>(errors));
    return removed == S_OK && errors == 0 ? 0 : 70;
}
