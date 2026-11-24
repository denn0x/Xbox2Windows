#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace cpu_srv {

// Refresh a caller-owned upload buffer before submitting work that reads its
// SRV. The caller serializes refresh/submit, keeps the source stable during
// this call and supplies the fence point covering all preceding GPU reads.
// With multiple queues, join the previous readers before using a single point.
// A null fence with value zero denotes the first use (or external retirement).
inline HRESULT refresh(ID3D12Resource* upload, SIZE_T destination_offset,
                       const void* source, SIZE_T bytes, ID3D12Fence* previous_fence,
                       UINT64 previous_value)
{
    if (!upload || !source || !bytes || (!previous_fence && previous_value)) return E_INVALIDARG;
    const auto desc = upload->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        destination_offset > desc.Width || bytes > desc.Width - destination_offset) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    HRESULT hr = upload->GetHeapProperties(&heap, &flags);
    if (FAILED(hr)) return hr;
    if (heap.Type != D3D12_HEAP_TYPE_UPLOAD) return E_INVALIDARG;
    if (previous_fence) {
        const UINT64 done = previous_fence->GetCompletedValue();
        if (done == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (done < previous_value) return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    // A source may straddle an unreadable page. Stage the complete read before
    // changing the live upload so a rejected refresh preserves its old contents.
    auto copy = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[bytes]);
    if (!copy) return E_OUTOFMEMORY;
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(), source, copy.get(), bytes, &copied))
        return HRESULT_FROM_WIN32(GetLastError());
    if (copied != bytes) return HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY);
    const D3D12_RANGE no_read{0, 0};
    void* mapped{};
    hr = upload->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return hr;
    std::memcpy(static_cast<std::byte*>(mapped) + destination_offset, copy.get(), bytes);
    const D3D12_RANGE written{destination_offset, destination_offset + bytes};
    upload->Unmap(0, &written);
    return S_OK;
}

} // namespace cpu_srv
