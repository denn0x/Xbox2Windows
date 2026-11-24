// LOCAL: see xmem_gpu_pool.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "xmem_gpu_pool.h"

#include "startup_stubs/local_log.h"

#include <cwchar>

namespace x2w_gpu_pool {
namespace {
LONG next_index = 0;
LONG misses = 0;

// The pool is this process's alone: the process that creates the named event Local\x2w-gpu-slab-pool-owner owns it.
// The handle stays open until the process ends (also on a kill: the kernel closes it and the name goes away), so
// the claim does not depend on the thread that made it, unlike a mutex.
bool owner() noexcept
{
    static const bool owned = [] {
        const HANDLE claim = CreateEventW(nullptr, TRUE, FALSE, L"Local\\x2w-gpu-slab-pool-owner");
        const DWORD error = GetLastError();
        const bool ok = claim && error != ERROR_ALREADY_EXISTS;
        if (!ok) {
            if (claim) CloseHandle(claim);
            x2w_local::logf("x2w-xmem GPU slab pool: in use by another process (error %lu): fresh sections instead", error);
        }
        return ok;  // the handle is kept open on purpose
    }();
    return owned;
}
}  // namespace

Mode mode() noexcept
{
    static const Mode value = [] {
        char text[8]{};
        const DWORD n = GetEnvironmentVariableA("X2W_XMEM_GPU_POOL", text, sizeof(text));
        if (n == 1 && text[0] == '0') return Mode::off;
        if (n == 3 && (text[0] | 0x20) == 'a' && (text[1] | 0x20) == 'l' && (text[2] | 0x20) == 'l') return Mode::all;
        return Mode::moves;
    }();
    return value;
}

HANDLE take(ULONG64 bytes) noexcept
{
    if (mode() == Mode::off || !owner()) return nullptr;
    const LONG index = InterlockedIncrement(&next_index) - 1;
    wchar_t name[64];
    swprintf(name, 64, L"Local\\x2w-gpu-slab-%ld", index);
    const HANDLE section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!section) {
        const DWORD error = GetLastError();
        if (InterlockedIncrement(&misses) <= 2)
            x2w_local::logf("x2w-xmem GPU slab pool: %ls not available (error %lu): a fresh section instead "
                            "(a pagefile one opened as a GPU heap stays charged after this process; "
                            "tools/startup/gpu_slab_pool.py ensure)",
                            name, error);
        return nullptr;
    }
    // Size check through a view: the holder may run with another --mib.
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION info{};
    const bool big_enough = view && VirtualQuery(view, &info, sizeof(info)) == sizeof(info) && info.RegionSize >= bytes;
    if (view) UnmapViewOfFile(view);
    if (!big_enough) {
        CloseHandle(section);
        x2w_local::logf("x2w-xmem GPU slab pool: %ls is smaller than 0x%llx bytes: a fresh section instead", name,
                        static_cast<unsigned long long>(bytes));
        return nullptr;
    }
    static LONG taken;
    if (InterlockedIncrement(&taken) <= 64)
        x2w_local::logf("x2w-xmem GPU slab pool: %ls taken for %s", name, mode() == Mode::all ? "a slab" : "a GPU slab");
    return section;
}
}  // namespace x2w_gpu_pool
