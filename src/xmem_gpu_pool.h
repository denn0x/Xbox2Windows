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
// X2W_XMEM_GPU_POOL:
//   unset / 1 / on  moves: a file-backed slab that the GPU needs is moved into the next pool section (migrate_slab).
//   all             every slab comes from the pool from the start: no file-backed slabs, no live moves; a pool
//                   section's slots are zeroed when first handed out (section_for). Pool exhausted: fresh sections.
//   0               off: fresh sections as before.
// Only one process uses the pool at a time: the first take() creates the named event Local\x2w-gpu-slab-pool-owner
// (kept open until the process ends); a process that finds it existing gets no pool sections (logged) instead of
// sharing them.
#include <windows.h>

namespace x2w_gpu_pool {
enum class Mode { off, moves, all };
Mode mode() noexcept;
// The next unused pool section of at least bytes, opened read-write, or nullptr (pool off, not running,
// exhausted, owned by another process, or too small). The contents are whatever the previous user left: the caller
// overwrites or zeroes them.
HANDLE take(ULONG64 bytes) noexcept;
}  // namespace x2w_gpu_pool
