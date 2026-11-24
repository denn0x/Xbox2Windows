#pragma once

#include <d3d12.h>

namespace d3d12x_struct_return {

// The leading four arguments model the device method's register arguments.
// The caller supplies the live device separately so neither candidate needs
// to be dereferenced to identify the device. The other argument must point to
// writable D3D12_HEAP_PROPERTIES storage. Returns that pointer on success.
D3D12_HEAP_PROPERTIES* get_custom_heap_properties(
    void* first, void* second, UINT node_mask, D3D12_HEAP_TYPE heap_type,
    ID3D12Device* device) noexcept;

}  // namespace d3d12x_struct_return
