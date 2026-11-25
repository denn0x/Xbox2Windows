// LOCAL stall: device slot 133 (CreatePlacedResourceX) with and without X2W_PLACED_FAST.
//
//   startup_placed_fast <warp|hardware> <fast|walk>
//   (X2W_D3D12_RENDER=1 X2W_D3D12_PERMISSIVE=1 X2W_D3D12_LIST_NOOP=1; fast also X2W_PLACED_FAST=1;
//    the test sets X2W_TRAP_LOG itself)
//
// One private 4 MiB allocation, all committed. Buffers, in order:
//   A base +0, 64 KiB          (opens the allocation's heap: always walked)
//   B base +1 MiB, 1 MiB       (inside the heap)
//   C base +3 MiB, 2 MiB       (past the end of the allocation)
// Expected in both modes: A and B "aliased", C "not aliased" (the result the walk gives). fast: the
// "X2W_PLACED_FAST" line counts exactly 1 call (B; C is past the region queried at its address, so it is walked).
// Not covered: a buffer crossing a region boundary inside the allocation. A read-only page makes
// OpenExistingHeapFromAddress fail on WARP for the whole allocation, so that split cannot be built here; walk (control): no such line.
// Exit 0 pass, 77 capability skip (no adapter, no existing-heap support), other values name the step.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0, Word e = 0, Word f = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string mode = argv[2];
    if (mode != "fast" && mode != "walk") return 2;
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
    {
        ComPtr<ID3D12Device3> desktop;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 4;
        void* block = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ComPtr<ID3D12Heap> heap;
        const HRESULT hr = block ? desktop->OpenExistingHeapFromAddress(block, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
        heap.Reset();
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        if (FAILED(hr)) {
            std::printf("OpenExistingHeapFromAddress -> 0x%08lx: capability skip\n", static_cast<unsigned long>(hr));
            return 77;
        }
    }
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 5;
    const std::wstring log_path = std::wstring(temp) + L"x2w_placed_fast_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                  std::to_wstring(GetTickCount64()) + L".log";
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path.c_str());

    std::array<unsigned char, 64> device_parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), device_parameters.data(), __uuidof(ID3D12Device), &device))) return 6;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return 7;
    struct Place { const char* name; std::size_t offset, width; bool decommit_first; };
    const Place places[] = {{"A", 0, 64u << 10, false}, {"B", 1u << 20, 1u << 20, false}, {"C", 3u << 20, 2u << 20, false}};
    std::vector<void*> made;
    for (const Place& p : places) {
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, p.width, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        void* resource = nullptr;
        const Word hr = invoke(device, 133, arg(memory + p.offset), arg(&buffer), D3D12_RESOURCE_STATE_COMMON, 0,
                               arg(&__uuidof(ID3D12Resource)), arg(&resource));
        std::printf("%s: +0x%zx width 0x%zx -> 0x%08llx\n", p.name, p.offset, p.width, static_cast<unsigned long long>(hr));
        made.push_back(resource);
    }

    std::ifstream file(log_path, std::ios::binary);
    const std::string log{std::istreambuf_iterator<char>(file), {}};
    // The slot-133 lines in call order: their placement word.
    std::vector<std::string> placement;
    for (std::size_t p = log.find("Device::slot133(CreatePlacedResourceX?) address="); p != std::string::npos;
         p = log.find("Device::slot133(CreatePlacedResourceX?) address=", p + 1)) {
        const std::string line = log.substr(p, log.find('\n', p) - p);
        placement.push_back(line.find(" not aliased") != std::string::npos ? "not aliased" : line.find(" aliased") != std::string::npos ? "aliased" : "?");
    }
    int fast_calls = 0;
    if (const std::size_t p = log.find("slot133 X2W_PLACED_FAST: "); p != std::string::npos)
        for (std::size_t q = p; q != std::string::npos; q = log.find("slot133 X2W_PLACED_FAST: ", q + 1))
            fast_calls = std::atoi(log.c_str() + q + std::strlen("slot133 X2W_PLACED_FAST: "));
    int failures = 0;
    const char* want[] = {"aliased", "aliased", "not aliased"};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string got = i < placement.size() ? placement[i] : "missing";
        std::printf("%s placement: %s (want %s)\n", places[i].name, got.c_str(), want[i]);
        if (got != want[i]) ++failures;
    }
    std::printf("X2W_PLACED_FAST calls logged: %d (want %d)\n", fast_calls, mode == "fast" ? 1 : 0);
    if (fast_calls != (mode == "fast" ? 1 : 0)) ++failures;
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 16 : 0;
}
