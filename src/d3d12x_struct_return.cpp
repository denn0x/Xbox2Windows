#include "d3d12x_struct_return.h"

namespace d3d12x_struct_return {

D3D12_HEAP_PROPERTIES* get_custom_heap_properties(
    void* first, void* second, UINT node_mask, D3D12_HEAP_TYPE heap_type,
    ID3D12Device* device) noexcept
{
    if (!device || !first || !second || first == second) return nullptr;

    void* output = nullptr;
    if (first == device) {
        output = second;
    } else if (second == device) {
        output = first;
    } else {
        return nullptr;
    }

    auto* properties = static_cast<D3D12_HEAP_PROPERTIES*>(output);
    *properties = device->GetCustomHeapProperties(node_mask, heap_type);
    return properties;
}

}  // namespace d3d12x_struct_return
