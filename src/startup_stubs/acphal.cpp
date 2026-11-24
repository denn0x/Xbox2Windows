// Desktop AcpHal.dll trap stubs.
// Every export traps; see trap.h. Scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "AcpHal.dll"
#define X2W_TRAP_MODULE_ID 1
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

#include <array>
#include <utility>

X2W_TRAP_EXPORT(2, AcpHalReleaseShapeContexts)

// Experimental (docs/startup-runtime.md). With X2W_AUDIO_PERMISSIVE=1 the exports
// below stand in for the audio hardware layer; otherwise they trap. Nothing decodes or mixes: the
// goal is that the title's audio start-up succeeds.
namespace {
using u64 = std::uint64_t;

bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }

// APU heap: one committed region; an APU address is an offset into it. A table of extents, sorted by
// offset, tiles [heap_start, heap_bytes) and each extent is free or used. Allocation is first-fit in
// address order, so while nothing has been freed it hands out the same offsets a bump allocator would;
// a freed extent is merged with free neighbours. The table is static: when it is full a request that
// needs a split fails like an exhausted heap. All state is guarded by heap_lock.
constexpr u64 heap_start = 0x10000;  // APU address 0 stays unused
constexpr std::size_t max_extents = 4096;
struct Extent {
    u64 start, size;
    bool used;
};
unsigned char* heap_base;
u64 heap_bytes;
Extent extents[max_extents];
std::size_t extent_count;
u64 high_water;  // offsets below this were handed out once and may hold old data
SRWLOCK heap_lock = SRWLOCK_INIT;

void heap_reset() noexcept
{
    extent_count = 0;
    if (heap_bytes > heap_start) extents[extent_count++] = {heap_start, heap_bytes - heap_start, false};
    high_water = heap_start;
}

// bytes and alignment (a power of two) are checked by the callers; both stay far below 2^63.
unsigned char* heap_take(u64 bytes, u64 alignment)
{
    unsigned char* result = nullptr;
    u64 zero_from = 0, zero_bytes = 0;
    AcquireSRWLockExclusive(&heap_lock);
    for (std::size_t i = 0; heap_base && i < extent_count && !result; ++i) {
        const Extent e = extents[i];
        const u64 address_low = (reinterpret_cast<std::uintptr_t>(heap_base) + e.start) & (alignment - 1);
        const u64 pad = (alignment - address_low) & (alignment - 1);
        if (e.used || pad > e.size || bytes > e.size - pad) continue;
        const u64 tail = e.size - pad - bytes;
        const std::size_t extra = (pad ? 1 : 0) + (tail ? 1 : 0);
        if (extent_count + extra > max_extents) continue;
        std::memmove(&extents[i + 1 + extra], &extents[i + 1], (extent_count - i - 1) * sizeof(Extent));
        std::size_t at = i;
        if (pad) extents[at++] = {e.start, pad, false};
        extents[at++] = {e.start + pad, bytes, true};
        if (tail) extents[at] = {e.start + pad + bytes, tail, false};
        extent_count += extra;
        result = heap_base + e.start + pad;
        const u64 end = e.start + pad + bytes;
        if (e.start + pad < high_water) {
            zero_from = e.start + pad;
            zero_bytes = (end < high_water ? end : high_water) - zero_from;
        }
        if (end > high_water) high_water = end;
    }
    ReleaseSRWLockExclusive(&heap_lock);
    if (zero_bytes) std::memset(heap_base + zero_from, 0, static_cast<std::size_t>(zero_bytes));  // fresh pages are zero
    return result;
}

