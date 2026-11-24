#pragma once

#include <d3d12.h>

#include <climits>
#include <cstddef>
#include <cstdint>

// A logical address window backed by a byte range of an already-owned buffer.
// The caller keeps the resource alive. resource_offset is the window's first
// byte within the resource.
struct RawUavWindow {
    UINT64 logical_base;
    UINT64 bytes;
    UINT64 resource_offset;
    UINT64 resource_width;
};

// A store resolved to one window: the raw UAV that covers exactly that window
// and the byte offset of the store inside it.
struct RawUavStore {
    std::size_t window;
    UINT offset;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view;
};

// Resolve a 4-byte (scalar) or 16-byte (four-DWORD) store at a logical address.
// Window bases are aligned to the raw view's 16-byte boundary. The whole
// store must fit in exactly one window with a representable shader offset.
// The output is assigned only after validation succeeds.
inline HRESULT resolve_raw_uav_store(const RawUavWindow* windows, std::size_t count, UINT64 address,
                                     UINT store_bytes, RawUavStore& result)
{
    constexpr UINT64 element_bytes = sizeof(UINT32);
    if (!windows || (store_bytes != 4 && store_bytes != 16) || address % store_bytes) return E_INVALIDARG;
    std::size_t selected = count;
    for (std::size_t i = 0; i < count; ++i) {
        const RawUavWindow& window = windows[i];
        if (!window.bytes || window.bytes % element_bytes || window.bytes / element_bytes > UINT_MAX ||
            window.logical_base % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT ||
            window.resource_offset % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT ||
            window.bytes - 1 > UINT64_MAX - window.logical_base ||
            window.resource_offset > window.resource_width ||
            window.bytes > window.resource_width - window.resource_offset)
            return E_INVALIDARG;

        if (address < window.logical_base) {
            if (window.logical_base - address < store_bytes) return E_INVALIDARG;
            continue;
        }
        const UINT64 offset = address - window.logical_base;
        if (offset >= window.bytes) continue;
        if (selected != count || offset > UINT_MAX || store_bytes > window.bytes - offset) return E_INVALIDARG;
        selected = i;
    }
    if (selected == count) return E_INVALIDARG;

    const RawUavWindow& window = windows[selected];
    result = {};
    result.window = selected;
    result.offset = static_cast<UINT>(address - window.logical_base);
    result.view.Format = DXGI_FORMAT_R32_TYPELESS;
    result.view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    result.view.Buffer.FirstElement = window.resource_offset / element_bytes;
    result.view.Buffer.NumElements = static_cast<UINT>(window.bytes / element_bytes);
    result.view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    return S_OK;
}
