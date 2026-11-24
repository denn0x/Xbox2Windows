#pragma once

#include <d3d12.h>

// Resolve a byte address and first element within an already-owned buffer.
// The caller keeps the resource alive and supplies its actual GPU base.
inline HRESULT make_raw_buffer_srv(D3D12_GPU_VIRTUAL_ADDRESS base, UINT64 width,
                                   D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 first_element,
                                   UINT count, D3D12_SHADER_RESOURCE_VIEW_DESC& result)
{
    constexpr UINT64 element_bytes = sizeof(UINT32);
    if (!base || address < base || address - base >= width || !count) return E_INVALIDARG;
    const UINT64 offset = address - base;
    const UINT64 available_elements = (width - offset) / element_bytes;
    if (offset % element_bytes || first_element > available_elements ||
        count > available_elements - first_element)
        return E_INVALIDARG;
    const UINT64 start = offset + first_element * element_bytes;
    if (start % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT) return E_INVALIDARG;

    result = {};
    result.Format = DXGI_FORMAT_R32_TYPELESS;
    result.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    result.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    result.Buffer.FirstElement = start / element_bytes;
    result.Buffer.NumElements = count;
    result.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return S_OK;
}