// Gives back the used extent that starts exactly at virtual_address. Anything else (outside the heap,
// inside a block, already free) is refused and leaves the table unchanged.
bool heap_give(const void* virtual_address)
{
    bool done = false;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(virtual_address);
    AcquireSRWLockExclusive(&heap_lock);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(heap_base);
    if (heap_base && address >= base + heap_start && address - base < heap_bytes) {
        const u64 offset = address - base;
        std::size_t lo = 0, hi = extent_count;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (extents[mid].start < offset) lo = mid + 1; else hi = mid;
        }
        if (lo < extent_count && extents[lo].start == offset && extents[lo].used) {
            extents[lo].used = false;
            if (lo + 1 < extent_count && !extents[lo + 1].used) {
                extents[lo].size += extents[lo + 1].size;
                std::memmove(&extents[lo + 1], &extents[lo + 2], (extent_count - lo - 2) * sizeof(Extent));
                --extent_count;
            }
            if (lo > 0 && !extents[lo - 1].used) {
                extents[lo - 1].size += extents[lo].size;
                std::memmove(&extents[lo], &extents[lo + 1], (extent_count - lo - 1) * sizeof(Extent));
                --extent_count;
            }
            done = true;
        }
    }
    ReleaseSRWLockExclusive(&heap_lock);
    return done;
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
    // Creation and publication are one locked step: base, size and the initial free extent become
    // visible together, and a repeat call finds the heap and leaves live blocks alone.
    AcquireSRWLockExclusive(&heap_lock);
    if (!heap_base) {
        const u64 bytes = ((cached + non_cached) + 0xFFFF) & ~u64{0xFFFF};
        void* memory = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (memory) {
            heap_bytes = bytes;
            heap_reset();
            heap_base = static_cast<unsigned char*>(memory);
        } else {
            hr = static_cast<long>(0x8007000E);  // E_OUTOFMEMORY
        }
    }
    unsigned char* const published = heap_base;
    ReleaseSRWLockExclusive(&heap_lock);
    x2w_local::logf("x2w-audio ApuCreateHeap cached=0x%llx non_cached=0x%llx -> 0x%08lx base=%p", cached, non_cached,
                    static_cast<unsigned long>(hr), static_cast<void*>(published));
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
    if (!block || !x2w_local::writable(block, 0x60)) return static_cast<long>(0x80004003);
    const auto* counts = reinterpret_cast<const std::uint32_t*>(block);
    auto** arrays = reinterpret_cast<unsigned char**>(block + 0x18);
    auto* apu = reinterpret_cast<std::uint32_t*>(block + 0x48);
    x2w_local::logf("x2w-audio AcpHalAllocateShapeContexts counts=%u,%u,%u,%u,%u,%u", counts[0], counts[1], counts[2],
                    counts[3], counts[4], counts[5]);
    bool allocated[6]{};
    auto rollback = [&]() {
        for (int j = 0; j < 6; ++j) {
            if (!allocated[j]) continue;
            heap_give(arrays[j]);
            arrays[j] = nullptr;
            apu[j] = 0;
            allocated[j] = false;
        }
    };
    for (int i = 0; i < 6; ++i) {
        if (!counts[i]) continue;
        if (i != 4) {
            x2w_local::logf("x2w-audio   context type %d requested (%u): element size unknown -> E_NOTIMPL", i, counts[i]);
            rollback();
            return static_cast<long>(0x80004001);
        }
        const u64 bytes = u64{counts[i]} * 64;
        unsigned char* memory = heap_take(bytes, 64);
        if (!memory) {
            rollback();
            return static_cast<long>(0x8007000E);
        }
        const u64 offset = static_cast<u64>(memory - heap_base);
        if (offset > 0xFFFFFFFFull) {
            heap_give(memory);
            rollback();
            return static_cast<long>(0x8007000E);
        }
        arrays[i] = memory;
        apu[i] = static_cast<std::uint32_t>(offset);
        allocated[i] = true;
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
    if (!rcx || !x2w_local::writable(reinterpret_cast<void*>(rcx), sizeof(void*))) return static_cast<long>(0x80004003);
    *reinterpret_cast<void* const* const**>(rcx) = &object;
    x2w_local::logf("x2w-audio AcpHalCreate -> stand-in object %p", static_cast<const void*>(&object));
    return 0;
}

// ApuAlloc(&virtualAddress, apuAddress* or null, size, alignment, flags) -> HRESULT (first call:
// size 0x222e00, alignment 0x800, null APU-address output). Size and alignment are taken as 32 bits.
// First fit; ApuFree returns blocks, and a reused block is zeroed as fresh heap memory is.
// A null output is E_POINTER, size 0 is E_INVALIDARG; an alignment below 16 or not a power of two means 16, and no room is E_OUTOFMEMORY; a failure leaves the heap as it was.
extern "C" __declspec(noinline) long ApuAlloc(void** virtual_address, std::uint32_t* apu_address, u64 size, u64 alignment,
                                              u64 flags)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuAlloc", 3, reinterpret_cast<u64>(virtual_address),
                       reinterpret_cast<u64>(apu_address), size, alignment, _ReturnAddress());
    const u64 bytes = static_cast<std::uint32_t>(size);
    u64 align = static_cast<std::uint32_t>(alignment);
    if (align < 16 || (align & (align - 1))) align = 16;
    const bool valid = bytes != 0;
    unsigned char* memory = virtual_address && valid ? heap_take(bytes, align) : nullptr;
    const long hr = memory ? 0 : static_cast<long>(!virtual_address ? 0x80004003 : valid ? 0x8007000E : 0x80070057);
    if (virtual_address) *virtual_address = memory;
    if (memory && apu_address) *apu_address = static_cast<std::uint32_t>(memory - heap_base);
    static LONG calls;
    if (FAILED(hr) || x2w_local::first_calls(calls, 8))
        x2w_local::logf("x2w-audio ApuAlloc size=0x%llx align=0x%llx flags=0x%llx apu_out=%p -> 0x%08lx %p", bytes, align,
                        static_cast<u64>(static_cast<std::uint32_t>(flags)), static_cast<void*>(apu_address),
                        static_cast<unsigned long>(hr), static_cast<void*>(memory));
    return hr;
}

// ApuFree(virtualAddress) -> HRESULT (every known caller discards it). Null is a no-op and S_OK; an address
// that is not the start of a live block (unknown, inside a block, already freed) is E_INVALIDARG.
extern "C" __declspec(noinline) long ApuFree(void* virtual_address)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "ApuFree", 5, reinterpret_cast<u64>(virtual_address), 0, 0, 0,
                       _ReturnAddress());
    if (!virtual_address) return 0;
    const long hr = heap_give(virtual_address) ? 0 : static_cast<long>(0x80070057);
    static LONG calls;
    if (FAILED(hr) || x2w_local::first_calls(calls, 4))
        x2w_local::logf("x2w-audio ApuFree %p -> 0x%08lx", virtual_address, static_cast<unsigned long>(hr));
    return hr;
}

// The three below take their shapes from their names; none has been seen yet, so each logs its first calls.

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
