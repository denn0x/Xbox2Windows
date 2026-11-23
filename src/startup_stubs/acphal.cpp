// Desktop AcpHal.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "AcpHal.dll"
#define X2W_TRAP_MODULE_ID 1
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

X2W_TRAP_EXPORT(0, AcpHalAllocateShapeContexts)
X2W_TRAP_EXPORT(1, AcpHalCreate)
X2W_TRAP_EXPORT(2, AcpHalReleaseShapeContexts)
X2W_TRAP_EXPORT(3, ApuAlloc)
X2W_TRAP_EXPORT(5, ApuFree)
X2W_TRAP_EXPORT(6, ApuIsVirtualAddressValid)
X2W_TRAP_EXPORT(7, ApuMapApuAddress)
X2W_TRAP_EXPORT(8, ApuMapVirtualAddress)

// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed). With X2W_AUDIO_PERMISSIVE=1:
// ApuCreateHeap(cachedSize, nonCachedSize) -> HRESULT (call shape from the title's audio start; the
// result is tested for failure). One committed region holds both parts; APU addresses will be
// offsets into it. Otherwise this traps.
namespace x2w_apu {
unsigned char* heap_base;
std::uint64_t heap_bytes;
}

extern "C" __declspec(noinline) long ApuCreateHeap(std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9)
{
    if (!x2w_local::env_on("X2W_AUDIO_PERMISSIVE"))
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuCreateHeap", 4, rcx, rdx, r8, r9, _ReturnAddress());
    const std::uint64_t cached = static_cast<std::uint32_t>(rcx), non_cached = static_cast<std::uint32_t>(rdx);
    long hr = 0;
    if (!x2w_apu::heap_base) {
        const std::uint64_t bytes = ((cached + non_cached) + 0xFFFF) & ~std::uint64_t{0xFFFF};
        x2w_apu::heap_base = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        x2w_apu::heap_bytes = x2w_apu::heap_base ? bytes : 0;
        if (!x2w_apu::heap_base) hr = static_cast<long>(0x8007000E);  // E_OUTOFMEMORY
    }
    x2w_local::logf("x2w-audio ApuCreateHeap cached=0x%llx non_cached=0x%llx -> 0x%08lx base=%p", cached, non_cached,
                    static_cast<unsigned long>(hr), static_cast<void*>(x2w_apu::heap_base));
    return hr;
}
