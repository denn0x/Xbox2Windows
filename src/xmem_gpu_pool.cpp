// LOCAL: see xmem_gpu_pool.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "xmem_gpu_pool.h"

#include "startup_stubs/local_log.h"

#include <cwchar>

namespace x2w_gpu_pool {
namespace {
bool enabled() noexcept
{
    static const bool on = [] {
        char text[8]{};
        const DWORD n = GetEnvironmentVariableA("X2W_XMEM_GPU_POOL", text, sizeof(text));
        return !(n == 1 && text[0] == '0');
    }();
    return on;
}
LONG next_index = 0;
LONG misses = 0;
}  // namespace

HANDLE take(ULONG64 bytes) noexcept
{
    if (!enabled()) return nullptr;
    const LONG index = InterlockedIncrement(&next_index) - 1;
    wchar_t name[64];
    swprintf(name, 64, L"Local\\x2w-gpu-slab-%ld", index);
    const HANDLE section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!section) {
        const DWORD error = GetLastError();
        if (InterlockedIncrement(&misses) <= 2)
            x2w_local::logf("x2w-xmem GPU slab pool: %ls not available (error %lu): a fresh pagefile section instead "
                            "(its commit stays charged after this process; tools/startup/gpu_slab_pool.py ensure)",
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
        x2w_local::logf("x2w-xmem GPU slab pool: %ls is smaller than 0x%llx bytes: a fresh pagefile section instead", name,
                        static_cast<unsigned long long>(bytes));
        return nullptr;
    }
    x2w_local::logf("x2w-xmem GPU slab pool: %ls taken for a GPU slab", name);
    return section;
}
}  // namespace x2w_gpu_pool
