// Pure validation and offset resolution for buffer memory SRVs (device slot 70).
// Acceleration-structure views are not handled here.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace x2w_startup {

// Byte size of a single element for typed buffer SRVs; 0 when unknown or invalid for typed buffers.
inline std::uint32_t typed_buffer_format_bytes(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return 4;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return 1;
    default:
        return 0;
    }
}

inline bool valid_buffer_resource_desc(const D3D12_RESOURCE_DESC& r)
{
    return r.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && r.Width > 0 && r.Height == 1 &&
           r.DepthOrArraySize == 1 && r.MipLevels == 1 && r.Format == DXGI_FORMAT_UNKNOWN &&
           r.SampleDesc.Count == 1 && r.SampleDesc.Quality == 0 &&
           r.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
           (r.Alignment == 0 || r.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) &&
           !(r.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

inline std::uint32_t buffer_srv_element_bytes(const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    if (v.ViewDimension != D3D12_SRV_DIMENSION_BUFFER ||
        v.Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING ||
        v.Buffer.NumElements == 0)
        return 0;
    const UINT flags = static_cast<UINT>(v.Buffer.Flags);
    if (flags & ~static_cast<UINT>(D3D12_BUFFER_SRV_FLAG_RAW)) return 0;
    if (flags == D3D12_BUFFER_SRV_FLAG_RAW) {
        if (v.Format != DXGI_FORMAT_R32_TYPELESS || v.Buffer.StructureByteStride != 0) return 0;
        return 4;
    }
    if (v.Buffer.StructureByteStride != 0) {
        if (v.Format != DXGI_FORMAT_UNKNOWN || v.Buffer.StructureByteStride > 2048 ||
            v.Buffer.StructureByteStride % 4 != 0)
            return 0;
        return v.Buffer.StructureByteStride;
    }
    return typed_buffer_format_bytes(v.Format);
}

inline bool supports_buffer_memory_srv(const D3D12_RESOURCE_DESC& r, const D3D12_SHADER_RESOURCE_VIEW_DESC& v)
{
    if (!valid_buffer_resource_desc(r)) return false;
    const std::uint32_t elem_bytes = buffer_srv_element_bytes(v);
    if (!elem_bytes) return false;
    const std::uint64_t total_elements = r.Width / elem_bytes;
    return v.Buffer.FirstElement <= total_elements &&
           v.Buffer.NumElements <= total_elements - v.Buffer.FirstElement;
}

inline bool resolve_buffer_srv_desc(std::uint64_t buffer_title_base, std::uint64_t buffer_width,
                                    std::uint64_t address, const D3D12_SHADER_RESOURCE_VIEW_DESC& view,
                                    D3D12_SHADER_RESOURCE_VIEW_DESC& resolved)
{
    const std::uint32_t elem_bytes = buffer_srv_element_bytes(view);
    if (!elem_bytes || !buffer_title_base || address < buffer_title_base) return false;
    const std::uint64_t offset = address - buffer_title_base;
    if (offset >= buffer_width || offset % elem_bytes != 0) return false;
    const std::uint64_t avail_elements = (buffer_width - offset) / elem_bytes;
    if (view.Buffer.FirstElement > avail_elements ||
        view.Buffer.NumElements > avail_elements - view.Buffer.FirstElement)
        return false;
    const std::uint64_t adjusted_first = view.Buffer.FirstElement + offset / elem_bytes;
    if (view.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW &&
        (adjusted_first * elem_bytes) % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0)
        return false;
    resolved = view;
    resolved.Buffer.FirstElement = adjusted_first;
    return true;
}

}  // namespace x2w_startup
