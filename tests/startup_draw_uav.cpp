// Translated draws with UAV stores and resource-heap reads (tests/startup_draw_uav_check.py). Title buffers
// are private allocations: the stand-in opens them as desktop heaps, so the stores must reach these pages.
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

using Word = std::uint64_t;
Word invoke(void*, unsigned, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0);
namespace {
template<class T> Word ptr(T* value) { return reinterpret_cast<Word>(value); }
std::uint32_t* uav_memory;
wchar_t uav_mode[32];

bool mode_is(const wchar_t* name) { return std::wcscmp(uav_mode, name) == 0; }

std::uint32_t* private_pages()
{
    return static_cast<std::uint32_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

// A one-entry shader-visible view heap whose descriptor memory is `backing` (0: the stand-in's own).
void* view_heap(void* guest, void* backing)
{
    struct Extended {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Word address, bytes;
    } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE}, ptr(backing), backing ? 65536u : 0u};
    void* heap = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 14, ptr(&heap_desc), ptr(&__uuidof(ID3D12DescriptorHeap)), ptr(&heap))))) return nullptr;
    return heap;
}

void* placed(void* guest, void* memory, const D3D12_RESOURCE_DESC& desc)
{
    void* resource = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 133, ptr(memory), ptr(&desc), D3D12_RESOURCE_STATE_COMMON, 0,
                                           ptr(&__uuidof(ID3D12Resource)), ptr(&resource))))) return nullptr;
    return resource;
}

// Modes tex, tex-noflag, tex-uint: entry 0 is a view of an 8x4 texture (UAV-capable RGBA8, a plain RGBA8 texture
// with a shader-resource view, or an RGBA8 typeless texture with an R8G8B8A8_UINT UAV).
int bind_texture_entry(void* guest, Word cpu)
{
    const bool uav_flag = !mode_is(L"tex-noflag");
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 4, 1, 1,
        mode_is(L"tex-uint") ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        uav_flag ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
    void* texture = placed(guest, private_pages(), desc);
    if (!texture) return 46;
    if (uav_flag) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{mode_is(L"tex-uint") ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_UAV_DIMENSION_TEXTURE2D};
        invoke(guest, 19, ptr(texture), 0, ptr(&uav), cpu);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        srv.Texture2D.MipLevels = 1;
        invoke(guest, 18, ptr(texture), ptr(&srv), cpu);
    }
    return 0;
}

// Mode heap: the view heap's memory is a private allocation (read in place as heapmem); entry 0 is a raw
// buffer view of a placed buffer whose first dword is 77.
int bind_heap_entry(void* guest, Word cpu)
{
    std::uint32_t* memory = private_pages();
    if (!memory) return 45;
    memory[0] = 77;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* buffer = placed(guest, memory, desc);
    if (!buffer) return 47;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer.NumElements = 64;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    invoke(guest, 18, ptr(buffer), ptr(&srv), cpu);
    return 0;
}
}

// X2W_TEST_DRAW_UAV=<mode>. aligned|misaligned|uavbase: entry 0 is a hardware buffer descriptor at title + 64
// or + 68 of a private allocation. Two draws, one column each.
int draw_uav_stores(ID3D12Device* desktop, void* guest, void* list)
{
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    if (FAILED(desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) || !heaps.Supported) return 77;
    GetEnvironmentVariableW(L"X2W_TEST_DRAW_UAV", uav_mode, 32);
    const bool heap_mode = mode_is(L"heap");
    // Heap draws need the console's 32-byte view slots (the title's own shader arithmetic).
    std::printf("view increment %u\n", desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    std::fflush(stdout);
    void* heap = view_heap(guest, heap_mode ? private_pages() : nullptr);
    if (!heap) return 42;
    const Word cpu = invoke(heap, 9);
    if (!cpu) return 42;
    if (std::wcsncmp(uav_mode, L"tex", 3) == 0) {
        if (const int rc = bind_texture_entry(guest, cpu)) return rc;
    } else if (heap_mode) {
        if (const int rc = bind_heap_entry(guest, cpu)) return rc;
    } else {
        uav_memory = private_pages();
        if (!uav_memory) return 45;
        const Word target = ptr(uav_memory) + (mode_is(L"aligned") ? 64 : 68);
        // gfx10 buffer descriptor: base [47:0], stride 0, 256 records (bytes), word 3 a marker the shader checks.
        const std::uint32_t words[8] = {static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(target >> 32) & 0xffff, 256, 0x12345678u};
        std::memcpy(reinterpret_cast<void*>(cpu), words, sizeof(words));
    }
    invoke(list, 32, 1, ptr(&heap));
    invoke(list, 36, 0, invoke(heap, 10));
    for (LONG x = 0; x < 2; ++x) {
        const D3D12_RECT strip{x, 0, x + 1, 4};
        invoke(list, 22, 1, ptr(&strip));
        invoke(list, 12, 3, 1);
    }
    return 0;
}

// After the list completed: the title memory as the CPU sees it.
void report_uav_stores()
{
    if (!uav_memory) return;
    std::printf("uav memory:");
    for (int i = 0; i < 64; ++i) std::printf(" %u", uav_memory[i]);
    std::printf("\n");
    std::fflush(stdout);
}
