#pragma once
// LOCAL accounting (out/gamerun/runtime_work/NOTES.md): where a startup run's commit goes.
//
// xmem.dll starts a reporter thread on its first physical-page or virtual-alloc call. Every 2 s for
// the first 30 s, then every 10 s, it logs one "x2w-mem" line with:
//   - the system commit charge and its change since xmem.dll was loaded (everything this run costs
//     the host: private commit, the page slabs, driver allocations, the debugger);
//   - this process's private commit (the part a job memory limit sees);
//   - the 256 MiB page slabs (pagefile-backed sections, charged to the system, not to the process);
//   - the commits the memory stand-in made on the title's behalf, by path;
//   - the process heaps' committed bytes;
//   - the GPU memory this process uses per adapter (DXGI local and non-local segments);
//   - byte counters published by other stand-ins (x2w_memory_counters export, see below).
// X2W_MEM_CENSUS_AT=<s>[,<s>...] adds an "x2w-mem census" at those seconds: a VirtualQuery walk of
// the address space outside the stand-in's own page reservations (where a query is slow), giving
// private committed bytes by kind and the largest private allocations.
// X2W_MEM_REPORT=0 turns the reporter off.
#include <windows.h>

namespace x2w_census {
// The title's startup blocks (commit_startup_block): the census reports them as their own kind.
void note_block(void* base, SIZE_T bytes) noexcept;
// Starts the reporter once (cheap after the first call).
void start() noexcept;
}  // namespace x2w_census

// Other stand-in DLLs may export this; the reporter adds their counters to its line:
//   extern "C" int WINAPI x2w_memory_counters(char* text, int capacity);
// writes "name=<MiB> ..." pairs (bytes held now), returns the length.
using X2wMemoryCounters = int(WINAPI*)(char* text, int capacity);

// Exported by xmem.dll (xmem_mapped_pages.cpp).
struct X2wXmemStats {
    ULONGLONG slabs, slab_bytes, live_pages, free_slots, slab_budget_bytes, slab_refusals;
    ULONGLONG startup_block_bytes, startup_block_calls, permissive_commit_bytes, permissive_commit_calls;
    ULONGLONG reservations;
    ULONGLONG file_slabs, migrations, migration_failures;  // X2W_XMEM_SLAB_DIR
};
extern "C" BOOL WINAPI x2w_xmem_memory_stats(X2wXmemStats* stats);
// Copies up to capacity (base, bytes) pairs of the stand-in's page reservations; returns the count
// of all of them (may exceed capacity).
extern "C" SIZE_T WINAPI x2w_xmem_reserved_ranges(ULONG_PTR* pairs, SIZE_T capacity);
