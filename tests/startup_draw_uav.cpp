// Translated draws with UAV stores and resource-heap reads (tests/startup_draw_uav_check.py). Title buffers
// are private allocations: the stand-in opens them as desktop heaps, so the stores must reach these pages.
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <vector>

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
void* view_heap(void* guest, void* backing, UINT entries = 1)
{
    struct Extended {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Word address, bytes;
    } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, entries, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE}, ptr(backing), backing ? 65536u : 0u};
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

// LOCAL (textures) mode heaptex: entry 0 is a shader-resource view of a plain 8x4 RGBA8 texture that no table
// binds (only the heap references it); its title memory holds texel (x, y) = (10 + 20x, 30 + 40y, 7, 255) in the
// desktop footprint (row pitch 256).
// With X2W_TEST_HEAPTEX_SURFACE=<path> (tests/gfx10_layout_check.cpp --write-tiled bc1_256_9mips_4k_s): entry 0 is
// instead a memory SRV (device slot 70) of that console-swizzled surface: BC1 256x256, 9 mips, layout 0x105, flags
// 0x300000, not announced (X2W_MEMORY_SRV_DETILE).
int bind_heap_memory_entry(void* guest, Word cpu, const wchar_t* path)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    if (bytes.empty()) return 43;
    void* memory = VirtualAlloc(nullptr, bytes.size() + (1u << 20), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) return 45;
    std::memcpy(memory, bytes.data(), bytes.size());
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 9, DXGI_FORMAT_BC1_UNORM, {1, 0},
        static_cast<D3D12_TEXTURE_LAYOUT>(0x105), static_cast<D3D12_RESOURCE_FLAGS>(0x300000)};
    struct ViewAt {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        Word address;
        Word tail[2];
    } v{};
    static_assert(offsetof(ViewAt, address) == 0x28 && sizeof(ViewAt) == 0x40);
    v.view.Format = desc.Format;
    v.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.view.Texture2D.MipLevels = 9;
    v.address = ptr(memory);
    invoke(guest, 70, ptr(&desc), ptr(&v), cpu);
    return 0;
}

int bind_heap_texture_entry(void* guest, Word cpu)
{
    wchar_t surface[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"X2W_TEST_HEAPTEX_SURFACE", surface, MAX_PATH);
    if (length && length < MAX_PATH) return bind_heap_memory_entry(guest, cpu, surface);
    auto* memory = reinterpret_cast<unsigned char*>(private_pages());
    if (!memory) return 45;
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            unsigned char* t = memory + y * 256 + x * 4;
            t[0] = static_cast<unsigned char>(10 + 20 * x);
            t[1] = static_cast<unsigned char>(30 + 40 * y);
            t[2] = 7;
            t[3] = 255;
        }
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    void* texture = placed(guest, memory, desc);
    if (!texture) return 46;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Texture2D.MipLevels = 1;
    invoke(guest, 18, ptr(texture), ptr(&srv), cpu);
    return 0;
}

