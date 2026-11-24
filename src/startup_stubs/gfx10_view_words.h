#pragma once
// LOCAL (compute translation runtime): facts the translated console compute programs need about a D3D12 view
// that the title created, in the console's own terms.
//
//  - buffer_view_layout: where a buffer view starts and how an index steps through it. A typed buffer view
//    (StructureByteStride 0, not RAW) steps by the format's element size, as the hardware buffer descriptor of a
//    typed buffer holds stride = element size (buffer_load_format ... idxen addresses base + index * stride).
//  - gfx10_format: the GFX10_FORMAT code (the 7-bit BUF_FMT / 9-bit IMG format of RDNA 1) of a DXGI format.
//  - gfx10_buffer_words / gfx10_image_words: the dwords of the buffer (V#) or image (T#) descriptor that the
//    console would hold for the view, for programs that read descriptor fields as numbers.
//
// Sources (public, Mesa revision bb59ce45, 2026-11-24):
//  - src/amd/registers/gfx10-rsrc.json: field bits of SQ_BUF_RSRC_WORD1..3 and SQ_IMG_RSRC_WORD1..5, and the
//    GFX10_FORMAT enum.
//  - src/amd/registers/gfx6.json: SQ_RSRC_IMG_TYPE (1D 8, 2D 9, 3D 10, CUBE 11, 1D_ARRAY 12, 2D_ARRAY 13) and
//    SQ_SEL_XYZW01 (0 0, 1 1, X 4, Y 5, Z 6, W 7).
//  - src/amd/common/ac_descriptors.c: ac_build_buffer_descriptor (word1 = BASE_ADDRESS_HI | STRIDE, word2 = size),
//    ac_build_raw_buffer_descriptor (format 32_FLOAT, swizzle XYZW, OOB_SELECT_RAW), and
//    ac_build_gfx10_texture_descriptor (word1 FORMAT | WIDTH_LO(width - 1), word2 WIDTH_HI((width - 1) >> 2) |
//    HEIGHT(height - 1), word3 DST_SEL | BASE_LEVEL | LAST_LEVEL | TYPE, word4 DEPTH | BASE_ARRAY with "Depth is
//    the last accessible layer", word5 ARRAY_PITCH (3D: 0 SRV, 1 UAV) | MAX_MIP = levels - 1).
//  - src/amd/common/gfx10_format_table.py: format names list channels from the most significant bits ("Keep the
//    bit sizes in little-endian order"), so DXGI R10G10B10A2 is 2_10_10_10 and R11G11B10 is 10_11_11.
//
// Fields that are NOT synthesized and stay 0 (the runtime logs this once per kind): buffer base address (word0,
// word1 BASE_ADDRESS_HI), SWIZZLE_ENABLE, CACHE_SWIZZLE, INDEX_STRIDE, ADD_TID_ENABLE, RESOURCE_LEVEL; image base
// address (word0, word1 BASE_ADDRESS_HI), MIN_LOD, RESOURCE_LEVEL, SW_MODE, BC_SWIZZLE, PERF_MOD, words 6-7
// (metadata / compression).
#include <d3d12.h>

#include <cstdint>
#include <cstring>

