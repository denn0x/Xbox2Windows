// Checks src/startup_stubs/bc7_4k_s.h without a device: the descriptions supports_bc7_4k_s accepts
// and refuses, Bc16Surface's padded sizes, every block offset on surfaces with partial and
// non-square tile grids, and detile into a pitched destination with guard bytes.
//
// The expected offsets come from the documented bit order, written as a table of (axis, bit)
// pairs, and from literal offsets worked out by hand; neither reuses the header's shifts and masks.
#include "startup_stubs/bc7_4k_s.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures;

using x2w_startup::Bc16Surface;

struct Bit {
    char axis;
    unsigned bit;
};

// Block index bits from low to high: y0 y1 x0 x1 y2 x2 y3 x3.
constexpr Bit documented[8] = {{'y', 0}, {'y', 1}, {'x', 0}, {'x', 1}, {'y', 2}, {'x', 2}, {'y', 3}, {'x', 3}};
// --mutate x-first: a plain x-first Morton order (negative control: the check must then fail).
constexpr Bit x_first[8] = {{'x', 0}, {'y', 0}, {'x', 1}, {'y', 1}, {'x', 2}, {'y', 2}, {'x', 3}, {'y', 3}};
const Bit* order = documented;

std::uint64_t expected_offset(UINT column, UINT row, UINT columns)
{
    const UINT tile_columns = (columns + 15) / 16;
    const UINT x = column % 16, y = row % 16;
    UINT block = 0;
    for (UINT position = 0; position < 8; ++position)
        block |= (((order[position].axis == 'x' ? x : y) >> order[position].bit) & 1u) << position;
    return (std::uint64_t{row / 16} * tile_columns + column / 16) * 4096 + block * 16;
}

void fail(const char* name, const char* format, ...)
{
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    ++failures;
}

unsigned long long u64(std::uint64_t value) { return value; }

void block_tag(unsigned char* out, UINT x, UINT y)
{
    out[0] = static_cast<unsigned char>(x);
    out[1] = static_cast<unsigned char>(x >> 8);
    out[2] = static_cast<unsigned char>(y);
    out[3] = static_cast<unsigned char>(y >> 8);
    for (UINT i = 4; i < 16; ++i) out[i] = static_cast<unsigned char>(0x40 + i + x * 3 + y * 5);
}

// Sizes, every block offset, and detile through padded row pitches with guards on both ends.
void surface(const char* name, UINT width, UINT height, UINT columns, UINT rows, UINT padded_width,
             UINT padded_height, UINT row_bytes, std::uint64_t size, bool full = true)
{
    const int before = failures;
    const Bc16Surface s(width, height);
    if (s.columns != columns || s.rows != rows) fail(name, "blocks %ux%u, expected %ux%u", s.columns, s.rows, columns, rows);
    if (s.padded_width() != padded_width || s.padded_height() != padded_height)
        fail(name, "padded %ux%u, expected %ux%u", s.padded_width(), s.padded_height(), padded_width, padded_height);
    if (s.padded_row_bytes() != row_bytes) fail(name, "padded row bytes %u, expected %u", s.padded_row_bytes(), row_bytes);
    if (s.size() != size) fail(name, "size %llu, expected %llu", u64(s.size()), u64(size));
    if (!full || failures != before) return;

    std::vector<unsigned char> seen(static_cast<std::size_t>(size / 16));
    for (UINT y = 0; y < rows; ++y)
        for (UINT x = 0; x < columns; ++x) {
            const auto offset = s.block_offset(x, y), want = expected_offset(x, y, columns);
            if (offset != want) {
                fail(name, "block (%u, %u) at 0x%llx, expected 0x%llx", x, y, u64(offset), u64(want));
                return;
            }
            if (offset + 16 > size || seen[static_cast<std::size_t>(offset / 16)]++) {
                fail(name, "block (%u, %u) outside the surface or reused at 0x%llx", x, y, u64(offset));
                return;
            }
        }

    // Tiled source: caller padding 0xff, each visible block tagged with its own coordinates.
    std::vector<unsigned char> tiled(static_cast<std::size_t>(size), 0xff);
    for (UINT y = 0; y < rows; ++y)
        for (UINT x = 0; x < columns; ++x)
            block_tag(&tiled[static_cast<std::size_t>(expected_offset(x, y, columns))], x, y);
    const auto source = tiled;

    for (UINT extra : {0u, 48u, 256u}) {
        const UINT pitch = columns * 16 + extra;
        const std::size_t guard = 64, span = std::size_t{rows - 1} * pitch + columns * 16;
        std::vector<unsigned char> dest(guard + std::size_t{rows} * pitch + guard, 0xa5);
        s.detile(tiled.data(), dest.data() + guard, pitch);
        if (tiled != source) fail(name, "detile wrote its source (pitch %u)", pitch);
        for (std::size_t i = 0; i < dest.size(); ++i) {
            const bool inside = i >= guard && i < guard + span && (i - guard) % pitch < columns * 16;
            if (inside) {
                const std::size_t at = i - guard;
                const UINT y = static_cast<UINT>(at / pitch), x = static_cast<UINT>(at % pitch / 16);
                unsigned char want[16];
                block_tag(want, x, y);
                if (dest[i] != want[at % 16]) {
                    fail(name, "detile pitch %u: block (%u, %u) wrong", pitch, x, y);
                    break;
                }
            } else if (dest[i] != 0xa5) {
                fail(name, "detile pitch %u wrote a guard or row-gap byte at %lld", pitch,
                     static_cast<long long>(i) - static_cast<long long>(guard));
                break;
            }
        }
    }
}