// LOCAL (textures) modes membuf / membuf-off / membuf-link (X2W_BUFFER_HEAP_INDEX, tests/startup_buffer_heap_index_check.py).
// Slots are `increment` bytes apart (the desktop increment: 32 like the console, 64 on WARP), five of them. The heap's
// descriptor memory starts as the title's free list (slot M holds M + 1, as run 87 measured). Slot 1 is a decoy: a
// raw buffer view of a placed buffer whose first dword is 55 (slot N of the link 1, never to be bound).
// membuf / membuf-off: slot 0 is a memory SRV (device slot 70) of private memory no resource covers, first dword 99.
// membuf-link: slot 0 stays free. Then the title copies slot 0 to byte 128 of the heap (slot 4, or 2 on WARP) with its
// own memory copy; the draw's table entry is that slot (`srv 0 buf table 0 128`).
int bind_memory_buffer_entries(void* guest, Word cpu, UINT increment)
{
    if (increment < 32 || 128 % increment) return 48;
    auto* heap_bytes = reinterpret_cast<unsigned char*>(cpu);
    for (std::uint32_t m = 0; m < 5; ++m) {
        const std::uint32_t link = m + 1;
        std::memset(heap_bytes + m * increment, 0, increment);
        std::memcpy(heap_bytes + m * increment, &link, 4);
    }
    std::uint32_t* decoy = private_pages();
    std::uint32_t* memory = private_pages();
    if (!decoy || !memory) return 45;
    decoy[0] = 55;
    memory[0] = 99;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* buffer = placed(guest, decoy, desc);
    if (!buffer) return 47;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer.NumElements = 64;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    invoke(guest, 18, ptr(buffer), ptr(&srv), cpu + increment);
    if (!mode_is(L"membuf-link")) {
        struct ViewAt {
            D3D12_SHADER_RESOURCE_VIEW_DESC view;
            Word address;
            Word tail[2];
        } v{};
        static_assert(offsetof(ViewAt, address) == 0x28 && sizeof(ViewAt) == 0x40);
        v.view = srv;
        v.address = mode_is(L"membuf-refused") ? 0 : ptr(memory);  // membuf-refused: null address, refused null_address
        if (mode_is(L"membuf-structured")) {  // as run 109's slot 0x33: structured, one record (here 4 of 16 bytes)
            v.view.Format = DXGI_FORMAT_UNKNOWN;
            v.view.Buffer.NumElements = 4;
            v.view.Buffer.StructureByteStride = 16;
            v.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        } else if (mode_is(L"membuf-typed")) {  // as run 109's first logged refusals (typed; R32_UINT here)
            v.view.Format = DXGI_FORMAT_R32_UINT;
            v.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        }
        invoke(guest, 70, ptr(&desc), ptr(&v), cpu);
    }
    std::memcpy(heap_bytes + 128, heap_bytes, 32);
    std::uint32_t copied[4];
    std::memcpy(copied, heap_bytes + 128, sizeof copied);
    std::printf("membuf entry words: %08x %08x %08x %08x\n", copied[0], copied[1], copied[2], copied[3]);
    std::fflush(stdout);
    return 0;
}

// A 64 KiB placed buffer over `memory` (title memory of a resource for CopyBufferRegion).
void* memory_buffer(void* guest, void* memory)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 65536;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return placed(guest, memory, desc);
}
void hardware_descriptor(Word cpu, Word base, std::uint32_t stride = 0)
{
    // gfx10 buffer descriptor: base [47:0], stride [61:48] (0 unless given), 256 records, word 3 a marker the shader
    // checks. With a stride the record count is in records: 16 records of `stride` bytes.
    const std::uint32_t words[8] = {static_cast<std::uint32_t>(base), (static_cast<std::uint32_t>(base >> 32) & 0xffff) | stride << 16,
                                    stride ? 16u : 256u, 0x12345678u};
    std::memcpy(reinterpret_cast<void*>(cpu), words, sizeof(words));
}
void* copy_dest;
void* copy_source;
Word copy_dest_offset, copy_source_offset;
std::uint32_t* copy_memory;  // allocation of the copy destination
}