namespace x2w_startup {

namespace gfx10 {
constexpr std::uint32_t sel_0 = 0, sel_1 = 1, sel_x = 4, sel_y = 5, sel_z = 6, sel_w = 7;  // SQ_SEL_XYZW01
constexpr std::uint32_t dst_sel(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w)
{
    return x | (y << 3) | (z << 6) | (w << 9);  // DST_SEL_X [0,2], _Y [3,5], _Z [6,8], _W [9,11]
}
constexpr std::uint32_t xyzw = dst_sel(sel_x, sel_y, sel_z, sel_w), xyz1 = dst_sel(sel_x, sel_y, sel_z, sel_1),
                        xy01 = dst_sel(sel_x, sel_y, sel_0, sel_1), x001 = dst_sel(sel_x, sel_0, sel_0, sel_1),
                        zyxw = dst_sel(sel_z, sel_y, sel_x, sel_w), zyx1 = dst_sel(sel_z, sel_y, sel_x, sel_1),
                        a8 = dst_sel(sel_0, sel_0, sel_0, sel_x);
constexpr std::uint32_t img_1d = 8, img_2d = 9, img_3d = 10, img_cube = 11, img_1d_array = 12, img_2d_array = 13;  // SQ_RSRC_IMG_TYPE
constexpr std::uint32_t format_32_float = 22;
constexpr std::uint32_t oob_structured_with_offset = 0, oob_raw = 3;  // SQ_BUF_RSRC_WORD3__OOB_SELECT
}  // namespace gfx10

struct Gfx10Format {
    std::uint32_t code = 0;     // GFX10_FORMAT; 0 = not mapped
    std::uint32_t bytes = 0;    // element bytes when usable as a buffer format, else 0
    std::uint32_t dst_sel = 0;  // DST_SEL_X..W of the channels the format has
};

// The GFX10_FORMAT of a desktop DXGI format (typed, not typeless). Unmapped formats return code 0.
inline Gfx10Format gfx10_format(DXGI_FORMAT f)
{
    using namespace gfx10;
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {77, 16, xyzw};
    case DXGI_FORMAT_R32G32B32A32_UINT: return {75, 16, xyzw};
    case DXGI_FORMAT_R32G32B32A32_SINT: return {76, 16, xyzw};
    case DXGI_FORMAT_R32G32B32_FLOAT: return {74, 12, xyz1};
    case DXGI_FORMAT_R32G32B32_UINT: return {72, 12, xyz1};
    case DXGI_FORMAT_R32G32B32_SINT: return {73, 12, xyz1};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {71, 8, xyzw};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return {65, 8, xyzw};
    case DXGI_FORMAT_R16G16B16A16_UINT: return {69, 8, xyzw};
    case DXGI_FORMAT_R16G16B16A16_SNORM: return {66, 8, xyzw};
    case DXGI_FORMAT_R16G16B16A16_SINT: return {70, 8, xyzw};
    case DXGI_FORMAT_R32G32_FLOAT: return {64, 8, xy01};
    case DXGI_FORMAT_R32G32_UINT: return {62, 8, xy01};
    case DXGI_FORMAT_R32G32_SINT: return {63, 8, xy01};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {50, 4, xyzw};  // 2_10_10_10_UNORM
    case DXGI_FORMAT_R10G10B10A2_UINT: return {54, 4, xyzw};   // 2_10_10_10_UINT
    case DXGI_FORMAT_R11G11B10_FLOAT: return {36, 4, xyz1};    // 10_11_11_FLOAT
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {56, 4, xyzw};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {130, 0, xyzw};  // 8_8_8_8_SRGB (image only)
    case DXGI_FORMAT_R8G8B8A8_UINT: return {60, 4, xyzw};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return {57, 4, xyzw};
    case DXGI_FORMAT_R8G8B8A8_SINT: return {61, 4, xyzw};
    case DXGI_FORMAT_B8G8R8A8_UNORM: return {56, 4, zyxw};
    case DXGI_FORMAT_B8G8R8X8_UNORM: return {56, 4, zyx1};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {130, 0, zyxw};
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return {130, 0, zyx1};
    case DXGI_FORMAT_R16G16_FLOAT: return {29, 4, xy01};
    case DXGI_FORMAT_R16G16_UNORM: return {23, 4, xy01};
    case DXGI_FORMAT_R16G16_UINT: return {27, 4, xy01};
    case DXGI_FORMAT_R16G16_SNORM: return {24, 4, xy01};
    case DXGI_FORMAT_R16G16_SINT: return {28, 4, xy01};
    case DXGI_FORMAT_D32_FLOAT: return {22, 0, x001};
    case DXGI_FORMAT_R32_FLOAT: return {22, 4, x001};
    case DXGI_FORMAT_R32_UINT: return {20, 4, x001};
    case DXGI_FORMAT_R32_SINT: return {21, 4, x001};
    case DXGI_FORMAT_R8G8_UNORM: return {14, 2, xy01};
    case DXGI_FORMAT_R8G8_UINT: return {18, 2, xy01};
    case DXGI_FORMAT_R8G8_SNORM: return {15, 2, xy01};
    case DXGI_FORMAT_R8G8_SINT: return {19, 2, xy01};
    case DXGI_FORMAT_R16_FLOAT: return {13, 2, x001};
    case DXGI_FORMAT_D16_UNORM: return {7, 0, x001};
    case DXGI_FORMAT_R16_UNORM: return {7, 2, x001};
    case DXGI_FORMAT_R16_UINT: return {11, 2, x001};
    case DXGI_FORMAT_R16_SNORM: return {8, 2, x001};
    case DXGI_FORMAT_R16_SINT: return {12, 2, x001};
    case DXGI_FORMAT_R8_UNORM: return {1, 1, x001};
    case DXGI_FORMAT_R8_UINT: return {5, 1, x001};
    case DXGI_FORMAT_R8_SNORM: return {2, 1, x001};
    case DXGI_FORMAT_R8_SINT: return {6, 1, x001};
    case DXGI_FORMAT_A8_UNORM: return {1, 1, a8};
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return {132, 0, xyz1};  // 5_9_9_9_FLOAT
    // Depth/stencil as Mesa's hardcoded table (Z24_UNORM_S8_UINT 8_24_UNORM, Z32_FLOAT_S8X24 X24_8_32_FLOAT).
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return {141, 0, x001};
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return {146, 0, x001};
    case DXGI_FORMAT_BC1_UNORM: return {169, 0, xyzw};
    case DXGI_FORMAT_BC1_UNORM_SRGB: return {170, 0, xyzw};
    case DXGI_FORMAT_BC2_UNORM: return {171, 0, xyzw};
    case DXGI_FORMAT_BC2_UNORM_SRGB: return {172, 0, xyzw};
    case DXGI_FORMAT_BC3_UNORM: return {173, 0, xyzw};
    case DXGI_FORMAT_BC3_UNORM_SRGB: return {174, 0, xyzw};
    case DXGI_FORMAT_BC4_UNORM: return {175, 0, x001};
    case DXGI_FORMAT_BC4_SNORM: return {176, 0, x001};
    case DXGI_FORMAT_BC5_UNORM: return {177, 0, xy01};
    case DXGI_FORMAT_BC5_SNORM: return {178, 0, xy01};
    case DXGI_FORMAT_BC6H_UF16: return {179, 0, xyz1};
    case DXGI_FORMAT_BC6H_SF16: return {180, 0, xyz1};
    case DXGI_FORMAT_BC7_UNORM: return {181, 0, xyzw};
    case DXGI_FORMAT_BC7_UNORM_SRGB: return {182, 0, xyzw};
    default: return {};
    }
}

