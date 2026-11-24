#pragma once

#include <d3d12.h>
#include <cstdint>

namespace x2w_startup {
// Called for a CPU-backed 2D texture already uploaded, without a new XG write notification.
// A decoder can write R8 planes directly while cycling through its frame buffers.
inline bool recheck_cpu_texture(const D3D12_RESOURCE_DESC& desc, UINT64 bytes, UINT elapsed_frames)
{
    const bool plane = desc.Format == DXGI_FORMAT_R8_UNORM && desc.MipLevels == 1 && desc.DepthOrArraySize == 1;
    return bytes <= (UINT64{32} << 20) && elapsed_frames >= (plane ? 1u : 15u);
}

// GetCopyableFootprints orders a single NV12 image as Y followed by UV.
// Keep this layout identical in the XG writer and the desktop draw uploader.
struct SliceFootprints {
    static constexpr UINT max_mips = 15;
    UINT planes;
    UINT count;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed[max_mips * 2]{};
    UINT rows[max_mips * 2]{};
    UINT64 row_bytes[max_mips * 2]{};
    UINT64 size = UINT64_MAX;

    static bool supports(const D3D12_RESOURCE_DESC& desc)
    {
        return desc.Format != DXGI_FORMAT_NV12 ||
               (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
                desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && desc.Width && desc.Height &&
                (desc.Width & 1) == 0 && (desc.Height & 1) == 0);
    }

    SliceFootprints(ID3D12Device* device, D3D12_RESOURCE_DESC desc, UINT mips)
        : planes(desc.Format == DXGI_FORMAT_NV12 ? 2u : 1u), count(mips * planes)
    {
        if (device && supports(desc) && mips && mips <= max_mips)
            device->GetCopyableFootprints(&desc, 0, count, 0, placed, rows, row_bytes, &size);
    }

    // Bytes from the start of subresource i to the end of its last row; 0 when there is nothing to
    // span: an index past the arrays, or a plan whose entry was never filled (no device, unsupported
    // format) and so has no rows or depth. Callers treat 0 as an empty copy.
    UINT64 bytes(UINT i) const
    {
        if (i >= max_mips * 2) return 0;
        const UINT64 depth = placed[i].Footprint.Depth, row_count = rows[i];
        if (!depth || !row_count) return 0;
        const UINT64 pitch = placed[i].Footprint.RowPitch;
        const UINT64 preceding_rows = depth * row_count - 1;
        if (pitch && preceding_rows > (UINT64_MAX - row_bytes[i]) / pitch) return 0;
        return pitch * preceding_rows + row_bytes[i];
    }

    void upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* upload_buffer,
                UINT mips, UINT slices, UINT slice, UINT i, UINT64 offset) const
    {
        // Nothing is copied for a missing object, a plan without footprints, or an invalid index.
        if (!list || !texture || !upload_buffer || !mips || mips > max_mips || !slices || slice >= slices ||
            i >= count || i >= max_mips * 2 || size == UINT64_MAX)
            return;
        const UINT64 subresource = i % mips + UINT64{slice} * mips + UINT64{i / mips} * slices * mips;
        if (subresource > UINT32_MAX) return;
        D3D12_TEXTURE_COPY_LOCATION from{upload_buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = {offset, placed[i].Footprint};
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = static_cast<UINT>(subresource);
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
};
}  // namespace x2w_startup
