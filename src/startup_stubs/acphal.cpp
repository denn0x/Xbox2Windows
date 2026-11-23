// Desktop AcpHal.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "AcpHal.dll"
#define X2W_TRAP_MODULE_ID 1
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

#include <array>
#include <utility>

X2W_TRAP_EXPORT(2, AcpHalReleaseShapeContexts)

// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed). With X2W_AUDIO_PERMISSIVE=1 the exports
// below stand in for the audio hardware layer; otherwise they trap. Nothing decodes or mixes: the
// goal is that the title's audio start-up succeeds.
namespace {
using u64 = std::uint64_t;

bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }

// APU heap: one committed region; an APU address is an offset into it. A bump allocator serves the
// context arrays (and later ApuAlloc, once its call shape is seen).
unsigned char* heap_base;
u64 heap_bytes;
u64 heap_used;
SRWLOCK heap_lock = SRWLOCK_INIT;

unsigned char* heap_take(u64 bytes, u64 alignment)
{
    AcquireSRWLockExclusive(&heap_lock);
    const u64 start = (heap_used + alignment - 1) & ~(alignment - 1);
    unsigned char* result = heap_base && start + bytes <= heap_bytes ? heap_base + start : nullptr;
    if (result) heap_used = start + bytes;
    ReleaseSRWLockExclusive(&heap_lock);
    return result;
}

// AcpHal object (not COM: slot 0 is the first method). Seen: slot 0 (0x1000, 0x1000) -> HRESULT, a
// connect with two queue lengths; slot 2 (type, id, frame, &value, 0) -> HRESULT, a command submit.
// Both succeed and are logged; every other slot traps and names itself.
template <int N>
__declspec(noinline) u64 hal_trap(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    char name[48];
    std::snprintf(name, sizeof(name), "AcpHal::slot%d", N);
    x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, 0x40 + N, rcx, rdx, r8, r9, _ReturnAddress());
}
long __stdcall hal_connect(void*, unsigned commands, unsigned messages)
{
    x2w_local::logf("x2w-audio AcpHal::slot0(connect) command_queue=%u message_queue=%u -> S_OK", commands, messages);
    return 0;
}
long __stdcall hal_submit(void*, unsigned type, u64 id, unsigned frame, const unsigned* input, u64 extra)
{
    static LONG calls;
    if (x2w_local::first_calls(calls, 16))
        x2w_local::logf("x2w-audio AcpHal::slot2(submit) type=%u id=0x%llx frame=0x%x input=%p [0x%x] extra=0x%llx -> S_OK", type,
                        id, frame, static_cast<const void*>(input),
                        input && x2w_local::readable(input, 4) ? *input : 0u, extra);
    return 0;
}

template <std::size_t... I>
auto hal_table(std::index_sequence<I...>)
{
    return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&hal_trap<static_cast<int>(I)>)...};
}
}  // namespace

// ApuCreateHeap(cachedSize, nonCachedSize) -> HRESULT (the caller tests for failure).
extern "C" __declspec(noinline) long ApuCreateHeap(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuCreateHeap", 4, rcx, rdx, r8, r9, _ReturnAddress());
    const u64 cached = static_cast<std::uint32_t>(rcx), non_cached = static_cast<std::uint32_t>(rdx);
    long hr = 0;
    if (!heap_base) {
        const u64 bytes = ((cached + non_cached) + 0xFFFF) & ~u64{0xFFFF};
        heap_base = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        heap_bytes = heap_base ? bytes : 0;
        heap_used = 0x10000;  // APU address 0 stays unused
        if (!heap_base) hr = static_cast<long>(0x8007000E);  // E_OUTOFMEMORY
    }
    x2w_local::logf("x2w-audio ApuCreateHeap cached=0x%llx non_cached=0x%llx -> 0x%08lx base=%p", cached, non_cached,
                    static_cast<unsigned long>(hr), static_cast<void*>(heap_base));
    return hr;
}

// AcpHalAllocateShapeContexts(contexts*) -> HRESULT. Layout from the caller: six UINT32 counts at
// +0x00 (the fifth, at +0x10, is the XMA count: 350), then six array pointers at +0x18; the caller
// clears count << 6 bytes at the pointer at +0x38, so an XMA context is 64 bytes. The six UINT32
// after the pointers (+0x48) are taken to be the arrays' APU addresses. Only the XMA element size is
// known; a nonzero count of another type fails so the gap is visible.
extern "C" __declspec(noinline) long AcpHalAllocateShapeContexts(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "AcpHalAllocateShapeContexts", 0, rcx, rdx, r8, r9,
                       _ReturnAddress());
    auto* block = reinterpret_cast<unsigned char*>(rcx);
    if (!block) return static_cast<long>(0x80004003);
    const auto* counts = reinterpret_cast<const std::uint32_t*>(block);
    auto** arrays = reinterpret_cast<unsigned char**>(block + 0x18);
    auto* apu = reinterpret_cast<std::uint32_t*>(block + 0x48);
    x2w_local::logf("x2w-audio AcpHalAllocateShapeContexts counts=%u,%u,%u,%u,%u,%u", counts[0], counts[1], counts[2],
                    counts[3], counts[4], counts[5]);
    for (int i = 0; i < 6; ++i) {
        if (!counts[i]) continue;
        if (i != 4) {
            x2w_local::logf("x2w-audio   context type %d requested (%u): element size unknown -> E_NOTIMPL", i, counts[i]);
            return static_cast<long>(0x80004001);
        }
        unsigned char* memory = heap_take(u64{counts[i]} * 64, 64);
        if (!memory) return static_cast<long>(0x8007000E);
        arrays[i] = memory;
        apu[i] = static_cast<std::uint32_t>(memory - heap_base);
        x2w_local::logf("x2w-audio   XMA contexts: %u x 64 bytes at %p (apu 0x%x)", counts[i], static_cast<void*>(memory),
                        apu[i]);
    }
    return 0;
}

