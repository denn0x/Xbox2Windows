// Test-only recording provider for the acphal.dll HAL, shape-context and heap exports:
//   acphal.dll!ApuCreateHeap
//   acphal.dll!AcpHalCreate
//   acphal.dll!AcpHalAllocateShapeContexts
//   acphal.dll!AcpHalReleaseShapeContexts
// plus acphal.dll!ApuAlloc and acphal.dll!ApuFree, so that heap creation has an
// observable effect through the public allocation call shape.
//
// Public evidence:
// - ApuAlloc and ApuFree follow public MIT call sites in microsoft/DirectXTK12 at
//   f3555e3 (SoundEffect.cpp, WaveBankReader.cpp, SoundStreamInstance.cpp).
// - DirectXTK12 names ApuCreateHeap only in an ApuAlloc failure diagnostic, as the
//   heap ApuAlloc draws from. It has no public call site or signature.
// - The MIT GDK Learn index (microsoft/XBOX-GDK-dotnet at f8c3438) lists AcpHal and
//   ShapeContexts member pages by name only.
//
// Every argument list for the four exports above is therefore a fixture contract
// chosen for this test, not console evidence:
//   HRESULT ApuCreateHeap(UINT32 size);
//   HRESULT AcpHalCreate(void** hal);
//   HRESULT AcpHalAllocateShapeContexts(void* hal, UINT32 count, void** contexts);
//   HRESULT AcpHalReleaseShapeContexts(void* hal, void* contexts);
// The heap limit, the 16-slot context pool, the 64-byte slot size, the single HAL
// instance and every refusal code are proposed desktop policy for this fixture.
// This is not a production acphal.dll shim.
//
// ACPHAL_TEST_FAULTY builds a negative-control provider whose shape-context
// allocation reports success without clearing the returned slots.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace {
constexpr std::uint32_t arena_size = 0x10000; // 64 KiB upper bound for ApuCreateHeap
constexpr std::uint32_t heap_granularity = 0x1000;
constexpr std::uint32_t slot_size = 64;
constexpr std::uint32_t slot_count = 16;
constexpr std::uint32_t hal_magic = 0x4C414841u; // "AHAL"
constexpr int max_records = 32;

const HRESULT already_exists = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
const HRESULT not_valid_state = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

alignas(4096) unsigned char arena[arena_size];
alignas(64) unsigned char pool[slot_count * slot_size];

struct Hal {
    std::uint32_t magic;
    std::uint32_t live_allocations;
};

struct ContextRecord {
    std::uint32_t first_slot;
    std::uint32_t count;
    bool active;
};

SRWLOCK lock = SRWLOCK_INIT;

// Heap state.
std::uint32_t heap_limit = 0;
std::uint32_t bump = 0;

// HAL state.
Hal hal_instance = {};
bool hal_created = false;
bool slot_used[slot_count] = {};
ContextRecord records[max_records] = {};
int record_count = 0;

// Counters read back by the checker.
std::uint32_t heap_create_calls = 0;
std::uint32_t alloc_calls = 0;
std::uint32_t free_calls = 0;
std::uint32_t hal_create_calls = 0;
std::uint32_t shape_alloc_calls = 0;
std::uint32_t shape_alloc_successes = 0;
std::uint32_t shape_release_calls = 0;
std::uint32_t double_release_refusals = 0;
std::uint32_t handle_refusals = 0;

bool is_live_hal(const void* hal)
{
    return hal_created && hal == &hal_instance && hal_instance.magic == hal_magic;
}

unsigned char* slot_address(std::uint32_t slot)
{
    return pool + static_cast<std::size_t>(slot) * slot_size;
}

// First fit over contiguous free slots.
bool find_run(std::uint32_t count, std::uint32_t* first)
{
    for (std::uint32_t start = 0; start + count <= slot_count; ++start) {
        bool run_free = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (slot_used[start + i]) {
                run_free = false;
                break;
            }
        }
        if (run_free) {
            *first = start;
            return true;
        }
    }
    return false;
}

