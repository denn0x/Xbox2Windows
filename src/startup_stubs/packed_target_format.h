// Experimental (docs/startup-runtime.md): a stand-in format for 16-bit packed render targets that
// the adapter cannot use the way the description asks.
//
// A console executable creates B5G5R5A1_UNORM and B4G4R4A4_UNORM 2D textures as render targets,
// the first also with unordered access. B5G6R5_UNORM and 2D arrays (a cube map is an array of six
// slices) follow the same rule. An adapter may not support that (measured: neither format
// as a render target or typed UAV on an NVIDIA adapter through vkd3d-proton); the description is
// then refused, the allocation-info answer is all ones and the executable gets no texture. Such a
// texture is created as R8G8B8A8_UNORM and tagged with the format it stands in for; a view that
// names that format on that texture is created as R8G8B8A8_UNORM.
//
// Limits. The stand-in keeps 8 bits a channel where the packed format keeps 5, 6, 4 or 1; a
// B5G6R5_UNORM stand-in also has an alpha channel the packed format lacks (it samples as 1): values the
// executable renders are not quantized, so a read of the target can differ from the console's in
// the low bits (0.45 reads 115 here, 119 from B4G4R4A4). Texel data the executable writes or reads
// itself in the 16-bit layout (uploads, copies to or from buffers) is not converted; only textures
// that are rendered to and then sampled or presented are covered.
#pragma once

#include <d3d12.h>

namespace x2w_startup {
inline bool packed_target_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B5G5R5A1_UNORM || format == DXGI_FORMAT_B4G4R4A4_UNORM || format == DXGI_FORMAT_B5G6R5_UNORM;
}

// The format to create a texture with. support1(format) returns the adapter's
// D3D12_FORMAT_SUPPORT1 bits for a format, or 0 when the query fails. The format changes only for
// the roles that were measured: a single-sample 2D texture or 2D array that is a render
// target, alone or with unordered access, in a packed format for which the adapter lacks one of
// those uses while R8G8B8A8_UNORM has them all. Anything else keeps its format.
//
// An array of 2 to D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION slices is covered the same way: the
// stand-in changes the format and nothing else, so slice count, mip count and view dimensions of
// the description and of its views are kept. Zero slices, more than the limit, three dimensions and
// multisampling are not modelled and keep their format.
template <class Support1>
DXGI_FORMAT packed_target_stand_in(const D3D12_RESOURCE_DESC& desc, Support1&& support1)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize == 0 ||
        desc.DepthOrArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION || desc.SampleDesc.Count != 1 ||
        !packed_target_format(desc.Format) || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return desc.Format;
    UINT required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) required |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    if ((static_cast<UINT>(support1(desc.Format)) & required) == required) return desc.Format;
    if ((static_cast<UINT>(support1(DXGI_FORMAT_R8G8B8A8_UNORM)) & required) != required) return desc.Format;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Provenance: a texture created as a stand-in carries the format it was asked for as private data.
// {6B1D3C52-8E0A-4F1B-9C6E-2A7D5E4F9B31}
inline constexpr GUID packed_stand_in_tag = {0x6b1d3c52, 0x8e0a, 0x4f1b, {0x9c, 0x6e, 0x2a, 0x7d, 0x5e, 0x4f, 0x9b, 0x31}};

// Tags `created` when it is a stand-in for `requested` (the description's desktop format).
inline void mark_packed_stand_in(ID3D12Resource* created, DXGI_FORMAT requested)
{
    if (created && packed_target_format(requested) && created->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        created->SetPrivateData(packed_stand_in_tag, sizeof(requested), &requested);
}

// The packed format `resource` stands in for, or DXGI_FORMAT_UNKNOWN.
inline DXGI_FORMAT packed_stand_in_of(ID3D12Resource* resource)
{
    DXGI_FORMAT requested = DXGI_FORMAT_UNKNOWN;
    UINT bytes = sizeof(requested);
    if (!resource || FAILED(resource->GetPrivateData(packed_stand_in_tag, &bytes, &requested)) || bytes != sizeof(requested))
        return DXGI_FORMAT_UNKNOWN;
    return requested;
}

enum class PackedView {
    unchanged,  // not a packed view, or a packed view of a texture of that very format
    stand_in,   // a packed view of the stand-in for that format: create it as R8G8B8A8_UNORM
    refused,    // a packed view of any other texture; the two packed formats have no compatible format
};

// What to do with a view described with view_format on a texture of resource_format that stands in
// for stands_in_for (DXGI_FORMAT_UNKNOWN: an ordinary texture).
inline PackedView packed_view(DXGI_FORMAT view_format, DXGI_FORMAT resource_format, DXGI_FORMAT stands_in_for)
{
    if (!packed_target_format(view_format)) return PackedView::unchanged;
    if (stands_in_for == DXGI_FORMAT_UNKNOWN) return resource_format == view_format ? PackedView::unchanged : PackedView::refused;
    return stands_in_for == view_format && resource_format == DXGI_FORMAT_R8G8B8A8_UNORM ? PackedView::stand_in : PackedView::refused;
}
}  // namespace x2w_startup
