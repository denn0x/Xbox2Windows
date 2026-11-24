#pragma once

#include <d3d12.h>

namespace x2w_startup {
// Takes ownership of one reference to the desktop device chosen by the caller.
void attach_device(ID3D12Device* device, void** wrapped);

// Internal desktop build preparation. The caller retains the registered resources
// through execution and supplies prebuild sizes for these exact BLAS/TLAS inputs.
HRESULT resolve_placed_build(void* guest_device, UINT64 dest, UINT64 scratch,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& sizes,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& build);
}