ContextRecord* find_record(const void* contexts)
{
    // Prefer an active record so a reused address resolves to the live allocation.
    ContextRecord* released = nullptr;
    for (int i = record_count - 1; i >= 0; --i) {
        if (slot_address(records[i].first_slot) == contexts) {
            if (records[i].active) {
                return &records[i];
            }
            if (!released) {
                released = &records[i];
            }
        }
    }
    return released;
}
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI ApuCreateHeap(UINT32 size)
{
    AcquireSRWLockExclusive(&lock);
    ++heap_create_calls;
    HRESULT result;
    if (size == 0 || size > arena_size || (size % heap_granularity) != 0) {
        result = E_INVALIDARG;
    } else if (heap_limit != 0) {
        result = already_exists;
    } else {
        heap_limit = size;
        bump = 0;
        result = S_OK;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI ApuAlloc(
    void** virtual_address,
    void* second_argument,
    UINT32 size,
    UINT32 alignment)
{
    AcquireSRWLockExclusive(&lock);
    ++alloc_calls;
    HRESULT result;
    if (!virtual_address || second_argument || size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        result = E_INVALIDARG;
    } else if (heap_limit == 0) {
        result = not_valid_state;
    } else {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(arena);
        const std::uintptr_t start = (base + bump + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1);
        const std::uint64_t end = static_cast<std::uint64_t>(start - base) + size;
        if (end > heap_limit) {
            result = E_OUTOFMEMORY;
        } else {
            bump = static_cast<std::uint32_t>(end);
            *virtual_address = reinterpret_cast<void*>(start);
            result = S_OK;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

extern "C" __declspec(dllexport) void WINAPI ApuFree(void*)
{
    // Bump heap: blocks are not reused, so a release is only counted.
    AcquireSRWLockExclusive(&lock);
    ++free_calls;
    ReleaseSRWLockExclusive(&lock);
}

extern "C" __declspec(dllexport) HRESULT WINAPI AcpHalCreate(void** hal)
{
    AcquireSRWLockExclusive(&lock);
    ++hal_create_calls;
    HRESULT result;
    if (!hal) {
        result = E_POINTER;
    } else if (hal_created) {
        result = already_exists;
    } else {
        hal_instance = {hal_magic, 0};
        hal_created = true;
        *hal = &hal_instance;
        result = S_OK;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI AcpHalAllocateShapeContexts(
    void* hal,
    UINT32 count,
    void** contexts)
{
    AcquireSRWLockExclusive(&lock);
    ++shape_alloc_calls;
    HRESULT result;
    std::uint32_t first = 0;
    if (!is_live_hal(hal)) {
        ++handle_refusals;
        result = E_HANDLE;
    } else if (!contexts) {
        result = E_POINTER;
    } else if (count == 0 || count > slot_count) {
        result = E_INVALIDARG;
    } else if (record_count == max_records || !find_run(count, &first)) {
        result = E_OUTOFMEMORY;
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            slot_used[first + i] = true;
        }
#ifdef ACPHAL_TEST_FAULTY
        std::memset(slot_address(first), 0xCD, static_cast<std::size_t>(count) * slot_size);
#else
        std::memset(slot_address(first), 0, static_cast<std::size_t>(count) * slot_size);
#endif
        records[record_count++] = {first, count, true};
        ++hal_instance.live_allocations;
        ++shape_alloc_successes;
        *contexts = slot_address(first);
        result = S_OK;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI AcpHalReleaseShapeContexts(void* hal, void* contexts)
{
    AcquireSRWLockExclusive(&lock);
    ++shape_release_calls;
    HRESULT result;
    if (!is_live_hal(hal)) {
        ++handle_refusals;
        result = E_HANDLE;
    } else if (ContextRecord* record = find_record(contexts)) {
        if (record->active) {
            for (std::uint32_t i = 0; i < record->count; ++i) {
                slot_used[record->first_slot + i] = false;
            }
            record->active = false;
            --hal_instance.live_allocations;
            result = S_OK;
        } else {
            ++double_release_refusals;
            result = E_INVALIDARG;
        }
    } else {
        result = E_INVALIDARG;
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

// Verification and query exports.
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_heap_create_calls(void) { return heap_create_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_heap_limit(void) { return heap_limit; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_alloc_calls(void) { return alloc_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_free_calls(void) { return free_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_hal_create_calls(void) { return hal_create_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_shape_alloc_calls(void) { return shape_alloc_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_shape_alloc_successes(void) { return shape_alloc_successes; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_shape_release_calls(void) { return shape_release_calls; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_double_release_refusals(void) { return double_release_refusals; }
extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_handle_refusals(void) { return handle_refusals; }

extern "C" __declspec(dllexport) UINT32 WINAPI get_acphal_live_shape_allocations(void)
{
    AcquireSRWLockExclusive(&lock);
    const UINT32 live = hal_created ? hal_instance.live_allocations : 0;
    ReleaseSRWLockExclusive(&lock);
    return live;
}

extern "C" __declspec(dllexport) void WINAPI reset_acphal_hal_records(void)
{
    AcquireSRWLockExclusive(&lock);
    heap_limit = 0;
    bump = 0;
    hal_instance = {};
    hal_created = false;
    for (bool& used : slot_used) {
        used = false;
    }
    for (ContextRecord& record : records) {
        record = {};
    }
    record_count = 0;
    heap_create_calls = 0;
    alloc_calls = 0;
    free_calls = 0;
    hal_create_calls = 0;
    shape_alloc_calls = 0;
    shape_alloc_successes = 0;
    shape_release_calls = 0;
    double_release_refusals = 0;
    handle_refusals = 0;
    ReleaseSRWLockExclusive(&lock);
}
