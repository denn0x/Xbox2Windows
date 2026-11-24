#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

using Word = std::uint64_t;
Word invoke(void*, unsigned, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0, Word = 0);
namespace {
template<class T> Word ptr(T* value) { return reinterpret_cast<Word>(value); }
struct ViewAt {
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    Word address;
    Word tail[2];
};
static_assert(offsetof(ViewAt, address) == 0x28 && sizeof(ViewAt) == 0x40);

bool bind_view_table(void* guest, void* list, Word& cpu)
{
    struct Extended {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Word address, bytes;
    } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE}, 0, 0};
    void* heap = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 14, ptr(&heap_desc), ptr(&__uuidof(ID3D12DescriptorHeap)), ptr(&heap))))) return false;
    cpu = invoke(heap, 9);
    invoke(list, 32, 1, ptr(&heap));
    invoke(list, 36, 0, invoke(heap, 10));
    return true;
}
}

// Two separately placed R8 planes have the same descriptor and different row
// data. The producer and oracle both use the documented desktop footprint.
int draw_address_srvs(ID3D12Device* desktop, void* guest, void* list)
{
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1,
        DXGI_FORMAT_R8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0, bytes = 0;
    desktop->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &bytes);
    if (rows != 2 || row_bytes != 2 || bytes < footprint.Footprint.RowPitch + 2) return 40;
    std::array<std::vector<unsigned char>, 2> planes;
    void* placed[2]{};
    for (unsigned i = 0; i < planes.size(); ++i) {
        planes[i].resize(static_cast<std::size_t>(bytes));
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < row_bytes; ++x)
                planes[i][y * footprint.Footprint.RowPitch + x] = static_cast<unsigned char>(16 + 128 * i + 32 * y + 7 * x);
        if (FAILED(static_cast<HRESULT>(invoke(guest, 133, ptr(planes[i].data()), ptr(&desc), D3D12_RESOURCE_STATE_COMMON,
                                               0, ptr(&__uuidof(ID3D12Resource)), ptr(&placed[i]))))) return 41;
    }
    Word cpu = 0;
    if (!bind_view_table(guest, list, cpu)) return 42;
    ViewAt v{};
    v.view.Format = DXGI_FORMAT_R8_UNORM;
    v.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.view.Texture2D.MipLevels = 1;
    const auto draw = [&](LONG x) {
        const D3D12_RECT strip{x, 0, x + 1, 4};
        invoke(list, 22, 1, ptr(&strip));
        invoke(list, 12, 3, 1);
    };
    const auto publish = [&](const D3D12_RESOURCE_DESC& resource_desc, Word address) {
        v.address = address;
        invoke(guest, 70, ptr(&resource_desc), ptr(&v), cpu);
    };
    invoke(guest, 18, ptr(placed[0]), ptr(&v.view), cpu);
    draw(0); // Ordinary resource-object SRV is the independent control.
    publish(desc, ptr(planes[1].data())); draw(1);
    publish(desc, ptr(planes[0].data())); draw(2);
    publish(desc, 0); draw(3); // Rejection preserves the previous descriptor.
    publish(desc, ptr(planes[1].data())); draw(4);
    auto wrong_desc = desc;
    wrong_desc.Width = 1;
    publish(wrong_desc, ptr(planes[0].data())); draw(5);
    publish(desc, ptr(planes[0].data()) + 1); // Interior address, not the registered base.
    // Refusals that name their cause in the log; draw 6 shows each left the descriptor alone.
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 256;
    buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ViewAt buffer_view{};
    buffer_view.view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    buffer_view.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    buffer_view.view.Buffer.NumElements = 64;
    buffer_view.view.Buffer.StructureByteStride = 4;
    buffer_view.address = ptr(planes[0].data());
    invoke(guest, 70, ptr(&buffer_desc), ptr(&buffer_view), cpu);
    v.address = ptr(planes[0].data());
    invoke(guest, 70, ptr(&desc), ptr(&v), cpu + 1); // Not the start of a descriptor in the view heap.
    draw(6);
    publish(desc, ptr(planes[0].data())); draw(7);
    return 0;
}

