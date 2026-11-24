// Checks src/startup_stubs/gfx10_view_words.h without a device: buffer view layouts (typed views step by the
// element size), GFX10_FORMAT codes, V# and T# words decoded with an independent field table (bit positions of
// Mesa src/amd/registers/gfx10-rsrc.json), and the view descriptions read from real D3D12 SDK structs.
// --mutate legacy | width | order | uint: the helper's answer replaced the way a wrong helper would answer (the
// old `stride ? stride : 4` layout, the whole width - 1 in word 2, R10G10B10A2 as 10_10_10_2, an R32_FLOAT
// integer UAV); each must fail on the rows it concerns.
#include "startup_stubs/gfx10_view_words.h"

#include <cstdio>
#include <cstring>

namespace {
int failures, checks;
const char* mutation = "";
using namespace x2w_startup;

void expect(const char* name, unsigned long long got, unsigned long long wanted)
{
    ++checks;
    if (got != wanted) {
        ++failures;
        std::printf("FAIL %s: got 0x%llx (%llu), wanted 0x%llx (%llu)\n", name, got, got, wanted, wanted);
    }
}

std::uint32_t field(const std::uint32_t* w, int word, int lo, int hi) { return (w[word] >> lo) & ((hi - lo == 31) ? ~0u : ((1u << (hi - lo + 1)) - 1)); }

BufferViewLayout layout(DXGI_FORMAT f, std::uint64_t first, std::uint32_t count, std::uint32_t stride, std::uint32_t flags)
{
    return buffer_view_layout(f, first, count, stride, flags, !std::strcmp(mutation, "legacy"));
}

void image_words(const ImageViewFacts& v, std::uint32_t w[8], bool* unmapped = nullptr)
{
    gfx10_image_words(v, w, unmapped);
    if (!std::strcmp(mutation, "width")) {  // a wrong split: all of width - 1 in word 2
        const std::uint32_t width = static_cast<std::uint32_t>(v.width - 1);
        w[1] &= ~(3u << 30);
        w[2] = (w[2] & ~0xfffu) | (width & 0xfff);
    }
}

std::uint32_t format_code(DXGI_FORMAT f)
{
    if (!std::strcmp(mutation, "order") && f == DXGI_FORMAT_R10G10B10A2_UNORM) return 44;
    return gfx10_format(f).code;
}

void buffers()
{
    // Typed R16_UINT view, FirstElement 3: element 2 bytes, starts at byte 6, 10 elements = 20 bytes.
    BufferViewLayout l = layout(DXGI_FORMAT_R16_UINT, 3, 10, 0, 0);
    expect("r16 typed kind", static_cast<int>(l.kind), static_cast<int>(BufferViewKind::typed));
    expect("r16 offset", l.offset, 6);
    expect("r16 bytes", l.bytes, 20);
    expect("r16 index stride", l.index_stride, 2);
    expect("r16 format", l.format.code, 11);
    // R32G32B32A32_FLOAT typed view: 16-byte elements.
    l = layout(DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 4, 0, 0);
    expect("rgba32f offset", l.offset, 32);
    expect("rgba32f bytes", l.bytes, 64);
    expect("rgba32f index stride", l.index_stride, 16);
    // R32_UINT typed: the old layout had the right base but index stride 0.
    l = layout(DXGI_FORMAT_R32_UINT, 5, 8, 0, 0);
    expect("r32u offset", l.offset, 20);
    expect("r32u index stride", l.index_stride, 4);
    // RAW and structured views keep the old layout.
    l = layout(DXGI_FORMAT_R32_TYPELESS, 4, 64, 0, 1);
    expect("raw kind", static_cast<int>(l.kind), static_cast<int>(BufferViewKind::raw));
    expect("raw offset", l.offset, 16);
    expect("raw bytes", l.bytes, 256);
    expect("raw index stride", l.index_stride, 0);
    expect("raw num_records (bytes)", l.num_records, 256);
    l = buffer_view_layout(DXGI_FORMAT_UNKNOWN, 7, 3, 36, 0);
    expect("structured offset", l.offset, 252);
    expect("structured index stride", l.index_stride, 36);
    expect("structured num_records (elements)", l.num_records, 3);

    // V# of the R16_UINT view: STRIDE [16,29] = 2, NUM_RECORDS = 10, FORMAT [12,18] = 16_UINT, DST_SEL X001.
    std::uint32_t w[8];
    gfx10_buffer_words(layout(DXGI_FORMAT_R16_UINT, 3, 10, 0, 0), w);
    expect("v# word0 (no address)", w[0], 0);
    expect("v# stride", field(w, 1, 16, 29), 2);
    expect("v# base hi (no address)", field(w, 1, 0, 15), 0);
    expect("v# num_records", w[2], 10);
    expect("v# dst_sel_x", field(w, 3, 0, 2), 4);
    expect("v# dst_sel_y", field(w, 3, 3, 5), 0);
    expect("v# dst_sel_w", field(w, 3, 9, 11), 1);
    expect("v# format", field(w, 3, 12, 18), 11);
    expect("v# oob_select", field(w, 3, 28, 29), 0);
    expect("v# type", field(w, 3, 30, 31), 0);
    // Raw view V# as ac_build_raw_buffer_descriptor: 32_FLOAT, XYZW, OOB_SELECT_RAW, NUM_RECORDS in bytes.
    gfx10_buffer_words(buffer_view_layout(DXGI_FORMAT_R32_TYPELESS, 0, 64, 0, 1), w);
    expect("raw v# stride", field(w, 1, 16, 29), 0);
    expect("raw v# num_records", w[2], 256);
    expect("raw v# format", field(w, 3, 12, 18), 22);
    expect("raw v# dst_sel", field(w, 3, 0, 11), 4 | 5 << 3 | 6 << 6 | 7 << 9);
    expect("raw v# oob_select", field(w, 3, 28, 29), 3);

    // The record offsets against the SDK structs.
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R8_UNORM;
    s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    s.Buffer.FirstElement = 9;
    s.Buffer.NumElements = 100;
    unsigned char bytes[64] = {};
    std::memcpy(bytes, &s, sizeof(s));
    expect("srv desc parsed", buffer_view_layout_from_desc(false, bytes, sizeof(s), l, !std::strcmp(mutation, "legacy")), 1);
    expect("srv r8 offset", l.offset, 9);
    expect("srv r8 bytes", l.bytes, 100);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.FirstElement = 12;
    u.Buffer.NumElements = 4;
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    std::memset(bytes, 0, sizeof(bytes));
    std::memcpy(bytes, &u, sizeof(u));
    expect("uav desc parsed", buffer_view_layout_from_desc(true, bytes, sizeof(u), l), 1);
    expect("uav raw kind", static_cast<int>(l.kind), static_cast<int>(BufferViewKind::raw));
    expect("uav raw offset", l.offset, 48);
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    u.Format = DXGI_FORMAT_R16G16_FLOAT;
    std::memcpy(bytes, &u, sizeof(u));
    buffer_view_layout_from_desc(true, bytes, sizeof(u), l, !std::strcmp(mutation, "legacy"));
    expect("uav r16g16f index stride", l.index_stride, 4);
    expect("uav r16g16f format", l.format.code, 29);
}

void formats()
{
    expect("R10G10B10A2_UNORM = 2_10_10_10_UNORM", format_code(DXGI_FORMAT_R10G10B10A2_UNORM), 50);
    expect("R11G11B10_FLOAT = 10_11_11_FLOAT", format_code(DXGI_FORMAT_R11G11B10_FLOAT), 36);
    expect("R8G8B8A8_UNORM", format_code(DXGI_FORMAT_R8G8B8A8_UNORM), 56);
    expect("R32_FLOAT", format_code(DXGI_FORMAT_R32_FLOAT), 22);
    expect("R16G16B16A16_FLOAT", format_code(DXGI_FORMAT_R16G16B16A16_FLOAT), 71);
    expect("BC7_UNORM", format_code(DXGI_FORMAT_BC7_UNORM), 181);
    expect("typeless is unmapped", format_code(DXGI_FORMAT_R8G8B8A8_TYPELESS), 0);
    expect("B8G8R8A8 swizzle ZYXW", gfx10_format(DXGI_FORMAT_B8G8R8A8_UNORM).dst_sel, 6 | 5 << 3 | 4 << 6 | 7 << 9);
}

void images()
{
    // 2D 1920x1080, 11 mips, RGBA8 SRV of mips 2..: built from an SDK struct.
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MostDetailedMip = 2;
    s.Texture2D.MipLevels = UINT(-1);
    unsigned char bytes[64] = {};
    std::memcpy(bytes, &s, sizeof(s));
    ImageViewFacts v{};
    expect("srv texture2d parsed", image_view_from_desc(false, bytes, sizeof(s), v), 1);
    v.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    v.width = 1920;
    v.height = 1080;
    v.depth_or_array = 1;
    v.mip_levels = 11;
    std::uint32_t w[8];
    bool unmapped = true;
    image_words(v, w, &unmapped);
    expect("t# format mapped", unmapped, 0);
    const std::uint32_t width = field(w, 1, 30, 31) | field(w, 2, 0, 11) << 2;
    expect("t# width - 1 (WIDTH_LO | WIDTH_HI << 2)", width, 1919);
    expect("t# height - 1", field(w, 2, 14, 27), 1079);
    expect("t# format", field(w, 1, 20, 28), 56);
    expect("t# word1 nonzero (null test)", w[1] != 0, 1);
    expect("t# base level", field(w, 3, 12, 15), 2);
    expect("t# last level", field(w, 3, 16, 19), 10);
    expect("t# type 2D", field(w, 3, 28, 31), 9);
    expect("t# depth", field(w, 4, 0, 12), 0);
    expect("t# max mip", field(w, 5, 4, 7), 10);
    expect("t# word0 (no address)", w[0], 0);
    expect("t# words 6-7", w[6] | w[7], 0);
    // Hand-computed constants for the same view (word1 = 56 << 20 | 3 << 30; word2 = 479 | 1079 << 14).
    expect("t# word1 exact", w[1], 0xc3800000u);
    expect("t# word2 exact", w[2], 0x010dc1dfu);

    // 2D array UAV, slices 3..6 of 8, mip 1; 3D SRV depth 32.
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    u.Texture2DArray = {1, 3, 4, 0};
    std::memset(bytes, 0, sizeof(bytes));
    std::memcpy(bytes, &u, sizeof(u));
    v = {};
    expect("uav array parsed", image_view_from_desc(true, bytes, sizeof(u), v), 1);
    v.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    v.width = 512;
    v.height = 256;
    v.depth_or_array = 8;
    v.mip_levels = 4;
    image_words(v, w);
    expect("array type", field(w, 3, 28, 31), 13);
    expect("array base level = last level", field(w, 3, 12, 15) * 16 + field(w, 3, 16, 19), 1 * 16 + 1);
    expect("array base_array", field(w, 4, 16, 28), 3);
    expect("array depth = last layer", field(w, 4, 0, 12), 6);
    v = {};
    v.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    v.view_dimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    v.format = DXGI_FORMAT_R11G11B10_FLOAT;
    v.width = 64, v.height = 64, v.depth_or_array = 32, v.mip_levels = 1, v.level_count = UINT(-1);
    image_words(v, w);
    expect("3d type", field(w, 3, 28, 31), 10);
    expect("3d depth - 1", field(w, 4, 0, 12), 31);
    expect("3d array pitch (srv)", field(w, 5, 0, 3), 0);
    // A view the synthesizer cannot describe yields zeros (the runtime treats it as null).
    v.view_dimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    expect("2D view of a 3D resource refused", gfx10_image_words(v, w), 0);
    expect("refused words zero", w[1] | w[2] | w[3], 0);
}

void integer_uav()
{
    DXGI_FORMAT got = integer_uav_format(DXGI_FORMAT_R32_TYPELESS);
    if (!std::strcmp(mutation, "uint")) got = DXGI_FORMAT_R32_FLOAT;
    expect("R32_TYPELESS resource -> R32_UINT UAV", got, DXGI_FORMAT_R32_UINT);
    expect("R32_UINT resource -> R32_UINT UAV", integer_uav_format(DXGI_FORMAT_R32_UINT), DXGI_FORMAT_R32_UINT);
    expect("R32_FLOAT resource refused", integer_uav_format(DXGI_FORMAT_R32_FLOAT), DXGI_FORMAT_UNKNOWN);
    expect("R8G8B8A8_TYPELESS resource refused", integer_uav_format(DXGI_FORMAT_R8G8B8A8_TYPELESS), DXGI_FORMAT_UNKNOWN);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--mutate")) mutation = argv[2];
    buffers();
    formats();
    images();
    integer_uav();
    std::printf("startup_gfx10_view_words%s%s: %d checks, %d failures\n", *mutation ? " --mutate " : "", mutation, checks, failures);
    return failures ? 1 : 0;
}