// ---- Buffer views ----------------------------------------------------------------------------------------------
enum class BufferViewKind { raw, structured, typed, unknown };
struct BufferViewLayout {
    BufferViewKind kind = BufferViewKind::unknown;
    std::uint64_t offset = 0;        // bytes from the resource start to element 0
    std::uint64_t bytes = 0;         // bytes the view covers
    std::uint32_t index_stride = 0;  // bytes per index step (descriptor STRIDE); 0 for raw views
    std::uint32_t num_records = 0;   // descriptor NUM_RECORDS: elements when strided, bytes when raw
    Gfx10Format format{};            // typed views only
};

// first_element/count/structure_stride/flags as in D3D12_BUFFER_SRV / D3D12_BUFFER_UAV (flags bit 0 = RAW in both).
// legacy = the behaviour before the typed-view fix (element 4 bytes when the stride is 0, index stride 0).
inline BufferViewLayout buffer_view_layout(DXGI_FORMAT format, std::uint64_t first_element, std::uint32_t count,
                                           std::uint32_t structure_stride, std::uint32_t flags, bool legacy = false)
{
    BufferViewLayout l;
    std::uint32_t element = 4;
    if (legacy) {
        element = structure_stride ? structure_stride : 4;
        l.kind = structure_stride ? BufferViewKind::structured : BufferViewKind::unknown;
        l.index_stride = structure_stride;
    } else if (structure_stride) {
        element = structure_stride;
        l.kind = BufferViewKind::structured;
        l.index_stride = structure_stride;
    } else if ((flags & 1) || format == DXGI_FORMAT_R32_TYPELESS) {
        l.kind = BufferViewKind::raw;
    } else if (const Gfx10Format f = gfx10_format(format); f.code && f.bytes) {
        element = f.bytes;
        l.kind = BufferViewKind::typed;
        l.index_stride = f.bytes;
        l.format = f;
    }
    l.offset = first_element * element;
    l.bytes = std::uint64_t{count} * element;
    l.num_records = l.index_stride ? count : static_cast<std::uint32_t>(l.bytes);
    return l;
}

