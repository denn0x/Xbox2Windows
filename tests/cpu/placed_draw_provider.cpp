#include "../startup_stream_setup.h"
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
using Word = std::uint64_t;
template <class T> Word arg(T* value)
{
    return reinterpret_cast<Word>(value);
}
struct Buffer
{
    void* memory{};
    void* resource{};
};
struct Scene
{
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> desktop;
    StreamTarget frame;
    void* device{};
    void* pipeline{};
    Buffer buffers[2];
    FILE* trace{};
    bool submitted{};
    ~Scene()
    {
        for (auto& buffer : buffers)
        {
            if (buffer.resource)
                invoke(buffer.resource, 2);
            if (buffer.memory)
                VirtualFree(buffer.memory, 0, MEM_RELEASE);
        }
        for (auto* object : {frame.list, frame.allocator, frame.fence, frame.queue, frame.rtv_heap, pipeline, device})
            if (object)
                invoke(object, 2);
        if (trace)
            std::fclose(trace);
    }
};
std::unique_ptr<Scene> scene;
bool option(const char* name, const char* expected)
{
    const char* value = std::getenv(name);
    return value && std::strcmp(value, expected) == 0;
}
bool fence_complete(Scene& s)
{
    if (FAILED(static_cast<HRESULT>(invoke(s.frame.queue, 14, arg(s.frame.fence), 1))))
        return false;
    const auto until = GetTickCount64() + 10000;
    while (!invoke(s.frame.fence, 8) && GetTickCount64() < until)
        Sleep(1);
    return invoke(s.frame.fence, 8) == 1;
}
} // namespace

extern "C" __declspec(dllexport) Word placed_open()
{
    scene.reset(new (std::nothrow) Scene);
    if (!scene)
        return 0;
    auto& s = *scene;
    if (fopen_s(&s.trace, "calls.txt", "wb") || !s.trace)
        return 0;
    const bool hardware = option("X2W_PLACED_ADAPTER", "hardware");
    int rc = prepare_stream_pipeline("vs", "ps", "root", hardware, false, s.adapter, s.device, s.pipeline);
    if (rc == 77)
        return 77;
    if (rc || FAILED(D3D12CreateDevice(s.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s.desktop))))
        return 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 desc{};
    LARGE_INTEGER driver{};
    if (FAILED(s.adapter.As(&adapter1)) || FAILED(adapter1->GetDesc1(&desc)) ||
        FAILED(adapter1->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) return 0;
    Word selected_luid{};
    std::memcpy(&selected_luid, &desc.AdapterLuid, sizeof(LUID));
    const auto observer_luid = s.desktop->GetAdapterLuid();
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    // prepare_stream_pipeline gives this adapter to D3D12XboxCreateDevice. The
    // observer checks its own device LUID before reading the draw's target.
    if (software == hardware || std::memcmp(&observer_luid, &desc.AdapterLuid, sizeof(LUID))) return 0;
    std::fprintf(s.trace, "device %ls driver=%u.%u.%u.%u luid=%016llx software=%u\n", desc.Description,
                 HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart),
                 selected_luid, static_cast<unsigned>(software));
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    if (FAILED(s.desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) ||
        !heaps.Supported)
        return 77;
    if (open_stream_target(s.desktop.Get(), s.device, s.pipeline, 8, 4, DXGI_FORMAT_R8G8B8A8_UNORM, 8, s.frame))
        return 0;
    for (Word low = 0x1600000000ull; low < 0x1700000000ull; low += 16ull << 20)
    {
        for (unsigned i = 0; i < 2; ++i)
            s.buffers[i].memory = VirtualAlloc(reinterpret_cast<void*>(low + (Word{i} << 40)), 65536,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (s.buffers[0].memory && s.buffers[1].memory)
        {
            std::fprintf(s.trace, "open bit=40\n");
            return low;
        }
        for (auto& buf : s.buffers)
            if (buf.memory)
                VirtualFree(std::exchange(buf.memory, nullptr), 0, MEM_RELEASE);
    }
    return 0;
}
extern "C" __declspec(dllexport) Word placed_mode()
{
    if (option("X2W_PLACED_CASE", "low"))
        return 0;
    if (option("X2W_PLACED_CASE", "high"))
        return 1;
    if (option("X2W_PLACED_CASE", "reject-submit"))
        return 3;
    if (option("X2W_PLACED_CASE", "partial-open"))
        return 4;
    return 2;
}
extern "C" __declspec(dllexport) Word placed_resource(Word address, Word bytes)
{
    auto& s = *scene;
    for (UINT i = 0; i < 2; ++i)
    {
        auto& b = s.buffers[i];
        if (arg(b.memory) != address || b.resource || bytes > 65536)
            continue;
        std::fprintf(s.trace, "resource index=%u bytes=%llu\n", i, bytes);
        if (!bytes)
            return 0;
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        const auto hr =
            static_cast<HRESULT>(invoke(s.device, 133, address, arg(&desc), D3D12_RESOURCE_STATE_COMMON, 0,
                                        arg(&__uuidof(ID3D12Resource)), arg(&b.resource)));
        return SUCCEEDED(hr) && b.resource && invoke(b.resource, 11) == address;
    }
    return 0;
}
extern "C" __declspec(dllexport) Word placed_bind(Word address, UINT bytes, UINT stride)
{
    const auto& s = *scene;
    const auto base = arg(s.buffers[0].memory);
    const D3D12_VERTEX_BUFFER_VIEW view{address, bytes, stride};
    std::fprintf(s.trace, "bind address_delta=%llu bytes=%u stride=%u\n", address - base, bytes, stride);
    invoke(s.frame.list, 48, 0, 1, arg(&view));
    return 0;
}
extern "C" __declspec(dllexport) Word placed_draw(UINT vertices, UINT instances)
{
    std::fprintf(scene->trace, "draw vertices=%u instances=%u\n", vertices, instances);
    invoke(scene->frame.list, 12, vertices, instances);
    return 0;
}
extern "C" __declspec(dllexport) Word placed_submit(UINT lists, UINT fence_value)
{
    auto& s = *scene;
    std::fprintf(s.trace, "submit lists=%u fence=%u\n", lists, fence_value);
    if (lists != 1 || fence_value != 1 || FAILED(static_cast<HRESULT>(invoke(s.frame.list, 9))))
        return 1;
    invoke(s.frame.queue, 10, lists, arg(&s.frame.list));
    s.submitted = true;
    if (!fence_complete(s))
        return 1;
    s.submitted = false;
    return read_stream_target(s.desktop.Get(), s.frame, "pixels.bin");
}
extern "C" __declspec(dllexport) Word placed_close()
{
    if (!scene)
        return 0;
    auto& s = *scene;
    if (s.submitted && !fence_complete(s) && SUCCEEDED(s.desktop->GetDeviceRemovedReason()))
    {
        // The GPU may still own the pages. Keep them until this fixture process terminates.
        scene.release();
        return 1;
    }
    UINT released = 0;
    bool refs_zero = true;
    for (auto& b : s.buffers)
    {
        if (b.resource)
        {
            const auto refs = invoke(std::exchange(b.resource, nullptr), 2);
            refs_zero = refs_zero && !refs;
            ++released;
        }
    }
    if (s.trace)
        std::fprintf(s.trace, "close placed_resources=%u\n", released);
    scene.reset();
    return refs_zero ? 0 : 1;
}
