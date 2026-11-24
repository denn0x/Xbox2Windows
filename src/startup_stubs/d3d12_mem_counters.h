#pragma once
// LOCAL memcut (out/gamerun/memcut_work/NOTES.md): cheap counters for the memory attribution of a
// startup run. The hooks in d3d12_xs_device.cpp / d3d12_draw.inl / d3d12_capture.inl are one line
// each; d3d12_mem_counters.inl reads these and the wrapper registry for the x2w_memory_counters
// export (xmem_census.h), which the x2w-mem reporter prints in every x2w-mem line.
#include <windows.h>
#include <psapi.h>
#include <cstdint>

namespace x2w_memcut {
struct Counters {
    volatile LONG64 ring_bytes;        // upload-ring bytes reserved since the start (rounded to 512)
    volatile LONG64 ring_frame_bytes;  // ... in the frame being recorded
    volatile LONG64 ring_frame_peak;   // ... largest finished frame so far
    volatile LONG ring_frame;
    volatile LONG64 ring_refused;      // requests over the per-copy cap (ring / 4) or without a ring
    volatile LONG64 copy_frame_bytes;  // frame_copy snapshots (CPU heap) in the frame being recorded
    volatile LONG64 copy_frame_peak;
    volatile LONG copy_frame;
    volatile LONG64 bound_after_release;   // textures bound for a draw after the title released them
    volatile LONG64 viewed_after_release;  // views created on a resource the title had released
};
inline Counters& counters()
{
    static Counters c;
    return c;
}
// Adds bytes to the current frame's sum; the first note of a new frame folds the old sum into peak.
// Approximate under races (a statistic, never used for a decision).
inline void per_frame(volatile LONG64& now, volatile LONG64& peak, volatile LONG& frame, LONG current, LONG64 bytes)
{
    if (InterlockedExchange(&frame, current) != current) {
        const LONG64 last = InterlockedExchange64(&now, 0);
        LONG64 seen = peak;
        while (last > seen) {
            const LONG64 previous = InterlockedCompareExchange64(&peak, last, seen);
            if (previous == seen) break;
            seen = previous;
        }
    }
    InterlockedAdd64(&now, bytes);
}
inline void note_ring(LONG frame, std::uint64_t bytes)
{
    Counters& c = counters();
    InterlockedAdd64(&c.ring_bytes, static_cast<LONG64>(bytes));
    per_frame(c.ring_frame_bytes, c.ring_frame_peak, c.ring_frame, frame, static_cast<LONG64>(bytes));
}
inline void note_ring_refused() { InterlockedIncrement64(&counters().ring_refused); }
inline void note_copy(LONG frame, std::uint64_t bytes)
{
    Counters& c = counters();
    per_frame(c.copy_frame_bytes, c.copy_frame_peak, c.copy_frame, frame, static_cast<LONG64>(bytes));
}
inline void note_after_release(bool bound)
{
    InterlockedIncrement64(bound ? &counters().bound_after_release : &counters().viewed_after_release);
}
}  // namespace x2w_memcut
