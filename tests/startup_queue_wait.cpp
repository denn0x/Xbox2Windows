// Console queue Wait (slot 17) through the D3D12 stand-in.
//
//   startup_queue_wait <warp|hardware>
//
// The probe makes one queue and two fences through D3D12XboxCreateDevice and calls the console
// queue slots directly:
//
//   wait:    slot 17 Wait(F, 1), slot 16 Signal(G, 1). G must stay 0 until the CPU signals F, then
//            reach 1.
//   control: the same calls with the wait on slot 15, which the stand-in still dispatches by
//            argument shape as a Signal. G reaches 1 without F being signalled, which is how the
//            stand-in treated slot 17 before. The control checks that the probe can see that.
//
// Exit codes: 0 pass, 77 no physical adapter, other values name the failing step.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d);
}

void* make_fence(void* device)
{
    void* fence = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))))) return nullptr;
    return fence;
}

Word completed(void* fence) { return invoke(fence, 8); }

bool reaches(void* fence, Word value, DWORD milliseconds)
{
    const auto deadline = GetTickCount64() + milliseconds;
    while (completed(fence) < value) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
    return true;
}

// Wait on F through wait_slot, then Signal G through slot 16. Returns G's completed value 250 ms
// later, before F is signalled, or ~0 on failure.
Word queue_then_signal(void* queue, void* f, void* g, unsigned wait_slot)
{
    if (FAILED(static_cast<HRESULT>(invoke(queue, wait_slot, arg(f), 1)))) return ~Word{};
    if (FAILED(static_cast<HRESULT>(invoke(queue, 16, arg(g), 1)))) return ~Word{};
    Sleep(250);
    return completed(g);
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
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return 3;
    std::printf("adapter=%ls\n", desc.Description);

    std::array<unsigned char, 64> parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &device))) return 4;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    void* queue = nullptr;
    void* control_queue = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue)))) ||
        FAILED(static_cast<HRESULT>(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&control_queue)))))
        return 5;
    void* f = make_fence(device);
    void* g = make_fence(device);
    void* control_f = make_fence(device);
    void* control_g = make_fence(device);
    if (!f || !g || !control_f || !control_g) return 6;

    // Control first, on its own queue: slot 15 is still dispatched by shape.
    const Word control = queue_then_signal(control_queue, control_f, control_g, 15);
    if (control == ~Word{}) return 7;
    std::printf("control: slot 15 as the wait, G=%llu before F is signalled (shape dispatch signals)\n",
                static_cast<unsigned long long>(control));
    if (control != 1) return 8;

    const Word before = queue_then_signal(queue, f, g, 17);
    if (before == ~Word{}) return 9;
    std::printf("wait: slot 17, G=%llu before F is signalled\n", static_cast<unsigned long long>(before));
    if (before != 0) return 10;
    if (completed(f) != 0) return 11;  // the wait must not signal F
    if (FAILED(static_cast<HRESULT>(invoke(f, 10, 1)))) return 12;  // fence Signal(1) from the CPU
    if (!reaches(g, 1, 10000)) return 13;
    std::printf("wait: after F=1 on the CPU, G=%llu\n", static_cast<unsigned long long>(completed(g)));
    return 0;
}
