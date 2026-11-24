// Experimental (docs/startup-runtime.md): the format of the view the window presenter samples.
//
// The presenter draws the presented surface with a shader that reads a float4. A texture created
// with a typeless format cannot be viewed with that format: a shader resource view needs a typed
// one. The 8-bit and 10:10:10:2 colour families are viewed as UNORM, the only member that a float4
// read takes unchanged; no sRGB conversion is added, because that is a colour choice. A typeless
// format that names several readings (16 and 32 bits a channel: float, unorm, snorm, uint, sint;
// depth/stencil planes; block-compressed data) is refused instead of guessed. A typed format
// passes unchanged. DXGI_FORMAT_UNKNOWN means refused.
#pragma once

#include <d3d12.h>

namespace x2w_startup {
inline DXGI_FORMAT present_view_format(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_UNKNOWN: return DXGI_FORMAT_UNKNOWN;
    default: return format;
    }
}

inline bool present_surface_supported(const D3D12_RESOURCE_DESC& desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.SampleDesc.Count == 1 && desc.Width != 0 && desc.Height != 0 &&
           (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
           present_view_format(desc.Format) != DXGI_FORMAT_UNKNOWN;
}
}  // namespace x2w_startup
