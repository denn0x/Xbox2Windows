#pragma once

#include <d3d12.h>

namespace x2w_local {
inline bool needs_null_unorm_fallback(ID3D12Device* device, DXGI_FORMAT original, D3D12_FORMAT_SUPPORT1 required)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{original};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        (support.Support1 & required))
        return false;
    support = {DXGI_FORMAT_R8G8B8A8_UNORM};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & required);
}

inline bool adapt_null_view(ID3D12Device* device, bool has_resource, bool has_counter,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC& source,
                            D3D12_UNORDERED_ACCESS_VIEW_DESC& desktop)
{
    if (has_resource || has_counter || source.ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2D ||
        source.Format != DXGI_FORMAT_B5G5R5A1_UNORM ||
        !needs_null_unorm_fallback(device, source.Format, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
        return false;
    desktop = source;
    desktop.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

inline bool adapt_null_view(ID3D12Device* device, bool has_resource, const D3D12_RENDER_TARGET_VIEW_DESC& source,
                            D3D12_RENDER_TARGET_VIEW_DESC& desktop)
{
    if (has_resource || source.ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D ||
        source.Format != DXGI_FORMAT_B4G4R4A4_UNORM ||
        !needs_null_unorm_fallback(device, source.Format, D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
        return false;
    desktop = source;
    desktop.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}
}