// X2W_TEST_DRAW_UAV=<mode>. aligned|misaligned|uavbase: entry 0 is a hardware buffer descriptor at title + 64
// or + 68 of a private allocation. Two draws, one column each.
int draw_uav_stores(ID3D12Device* desktop, void* guest, void* list)
{
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    if (FAILED(desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) || !heaps.Supported) return 77;
    GetEnvironmentVariableW(L"X2W_TEST_DRAW_UAV", uav_mode, 32);
    const bool heap_texture_mode = mode_is(L"heaptex");  // LOCAL (textures)
    const bool membuf_mode = std::wcsncmp(uav_mode, L"membuf", 6) == 0;  // LOCAL (textures)
    const bool heap_mode = mode_is(L"heap") || heap_texture_mode || membuf_mode;
    // Heap draws need the console's 32-byte view slots (the title's own shader arithmetic).
    std::printf("view increment %u\n", desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    std::fflush(stdout);
    const bool copy_mode = std::wcsncmp(uav_mode, L"copy", 4) == 0;
    void* heap = view_heap(guest, heap_mode ? private_pages() : nullptr, membuf_mode ? 5 : copy_mode ? 2 : 1);
    if (!heap) return 42;
    const Word cpu = invoke(heap, 9);
    if (!cpu) return 42;
    if (membuf_mode) {
        if (const int rc = bind_memory_buffer_entries(guest, cpu, desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)))
            return rc;
    } else if (heap_texture_mode) {
        if (const int rc = bind_heap_texture_entry(guest, cpu)) return rc;
    } else if (std::wcsncmp(uav_mode, L"tex", 3) == 0) {
        if (const int rc = bind_texture_entry(guest, cpu)) return rc;
    } else if (heap_mode) {
        if (const int rc = bind_heap_entry(guest, cpu)) return rc;
    } else if (copy_mode) {
        // copy-*: entry 0 (uav 0) = title + 64 of allocation A, entry 1 (srv 0) = title + 0x400 of A (copy-same) or
        // of a second allocation B. Between the two draws the list copies A + 64 (128 bytes) to the srv range.
        uav_memory = private_pages();
        std::uint32_t* other = mode_is(L"copy-two") ? private_pages() : uav_memory;
        if (!uav_memory || !other) return 45;
        copy_memory = other;
        copy_source = memory_buffer(guest, uav_memory);
        copy_dest = other == uav_memory ? copy_source : memory_buffer(guest, other);
        if (!copy_source || !copy_dest) return 47;
        copy_source_offset = 64;
        copy_dest_offset = 0x400;
        hardware_descriptor(cpu, ptr(uav_memory) + 64);
        hardware_descriptor(cpu + 32, ptr(other) + 0x400);
    } else {
        uav_memory = private_pages();
        if (!uav_memory) return 45;
        // LOCAL pstrans: stride16 / stride0 (udsrc uavstride): title + 64 with a record stride of 16 or 0 bytes.
        const bool stride_mode = std::wcsncmp(uav_mode, L"stride", 6) == 0;
        hardware_descriptor(cpu, ptr(uav_memory) + (mode_is(L"aligned") || stride_mode ? 64 : 68), mode_is(L"stride16") ? 16u : 0u);
    }
    invoke(list, 32, 1, ptr(&heap));
    invoke(list, 36, 0, invoke(heap, 10));
    // copy-twice: three strips; a second copy after draw 2 takes A + 68 (what draw 2 stored), and draw 3 reads it.
    const LONG strips = mode_is(L"copy-twice") ? 3 : 2;
    for (LONG x = 0; x < strips; ++x) {
        const D3D12_RECT strip{x, 0, x + 1, 4};
        invoke(list, 22, 1, ptr(&strip));
        invoke(list, 12, 3, 1);
        // CopyBufferRegion(dest, dest offset, source, source offset, bytes); copy-none: no copy (negative control)
        if (x < strips - 1 && copy_dest && !mode_is(L"copy-none"))
            invoke(list, 15, ptr(copy_dest), copy_dest_offset, ptr(copy_source), copy_source_offset + 4 * x, 128);
    }
    return 0;
}

// After the list completed: the title memory as the CPU sees it.
void report_uav_stores()
{
    if (!uav_memory) return;
    std::printf("uav memory:");
    for (int i = 0; i < 64; ++i) std::printf(" %u", uav_memory[i]);
    if (copy_dest) {  // copy-*: the copied range as the CPU sees it (title + 0x400 of the destination allocation)
        const auto* copied = copy_memory + 0x100;
        std::printf("\ncopied:");
        for (int i = 0; i < 32; ++i) std::printf(" %u", copied[i]);
    }
    std::printf("\n");
    std::fflush(stdout);
}