// The buffer part of a recorded D3D12 view description: slot 18 SRV {Format, ViewDimension, Mapping, Buffer at 16},
// slot 19 UAV {Format, ViewDimension, Buffer at 8}; Buffer = {FirstElement u64, NumElements, StructureByteStride,
// (UAV: CounterOffsetInBytes u64), Flags}, so Flags is at byte 32 in both. False when it is not a buffer view.
inline bool buffer_view_layout_from_desc(bool uav, const unsigned char* desc, std::size_t bytes, BufferViewLayout& out, bool legacy = false)
{
    if (bytes < 36) return false;
    std::uint32_t format = 0, dim = 0, count = 0, stride = 0, flags = 0;
    std::uint64_t first = 0;
    const std::size_t at = uav ? 8 : 16;
    std::memcpy(&format, desc, 4);
    std::memcpy(&dim, desc + 4, 4);
    if (dim != 1) return false;  // D3D12_SRV_DIMENSION_BUFFER == D3D12_UAV_DIMENSION_BUFFER == 1
    std::memcpy(&first, desc + at, 8);
    std::memcpy(&count, desc + at + 8, 4);
    std::memcpy(&stride, desc + at + 12, 4);
    std::memcpy(&flags, desc + 32, 4);
    out = buffer_view_layout(static_cast<DXGI_FORMAT>(format), first, count, stride, flags, legacy);
    return true;
}

// V# dwords 0..3 (4..7 = 0) for a buffer view. Word 0 / BASE_ADDRESS_HI stay 0 (no address is synthesized).
inline void gfx10_buffer_words(const BufferViewLayout& l, std::uint32_t out[8])
{
    using namespace gfx10;
    std::memset(out, 0, 8 * sizeof(std::uint32_t));
    out[1] = (l.index_stride & 0x3fff) << 16;  // STRIDE [16,29]
    out[2] = l.num_records;                     // NUM_RECORDS
    const bool typed = l.kind == BufferViewKind::typed;
    const std::uint32_t format = typed ? l.format.code : format_32_float;
    const std::uint32_t sel = typed ? l.format.dst_sel : xyzw;
    const std::uint32_t oob = l.index_stride ? oob_structured_with_offset : oob_raw;
    out[3] = sel | ((format & 127) << 12) | (oob << 28);  // FORMAT [12,18], OOB_SELECT [28,29], TYPE [30,31] = 0
}

// ---- Image views -----------------------------------------------------------------------------------------------
struct ImageViewFacts {
    // the resource (desktop description; same extent as the title's)
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    std::uint64_t width = 0;
    std::uint32_t height = 0, depth_or_array = 0, mip_levels = 0;
    // the view
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // typed view format (typeless already mapped)
    std::uint32_t view_dimension = 0;          // D3D12_SRV_DIMENSION / D3D12_UAV_DIMENSION value (same numbers here)
    bool uav = false;
    std::uint32_t base_level = 0, level_count = 0;  // level_count 0xffffffff: to the last level
    std::uint32_t first_slice = 0, slice_count = 0;
};

// Reads the view part of a recorded D3D12 view description (slot 18 SRV: union at byte 16; slot 19 UAV: at byte 8).
// False for buffer, multisampled or unknown view dimensions.
inline bool image_view_from_desc(bool uav, const unsigned char* desc, std::size_t bytes, ImageViewFacts& v)
{
    if (bytes < 8) return false;
    std::uint32_t f = 0, dim = 0, u[6] = {};
    std::memcpy(&f, desc, 4);
    std::memcpy(&dim, desc + 4, 4);
    const std::size_t at = uav ? 8 : 16;
    if (bytes < at + sizeof(u)) return false;
    std::memcpy(u, desc + at, sizeof(u));
    v.format = static_cast<DXGI_FORMAT>(f);
    v.view_dimension = dim;
    v.uav = uav;
    v.first_slice = 0;
    v.slice_count = 1;
    if (uav) {
        // UAV: Texture1D {MipSlice}, 1DArray {MipSlice, First, Size}, 2D {MipSlice, Plane}, 2DArray {MipSlice, First,
        // Size, Plane}, 3D {MipSlice, FirstWSlice, WSize}
        v.base_level = u[0];
        v.level_count = 1;
        if (dim == D3D12_UAV_DIMENSION_TEXTURE1DARRAY || dim == D3D12_UAV_DIMENSION_TEXTURE2DARRAY || dim == D3D12_UAV_DIMENSION_TEXTURE3D) {
            v.first_slice = u[1];
            v.slice_count = u[2];
        } else if (dim != D3D12_UAV_DIMENSION_TEXTURE1D && dim != D3D12_UAV_DIMENSION_TEXTURE2D) {
            return false;
        }
        return true;
    }
    // SRV: {MostDetailedMip, MipLevels, ...}; arrays add FirstArraySlice, ArraySize; cube array First2DArrayFace, NumCubes
    v.base_level = u[0];
    v.level_count = u[1];
    switch (dim) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: case D3D12_SRV_DIMENSION_TEXTURE2D: case D3D12_SRV_DIMENSION_TEXTURE3D: break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: v.first_slice = u[2]; v.slice_count = u[3]; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: v.slice_count = 6; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: v.first_slice = u[2]; v.slice_count = 6 * u[3]; break;
    default: return false;
    }
    return true;
}

