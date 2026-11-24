#pragma once
// LOCAL (out/gamerun/memleak_work/NOTES.md): GPU slabs from a long-lived pool of named pagefile sections.
//
// Measured on the RX 9070 XT (memleak_work/heaprepro.cpp): a pagefile-backed section that has been opened as a
// D3D12 heap (OpenExistingHeapFromFileMapping) keeps its whole commit charge after the process is gone, until the
// next reboot, however the process ended. FromAddress heaps over private memory do not. A slab that xmem moves
// to the pagefile for the GPU (migrate_slab) is exactly such a section, so every run lost (moved slabs) x 256 MiB.
//
// tools/startup/gpu_slab_pool.py holds named sections Local\x2w-gpu-slab-<i> across runs. take() opens the next
// one, so later runs reuse the same section objects and the host's loss stays at the pool size.
// X2W_XMEM_GPU_POOL=0 turns it off (fresh sections as before). Without the holder take() returns nullptr.
#include <windows.h>

namespace x2w_gpu_pool {
// The next unused pool section of at least bytes, opened read-write, or nullptr (pool off, not running,
// exhausted, or too small). The contents are whatever the previous user left: the caller overwrites them.
HANDLE take(ULONG64 bytes) noexcept;
}  // namespace x2w_gpu_pool
