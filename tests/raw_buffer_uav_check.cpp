// resolve_raw_uav_store (src/raw_buffer_uav.h) without a device. vm_raw_uav_warp resolves two
// windows that are 4 GiB apart and inside their buffers. This check covers what that fixture leaves
// open: windows that touch (the first byte of one is the byte after the other), a window that ends
// at the last logical address, the widest window a 32-bit element count can describe, and each
// refusal of window metadata that does not fit its resource. Expected values are worked by hand.
// --mutate adjacent: expects the first byte of the upper touching window in the lower one; it must fail.
#include "raw_buffer_uav.h"

#include <cstdio>
#include <cstring>

namespace {
int failures;

struct Accept {
    const char* name;
    const RawUavWindow* windows;
    std::size_t count;
    UINT64 address;
    UINT bytes;
    std::size_t window;
    UINT offset;
    UINT64 first_element;
    UINT elements;
};

struct Refuse {
    const char* name;
    const RawUavWindow* windows;
    std::size_t count;
    UINT64 address;
    UINT bytes;
};

void accept(const Accept& c)
{
    RawUavStore got{};
    const HRESULT hr = resolve_raw_uav_store(c.windows, c.count, c.address, c.bytes, got);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC& v = got.view;
    if (hr != S_OK || got.window != c.window || got.offset != c.offset || v.Format != DXGI_FORMAT_R32_TYPELESS ||
        v.ViewDimension != D3D12_UAV_DIMENSION_BUFFER || v.Buffer.FirstElement != c.first_element ||
        v.Buffer.NumElements != c.elements || v.Buffer.StructureByteStride != 0 || v.Buffer.CounterOffsetInBytes != 0 ||
        v.Buffer.Flags != D3D12_BUFFER_UAV_FLAG_RAW) {
        std::printf("FAIL %s: hr=0x%08lx window=%zu offset=0x%x first=0x%llx elements=0x%x, expected S_OK window=%zu "
                    "offset=0x%x first=0x%llx elements=0x%x\n",
                    c.name, static_cast<unsigned long>(hr), got.window, got.offset,
                    static_cast<unsigned long long>(v.Buffer.FirstElement), v.Buffer.NumElements, c.window, c.offset,
                    static_cast<unsigned long long>(c.first_element), c.elements);
        ++failures;
    }
}

void refuse(const Refuse& c)
{
    RawUavStore got{17, 0xcafe, {}};
    const HRESULT hr = resolve_raw_uav_store(c.windows, c.count, c.address, c.bytes, got);
    if (hr != E_INVALIDARG || got.window != 17 || got.offset != 0xcafe || got.view.Buffer.NumElements != 0) {
        std::printf("FAIL %s: hr=0x%08lx window=%zu offset=0x%x, expected E_INVALIDARG and the result unchanged\n",
                    c.name, static_cast<unsigned long>(hr), got.window, got.offset);
        ++failures;
    }
}
} // namespace

int main(int argc, char** argv)
{
    const bool adjacent = argc == 3 && !std::strcmp(argv[1], "--mutate") && !std::strcmp(argv[2], "adjacent");
    if (argc != 1 && !adjacent) {
        std::printf("usage: raw_buffer_uav_check [--mutate adjacent]\n");
        return 64;
    }

    // Window 0 covers logical 0x10000..0x100ff from byte 0x40 of a 0x200-byte buffer: first element
    // 0x40 / 4 = 0x10, 0x100 / 4 = 0x40 elements. Window 1 starts at 0x10100, the byte after window 0,
    // and covers a whole 0x80-byte buffer: first element 0, 0x20 elements.
    const RawUavWindow touching[]{{0x10000, 0x100, 0x40, 0x200}, {0x10100, 0x80, 0, 0x80}};
    // 0xfffffffffffffff0..0xffffffffffffffff: the last 16 logical bytes, a window that does not wrap.
    const RawUavWindow top[]{{0xfffffffffffffff0ull, 16, 0, 16}};
    // 4 * 0xffffffff bytes is the widest window whose element count fits in 32 bits.
    const RawUavWindow widest[]{{0, 0x3fffffffcull, 0, 0x3fffffffcull}};
    // A window that ends at the last byte of its buffer: 0x10 + 0x30 = 0x40.
    const RawUavWindow flush[]{{0x20000, 0x30, 0x10, 0x40}};

    const Accept accepted[]{
        {"first byte of the upper touching window", touching, 2, 0x10100, 4, adjacent ? 0u : 1u, 0, 0, 0x20},
        {"last dword of the lower touching window", touching, 2, 0x100fc, 4, 0, 0xfc, 0x10, 0x40},
        {"last vector of the lower touching window", touching, 2, 0x100f0, 16, 0, 0xf0, 0x10, 0x40},
        {"last dword of the upper touching window", touching, 2, 0x1017c, 4, 1, 0x7c, 0, 0x20},
        {"last dword of the address space", top, 1, 0xfffffffffffffffcull, 4, 0, 0xc, 0, 4},
        {"vector at the top of the address space", top, 1, 0xfffffffffffffff0ull, 16, 0, 0, 0, 4},
        {"widest window, offset 0xfffffff0", widest, 1, 0xfffffff0ull, 16, 0, 0xfffffff0u, 0, 0xffffffffu},
        {"window flush with its buffer end", flush, 1, 0x2002c, 4, 0, 0x2c, 4, 0xc},
    };
    for (const Accept& c : accepted) accept(c);

    // Each window below is refused even though the store lies inside its logical range.
    const RawUavWindow odd_bytes[]{{0x1000, 0x22, 0, 0x40}};                      // 0x22 is not whole dwords
    const RawUavWindow too_wide[]{{0, 0x400000004ull, 0, 0x400000004ull}};        // 0x100000001 elements
    const RawUavWindow odd_offset[]{{0x2000, 0x10, 4, 0x40}};                     // first byte 4 is not 16-aligned
    const RawUavWindow past_width[]{{0x3000, 0x10, 0x50, 0x40}};                  // starts after the buffer
    const RawUavWindow overrun[]{{0x4000, 0x40, 0x10, 0x40}};                     // 0x10 + 0x40 > 0x40
    const RawUavWindow with_empty[]{{0, 0, 0, 0x40}, {0x6000, 0x10, 0, 0x10}};    // an empty window in the set
    const Refuse refused[]{
        {"window bytes not whole dwords", odd_bytes, 1, 0x1000, 4},
        {"element count above 32 bits", too_wide, 1, 0, 4},
        {"resource offset not 16-byte aligned", odd_offset, 1, 0x2000, 4},
        {"resource offset past the buffer", past_width, 1, 0x3000, 4},
        {"window runs past the buffer", overrun, 1, 0x4000, 4},
        {"empty window beside a valid one", with_empty, 2, 0x6000, 4},
    };
    for (const Refuse& c : refused) refuse(c);

    if (failures) {
        std::printf("raw_buffer_uav: %d FAILED\n", failures);
        return 1;
    }
    std::printf("raw_buffer_uav: PASS (8 resolved, 6 window metadata refusals)\n");
    return 0;
}