// T# dwords 0..7. False (all words 0) when the view cannot be described; unmapped_format is set when the format
// has no GFX10_FORMAT (the words are still written, with FORMAT 0).
inline bool gfx10_image_words(const ImageViewFacts& v, std::uint32_t out[8], bool* unmapped_format = nullptr)
{
    using namespace gfx10;
    std::memset(out, 0, 8 * sizeof(std::uint32_t));
    if (unmapped_format) *unmapped_format = false;
    if (!v.width || !v.height || !v.mip_levels) return false;
    std::uint32_t type = 0;
    const bool three_d = v.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    switch (v.view_dimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: type = img_1d; break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: type = img_1d_array; break;
    case D3D12_SRV_DIMENSION_TEXTURE2D: type = img_2d; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: type = img_2d_array; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: type = img_3d; break;  // D3D12_UAV_DIMENSION_TEXTURE3D is also 8
    case D3D12_SRV_DIMENSION_TEXTURECUBE: case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: type = v.uav ? 0 : img_cube; break;
    default: break;
    }
    if (!type || (type == img_3d) != three_d) return false;
    const Gfx10Format f = gfx10_format(v.format);
    if (!f.code && unmapped_format) *unmapped_format = true;
    const std::uint32_t w = static_cast<std::uint32_t>(v.width - 1), h = v.height - 1;
    std::uint32_t base = v.base_level < v.mip_levels ? v.base_level : 0;
    std::uint32_t count = v.level_count == 0xffffffffu || v.level_count == 0 || base + v.level_count > v.mip_levels ? v.mip_levels - base : v.level_count;
    const std::uint32_t last = base + count - 1;
    out[1] = ((f.code & 511) << 20) | ((w & 3) << 30);         // FORMAT [20,28], WIDTH_LO [30,31]
    out[2] = ((w >> 2) & 0xfff) | ((h & 0x3fff) << 14);        // WIDTH_HI [0,11], HEIGHT [14,27]
    out[3] = (f.code ? f.dst_sel : xyzw) | ((base & 15) << 12) | ((last & 15) << 16) | (type << 28);  // BASE/LAST_LEVEL, TYPE
    std::uint32_t depth = 0, base_array = 0, array_pitch = 0;
    if (type == img_3d) {
        if (v.uav) {  // UAV mode: BASE_ARRAY first slice, DEPTH last slice of the bound level
            const std::uint32_t level_depth = (v.depth_or_array >> base) ? (v.depth_or_array >> base) : 1;
            const std::uint32_t first = v.first_slice < level_depth ? v.first_slice : 0;
            const std::uint32_t n = v.slice_count == 0xffffffffu || first + v.slice_count > level_depth ? level_depth - first : v.slice_count;
            depth = first + n - 1;
            base_array = first;
            array_pitch = 1;
        } else {
            depth = v.depth_or_array - 1;  // SRV mode: last slice of level 0
        }
    } else if (type == img_1d_array || type == img_2d_array || type == img_cube) {
        const std::uint32_t layers = v.depth_or_array ? v.depth_or_array : 1;
        const std::uint32_t first = v.first_slice < layers ? v.first_slice : 0;
        const std::uint32_t n = v.slice_count == 0 || v.slice_count == 0xffffffffu || first + v.slice_count > layers ? layers - first : v.slice_count;
        depth = first + n - 1;  // the last accessible layer
        base_array = first;
    }
    out[4] = (depth & 0x1fff) | ((base_array & 0x1fff) << 16);  // DEPTH [0,12], BASE_ARRAY [16,28]
    out[5] = (array_pitch & 15) | (((v.mip_levels - 1) & 15) << 4);  // ARRAY_PITCH [0,3], MAX_MIP [4,7]
    return true;
}

// The UAV format of an integer texture UAV (manifest kind tex2du, RWTexture2D<uint>) over a resource of this format:
// R32_UINT, or UNKNOWN when D3D12 does not allow an R32_UINT view of it.
inline DXGI_FORMAT integer_uav_format(DXGI_FORMAT resource_format)
{
    return resource_format == DXGI_FORMAT_R32_TYPELESS || resource_format == DXGI_FORMAT_R32_UINT ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
}

}  // namespace x2w_startup