int draw_bc_address_srvs(void* guest, void* list)
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"X2W_TEST_BC_VIEW", path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 43;
    const bool bc3 = GetEnvironmentVariableW(L"X2W_TEST_BC3", nullptr, 0) != 0;
    constexpr UINT tile = 4096, width = 128, height = 128;
    alignas(4096) static std::array<std::array<char, 4 * tile>, 2> backing{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(backing[0].data(), 4 * tile) || input.peek() != std::char_traits<char>::eof()) return 43;
    for (UINT i = 0; i < 4; ++i)
        std::memcpy(backing[1].data() + i * tile, backing[0].data() + (i ^ 1) * tile, tile);
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        bc3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC7_UNORM, {1, 0}, static_cast<D3D12_TEXTURE_LAYOUT>(0x105),
        bc3 ? D3D12_RESOURCE_FLAG_NONE : static_cast<D3D12_RESOURCE_FLAGS>(0x300000)};
    ViewAt v{};
    v.view.Format = desc.Format;
    v.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.view.Texture2D.MipLevels = 1;
    Word cpu = 0;
    if (!bind_view_table(guest, list, cpu)) return 42;
    const Word addresses[] = {ptr(backing[0].data()), ptr(backing[1].data()), 0, ptr(backing[0].data())};
    for (LONG i = 0; i < 4; ++i) {
        v.address = addresses[i];
        invoke(guest, 70, ptr(&desc), ptr(&v), cpu);
        const D3D12_RECT region{i * LONG(width), 0, (i + 1) * LONG(width), LONG(height)};
        invoke(list, 22, 1, ptr(&region));
        invoke(list, 12, 3, 1);
    }
    return 0;
}

// The backing is produced by XG CopyIntoSubresource from a source-built block texture.
int draw_bc_chain(void* guest, void* list)
{
    wchar_t path[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"X2W_TEST_BC_CHAIN", path, MAX_PATH);
    if (!size || size >= MAX_PATH) return 43;
    const bool array = GetEnvironmentVariableW(L"X2W_TEST_BC_ARRAY", nullptr, 0) != 0;
    const bool rectangular = GetEnvironmentVariableW(L"X2W_TEST_BC_RECT", nullptr, 0) != 0;
    const UINT height = rectangular ? 512 : 1024;
    const UINT16 slices = array ? 2 : 1;
    const UINT16 mips = array ? 9 : 6;
    const UINT bytes = slices * (rectangular ? 0xc0000 : 0x160000);
    static std::array<char, 2 * 0x160000 + 0xffff> storage{};
    char* backing = reinterpret_cast<char*>((ptr(storage.data()) + 0xffff) & ~Word{0xffff});
    std::ifstream input(path, std::ios::binary);
    if (!input.read(backing, bytes) || input.peek() != std::char_traits<char>::eof()) return 43;
    const bool bc5 = GetEnvironmentVariableW(L"X2W_TEST_BC5", nullptr, 0) != 0;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1024, height, slices, mips,
        bc5 ? DXGI_FORMAT_BC5_UNORM : DXGI_FORMAT_BC7_UNORM, {1, 0}, static_cast<D3D12_TEXTURE_LAYOUT>(0x109),
        D3D12_RESOURCE_FLAG_NONE};
    Word cpu = 0;
    if (!bind_view_table(guest, list, cpu)) return 42;
    ViewAt v{};
    v.address = ptr(backing);
    v.view.Format = desc.Format;
    v.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (array) {
        v.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        v.view.Texture2DArray.ArraySize = slices;
        v.view.Texture2DArray.MipLevels = mips;
    } else {
        v.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.view.Texture2D.MipLevels = mips;
    }
    invoke(guest, 70, ptr(&desc), ptr(&v), cpu);
    invoke(list, 12, 3, 1);
    return 0;
}

namespace {
struct PlacedPair {
    std::array<DWORD*, 2> memory{};
    std::array<void*, 2> resource{};
    bool rebind{};
} placed_pair;
}

int draw_placed_vertices(ID3D12Device* desktop, void* guest, void* list)
{
    auto& p = placed_pair;
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    if (FAILED(desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) || !heaps.Supported)
        return 77;
    if (!p.memory[0]) {
        constexpr auto flags = MEM_RESERVE | MEM_COMMIT;
        for (Word base = 0x1600000000ull; base < 0x1700000000ull; base += 16ull << 20) {
            auto* low = static_cast<DWORD*>(VirtualAlloc(reinterpret_cast<void*>(base), 65536, flags, PAGE_READWRITE));
            if (!low) continue;
            auto* high = static_cast<DWORD*>(VirtualAlloc(reinterpret_cast<void*>(base + (1ull << 40)), 65536, flags, PAGE_READWRITE));
            if (high) { p.memory = {low, high}; break; }
            VirtualFree(low, 0, MEM_RELEASE);
        }
        if (!p.memory[0]) return 45;
    }
    for (UINT i = 0; i < 2; ++i) {
        if (p.resource[i]) continue;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = p.rebind && i ? 516 : 260;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto hr = static_cast<HRESULT>(invoke(guest, 133, ptr(p.memory[i]), ptr(&desc),
            D3D12_RESOURCE_STATE_COMMON, 0, ptr(&__uuidof(ID3D12Resource)), ptr(&p.resource[i])));
        if (FAILED(hr) || !p.resource[i] || invoke(p.resource[i], 11) != ptr(p.memory[i])) return 46;
    }
    const UINT hi = p.rebind ? 128 : 64;
    p.memory[0][64] = 10; p.memory[1][hi] = 20;
    auto draw = [&](LONG column, DWORD* word, UINT bytes) {
        const D3D12_VERTEX_BUFFER_VIEW view{ptr(word), bytes, 4};
        invoke(list, 48, 0, 1, ptr(&view));
        const D3D12_RECT rect{column, 0, column + 1, 4};
        invoke(list, 22, 1, ptr(&rect));
        invoke(list, 12, 3, 1);
    };
    for (LONG col = 0; col < 8; ++col) {
        switch (col % 4) {
        case 0: draw(col, p.memory[0] + 64, 4); break;
        case 1: draw(col, p.memory[1] + hi, 4); break;
        case 2: draw(col, p.memory[1] + hi, 8); break; // Reject before requesting data; shader only loads byte 0.
        case 3: draw(col, p.rebind ? p.memory[1] + 64 : p.memory[0] + 64, 4); break;
        }
    }
    // Legitimate writes to the last DWORD of each buffer, before queue submission.
    // A snapshot made when recording the list would publish the previous values.
    p.memory[0][64] = p.rebind ? 49 : 41;
    p.memory[1][hi] = p.rebind ? 153 : 135;
    if (p.rebind) p.memory[1][64] = 179; // Reused address overlaps the retired smaller registration.
    return 0;
}