void offset(const char* name, UINT width, UINT height, UINT column, UINT row, std::uint64_t want)
{
    const auto got = Bc16Surface(width, height).block_offset(column, row);
    if (got != want) fail(name, "0x%llx, expected 0x%llx", u64(got), u64(want));
}

D3D12_RESOURCE_DESC bc7(UINT64 width = 81, UINT height = 69)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_BC7_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

void accepted(const char* name, const D3D12_RESOURCE_DESC& desc, bool want)
{
    if (x2w_startup::supports_bc7_4k_s(desc) != want) fail(name, want ? "refused, expected accepted" : "accepted, expected refused");
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "x-first") == 0) order = x_first;

    // Hand-worked offsets: each bit of the order alone, then the tile index on 4-by-2 and 2-by-4
    // tile grids (200x72 and 72x200), where a row-major index using the tile row count would differ.
    offset("block (0, 1): y0", 64, 64, 0, 1, 0x10);
    offset("block (0, 2): y1", 64, 64, 0, 2, 0x20);
    offset("block (1, 0): x0", 64, 64, 1, 0, 0x40);
    offset("block (2, 0): x1", 64, 64, 2, 0, 0x80);
    offset("block (0, 4): y2", 64, 64, 0, 4, 0x100);
    offset("block (4, 0): x2", 64, 64, 4, 0, 0x200);
    offset("block (0, 8): y3", 64, 64, 0, 8, 0x400);
    offset("block (8, 0): x3", 64, 64, 8, 0, 0x800);
    offset("block (15, 15)", 64, 64, 15, 15, 0xff0);
    offset("block (5, 3)", 64, 64, 5, 3, 0x270);
    offset("200x72 block (16, 0): tile 1", 200, 72, 16, 0, 0x1000);
    offset("200x72 block (0, 16): tile 4", 200, 72, 0, 16, 0x4000);
    offset("200x72 block (49, 17): tile 7", 200, 72, 49, 17, 0x7050);
    offset("72x200 block (0, 16): tile 2", 72, 200, 0, 16, 0x2000);
    offset("72x200 block (17, 49): tile 7", 72, 200, 17, 49, 0x7050);
    offset("16384x16384 last block", 16384, 16384, 4095, 4095, 0xffffff0ull);

    // name, pixels, blocks, padded pixels, padded row bytes, size.
    surface("1x1", 1, 1, 1, 1, 64, 64, 256, 4096);
    surface("4x4", 4, 4, 1, 1, 64, 64, 256, 4096);
    surface("5x3", 5, 3, 2, 1, 64, 64, 256, 4096);
    surface("64x64", 64, 64, 16, 16, 64, 64, 256, 4096);
    surface("65x64", 65, 64, 17, 16, 128, 64, 512, 8192);
    surface("64x65", 64, 65, 16, 17, 64, 128, 256, 8192);
    surface("81x69", 81, 69, 21, 18, 128, 128, 512, 16384);
    surface("200x72", 200, 72, 50, 18, 256, 128, 1024, 32768);
    surface("72x200", 72, 200, 18, 50, 128, 256, 512, 32768);
    surface("16384x16384", 16384, 16384, 4096, 4096, 16384, 16384, 65536, 0x10000000ull, false);

    // The description constructor reads the same width and height.
    const Bc16Surface from_desc(bc7(200, 72));
    if (from_desc.columns != 50 || from_desc.rows != 18 || from_desc.tile_columns != 4 || from_desc.tile_rows != 2)
        fail("Bc16Surface(desc) 200x72", "%ux%u blocks, %ux%u tiles", from_desc.columns, from_desc.rows,
             from_desc.tile_columns, from_desc.tile_rows);

    // Accepted: one 2D BC7_UNORM top mip in the default layout, at both dimension limits.
    accepted("81x69", bc7(), true);
    accepted("1x1", bc7(1, 1), true);
    accepted("16384x16384", bc7(16384, 16384), true);
    // Refused: one field changed at a time.
    auto desc = bc7();
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    accepted("3D", desc, false);
    desc = bc7();
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    accepted("1D", desc, false);
    desc = bc7();
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    accepted("buffer", desc, false);
    desc = bc7();
    desc.Format = DXGI_FORMAT_BC7_UNORM_SRGB;
    accepted("BC7_UNORM_SRGB", desc, false);
    desc = bc7();
    desc.Format = DXGI_FORMAT_BC7_TYPELESS;
    accepted("BC7_TYPELESS", desc, false);
    desc = bc7();
    desc.Format = DXGI_FORMAT_BC3_UNORM;
    accepted("BC3_UNORM", desc, false);
    desc = bc7();
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    accepted("row-major layout", desc, false);
    desc = bc7();
    desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    accepted("64 KiB swizzle layout", desc, false);
    accepted("width 0", bc7(0, 69), false);
    accepted("width 16385", bc7(16385, 69), false);
    accepted("width 2^32 + 64 (64 once truncated)", bc7(0x100000040ull, 69), false);
    accepted("height 0", bc7(81, 0), false);
    accepted("height 16385", bc7(81, 16385), false);
    desc = bc7();
    desc.DepthOrArraySize = 0;
    accepted("zero slices", desc, false);
    desc = bc7();
    desc.DepthOrArraySize = 2;
    accepted("two slices", desc, false);
    desc = bc7();
    desc.MipLevels = 0;
    accepted("full mip chain (MipLevels 0)", desc, false);
    desc = bc7();
    desc.MipLevels = 2;
    accepted("two mips", desc, false);
    desc = bc7();
    desc.SampleDesc.Count = 0;
    accepted("zero samples", desc, false);
    desc = bc7();
    desc.SampleDesc.Count = 4;
    accepted("four samples", desc, false);
    desc = bc7();
    desc.SampleDesc.Quality = 1;
    accepted("sample quality 1", desc, false);
    desc = bc7();
    desc.Alignment = 0x1000;
    accepted("4 KiB alignment", desc, false);
    desc = bc7();
    desc.Alignment = 0x10000;
    accepted("64 KiB alignment", desc, false);
    desc = bc7();
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    accepted("unordered-access flag", desc, false);
    desc = bc7();
    desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    accepted("deny-shader-resource flag", desc, false);

    if (failures) {
        std::printf("startup_bc7_4k_s: %d FAILED\n", failures);
        return 1;
    }
    std::printf("startup_bc7_4k_s: PASS (16 offsets, 10 surfaces, 3 accepted, 24 refused descriptions)\n");
    return 0;
}