// AcpHalCreate(out**) -> HRESULT.
extern "C" __declspec(noinline) long AcpHalCreate(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "AcpHalCreate", 1, rcx, rdx, r8, r9, _ReturnAddress());
    static auto table = hal_table(std::make_index_sequence<48>{});
    static bool ready = false;
    if (!ready) {
        table[0] = reinterpret_cast<void*>(&hal_connect);
        table[2] = reinterpret_cast<void*>(&hal_submit);
        ready = true;
    }
    static void* const* const object = table.data();
    if (!rcx) return static_cast<long>(0x80004003);
    *reinterpret_cast<void* const* const**>(rcx) = &object;
    x2w_local::logf("x2w-audio AcpHalCreate -> stand-in object %p", static_cast<const void*>(&object));
    return 0;
}

// ApuAlloc(&virtualAddress, apuAddress* or null, size, alignment, flags) -> HRESULT (first call:
// size 0x222e00, alignment 0x800, null APU-address output). Bump allocation; nothing is reused.
extern "C" __declspec(noinline) long ApuAlloc(void** virtual_address, std::uint32_t* apu_address, u64 size, u64 alignment,
                                              u64 flags)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuAlloc", 3, reinterpret_cast<u64>(virtual_address),
                       reinterpret_cast<u64>(apu_address), size, alignment, _ReturnAddress());
    const u64 bytes = static_cast<std::uint32_t>(size);
    u64 align = static_cast<std::uint32_t>(alignment);
    if (align < 16 || (align & (align - 1))) align = 16;
    unsigned char* memory = virtual_address ? heap_take(bytes, align) : nullptr;
    const long hr = memory ? 0 : static_cast<long>(virtual_address ? 0x8007000E : 0x80004003);
    if (virtual_address) *virtual_address = memory;
    if (memory && apu_address) *apu_address = static_cast<std::uint32_t>(memory - heap_base);
    static LONG calls;
    if (FAILED(hr) || x2w_local::first_calls(calls, 8))
        x2w_local::logf("x2w-audio ApuAlloc size=0x%llx align=0x%llx flags=0x%llx apu_out=%p -> 0x%08lx %p", bytes, align,
                        static_cast<u64>(static_cast<std::uint32_t>(flags)), static_cast<void*>(apu_address),
                        static_cast<unsigned long>(hr), static_cast<void*>(memory));
    return hr;
}

// The four below take their shapes from their names; none has been seen yet, so each logs its first calls.
extern "C" __declspec(noinline) long ApuFree(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuFree", 5, reinterpret_cast<u64>(virtual_address), 0, 0, 0,
                       _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuFree %p (not reused)", virtual_address);
    return 0;
}

extern "C" __declspec(noinline) int ApuIsVirtualAddressValid(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuIsVirtualAddressValid", 6,
                       reinterpret_cast<u64>(virtual_address), 0, 0, 0, _ReturnAddress());
    auto* p = static_cast<unsigned char*>(virtual_address);
    const int valid = heap_base && p >= heap_base && p < heap_base + heap_bytes;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuIsVirtualAddressValid %p -> %d", virtual_address, valid);
    return valid;
}

extern "C" __declspec(noinline) void* ApuMapApuAddress(u64 apu_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuMapApuAddress", 7, apu_address, 0, 0, 0, _ReturnAddress());
    const u64 offset = static_cast<std::uint32_t>(apu_address);
    void* result = heap_base && offset && offset < heap_bytes ? heap_base + offset : nullptr;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuMapApuAddress 0x%llx -> %p", offset, result);
    return result;
}

extern "C" __declspec(noinline) std::uint32_t ApuMapVirtualAddress(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuMapVirtualAddress", 8, reinterpret_cast<u64>(virtual_address),
                       0, 0, 0, _ReturnAddress());
    auto* p = static_cast<unsigned char*>(virtual_address);
    const std::uint32_t result = heap_base && p >= heap_base && p < heap_base + heap_bytes
        ? static_cast<std::uint32_t>(p - heap_base) : 0;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) x2w_local::logf("x2w-audio ApuMapVirtualAddress %p -> 0x%x", virtual_address, result);
    return result;
}
