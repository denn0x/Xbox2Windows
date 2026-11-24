// LOCAL (perf_work/STARTUP.md), opt-in X2W_D3D12_PLACED_POOL=1: small buffers that Device slot 133
// cannot alias with title memory (the "not aliased" fallback) are placed on default heaps that the
// stand-in creates in 64 MiB blocks, instead of one committed resource (one kernel allocation) each.
// The start-up loop creates ~2,800 such buffers (0x410 and 0x1000 bytes) one after the other.
//
// Each buffer takes a fresh 64 KiB slot (the desktop placement alignment, which is also the size a
// committed buffer this small occupies). Slots are never reused, so a buffer starts zero-filled as a
// committed one does; the memory of a released buffer is not returned until the process ends.
// X2W_D3D12_PLACED_POOL_MIB (default 512) caps the pool; past it, and for buffers over 64 KiB, the
// caller falls back to a committed resource. Unmeasured: whether placement is cheaper here.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdlib>

namespace x2w_startup {

class PlacedPool {
public:
    static constexpr std::uint64_t slot_bytes = std::uint64_t{64} << 10;
    static constexpr std::uint64_t heap_bytes = std::uint64_t{64} << 20;

    // S_OK with *out set, or a failure (pool off, full, buffer too large, or a desktop error): the
    // caller then creates a committed resource as before.
    HRESULT create(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, ID3D12Resource** out)
    {
        *out = nullptr;
        if (!enabled() || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width == 0 || desc.Width > slot_bytes)
            return E_NOTIMPL;
        AcquireSRWLockExclusive(&lock_);
        HRESULT hr = S_OK;
        if (!heap_ || used_ + slot_bytes > heap_bytes) {
            if (total_ + heap_bytes > budget()) {
                hr = E_OUTOFMEMORY;
            } else {
                D3D12_HEAP_DESC heap_desc{};
                heap_desc.SizeInBytes = heap_bytes;
                heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
                ID3D12Heap* heap = nullptr;
                hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
                if (SUCCEEDED(hr)) {
                    heap_ = heap;  // kept for the life of the process (placed buffers also hold it)
                    used_ = 0;
                    total_ += heap_bytes;
                    ++heaps_;
                }
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC placed = desc;
            placed.Alignment = 0;
            hr = device->CreatePlacedResource(heap_, used_, &placed, state, nullptr, IID_PPV_ARGS(out));
            if (SUCCEEDED(hr)) {
                used_ += slot_bytes;
                ++placed_;
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return hr;
    }

    std::uint64_t placed() const { return placed_; }
    std::uint64_t heaps() const { return heaps_; }

private:
    static bool enabled()
    {
        static const bool on = [] {
            char value[4]{};
            return GetEnvironmentVariableA("X2W_D3D12_PLACED_POOL", value, sizeof(value)) && value[0] == '1';
        }();
        return on;
    }
    static std::uint64_t budget()
    {
        static const std::uint64_t bytes = [] {
            char value[16]{};
            const DWORD n = GetEnvironmentVariableA("X2W_D3D12_PLACED_POOL_MIB", value, sizeof(value));
            const unsigned long mib = n && n < sizeof(value) ? std::strtoul(value, nullptr, 10) : 512ul;
            return std::uint64_t{mib} << 20;
        }();
        return bytes;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    ID3D12Heap* heap_ = nullptr;
    std::uint64_t used_ = 0, total_ = 0, heaps_ = 0, placed_ = 0;
};

}  // namespace x2w_startup