void release_placed_vertices(bool done)
{
    auto& p = placed_pair;
    invoke(p.resource[1], 2);
    p.resource[1] = nullptr;
    p.rebind = true;
    if (done) {
        invoke(p.resource[0], 2);
        p.resource[0] = nullptr;
    }
}

int draw_address_buffer_srvs(ID3D12Device* desktop, void* guest, void* list)
{
    static std::array<std::uint8_t, 65536 + 256> raw_buf0{};
    static std::array<std::uint8_t, 65536 + 256> raw_buf1{};
    auto* const buf0 = reinterpret_cast<std::uint32_t*>((ptr(raw_buf0.data()) + 0xffff) & ~Word{0xffff});
    auto* const buf1 = reinterpret_cast<std::uint32_t*>((ptr(raw_buf1.data()) + 0xffff) & ~Word{0xffff});
    std::memset(buf0, 0, 256);
    std::memset(buf1, 0, 256);
    buf0[0] = 17;
    buf0[1] = 99;
    buf0[4] = 63;
    buf1[4] = 141;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    void* placed[2]{};
    if (FAILED(static_cast<HRESULT>(invoke(guest, 133, ptr(buf0), ptr(&desc), D3D12_RESOURCE_STATE_COMMON,
                                           0, ptr(&__uuidof(ID3D12Resource)), ptr(&placed[0]))))) return 47;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 133, ptr(buf1), ptr(&desc), D3D12_RESOURCE_STATE_COMMON,
                                           0, ptr(&__uuidof(ID3D12Resource)), ptr(&placed[1]))))) return 47;

    Word cpu = 0;
    if (!bind_view_table(guest, list, cpu)) return 42;

    const auto draw = [&](LONG x) {
        const D3D12_RECT strip{x, 0, x + 1, 4};
        invoke(list, 22, 1, ptr(&strip));
        invoke(list, 12, 3, 1);
    };

    ViewAt raw{};
    raw.view.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    raw.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    raw.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    raw.view.Buffer.NumElements = 4;

    raw.address = ptr(buf0);
    invoke(guest, 70, ptr(&desc), ptr(&raw), cpu);
    draw(0);

    raw.address = ptr(buf0 + 4);
    invoke(guest, 70, ptr(&desc), ptr(&raw), cpu);
    draw(1);

    raw.address = ptr(buf1);
    raw.view.Buffer.FirstElement = 4;
    invoke(guest, 70, ptr(&desc), ptr(&raw), cpu);
    draw(2);

    // Unaligned interior address for a raw buffer must be refused and keep the previous descriptor.
    raw.view.Buffer.FirstElement = 0;
    raw.address = ptr(buf0 + 1);
    invoke(guest, 70, ptr(&desc), ptr(&raw), cpu);
    draw(3);

    ViewAt structured{};
    structured.view.Format = DXGI_FORMAT_UNKNOWN;
    structured.view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    structured.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    structured.view.Buffer.FirstElement = 1;
    structured.view.Buffer.NumElements = 1;
    structured.view.Buffer.StructureByteStride = 4;
    structured.address = ptr(buf0);
    invoke(guest, 70, ptr(&desc), ptr(&structured), cpu);
    // An acceleration-structure view is refused; draw(4) proves the structured descriptor stayed in place.
    ViewAt as_view{};
    as_view.view.Format = DXGI_FORMAT_UNKNOWN;
    as_view.view.ViewDimension = static_cast<D3D12_SRV_DIMENSION>(11);
    as_view.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as_view.address = ptr(buf0);
    invoke(guest, 70, ptr(&desc), ptr(&as_view), cpu);
    draw(4);

    raw.address = ptr(buf0);
    raw.view.Buffer.FirstElement = 0;
    invoke(guest, 70, ptr(&desc), ptr(&raw), cpu);
    draw(5);
    draw(6);
    draw(7);
    return 0;
}
