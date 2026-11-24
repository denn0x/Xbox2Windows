#pragma once

#include <d3d12.h>
#include <algorithm>
#include <array>

// Slot-10 tile plan for a 64 KiB standard layout: which mips own whole tiles, how many tiles
// each one covers, and where each one starts inside a slice. A mip that is at most half a tile
// wide and one tile high shares a single tile with all smaller mips; that tile comes first in the
// slice. The other mips follow from the smallest to the largest, each on a whole-tile grid.
// Fitted to texture pool files of one console package (docs/experiments.md); the two shapes of
// bc64_chain.h follow the same rule.
namespace x2w_startup::tile64 {
constexpr UINT max_mips = 16;

// Texels covered by one 64 KiB tile.
inline bool tile_shape(DXGI_FORMAT format, UINT& width, UINT& height)
{
    const UINT f = static_cast<UINT>(format);
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return width = 512, height = 256, true;   // 8-byte blocks
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return width = 256, height = 256, true;  // 16-byte blocks
    if (f >= 60 && f <= 65) return width = 256, height = 256, true;                               // 8 bits per texel
    if (f >= 48 && f <= 59) return width = 256, height = 128, true;                               // 16 bits per texel
    if ((f >= 27 && f <= 32) || f == 87 || f == 88 || (f >= 90 && f <= 93)) return width = 128, height = 128, true;   // 32 bits per texel, 8-bit channels
    return false;
}

struct Plan {
    UINT tile_width = 0, tile_height = 0, mips = 0, slices = 0;
    UINT standard = 0, shared_tiles = 0, slice_tiles = 0;
    std::array<UINT, max_mips> wide{}, high{}, start{};

    static bool supports(const D3D12_RESOURCE_DESC& d)
    {
        UINT w, h;
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && static_cast<UINT>(d.Layout) == 0x10a && d.Width && d.Height &&
               d.Width <= 16384 && d.Height <= 16384 && d.DepthOrArraySize && d.MipLevels && d.MipLevels <= max_mips &&
               d.SampleDesc.Count <= 1 && tile_shape(d.Format, w, h);
    }

    explicit Plan(const D3D12_RESOURCE_DESC& d) : mips(d.MipLevels), slices(d.DepthOrArraySize)
    {
        tile_shape(d.Format, tile_width, tile_height);
        for (; standard < mips; ++standard) {
            const UINT w = (std::max)(1u, static_cast<UINT>(d.Width) >> standard), h = (std::max)(1u, d.Height >> standard);
            if (w * 2 <= tile_width && h <= tile_height) break;
            wide[standard] = (w + tile_width - 1) / tile_width;
            high[standard] = (h + tile_height - 1) / tile_height;
        }
        shared_tiles = standard < mips ? 1 : 0;
        slice_tiles = shared_tiles;
        for (UINT mip = standard; mip-- > 0;) {
            start[mip] = slice_tiles;
            slice_tiles += wide[mip] * high[mip];
        }
    }
};
} // namespace x2w_startup::tile64
