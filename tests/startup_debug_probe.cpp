// Input for the startup debugger's diagnostics check (tests/startup_debug_check.py). It reaches two
// breakpoints in its own image, the way a retail image keeps soft asserts, reserves one large
// range, stays alive for a moment and exits 0. No GPU and no other input is involved.
#include <windows.h>

#include <cstdio>

namespace {
volatile int reached;

__declspec(noinline) void first_assert()
{
    reached = 1;
    __debugbreak();
}

__declspec(noinline) void second_assert()
{
    reached = 2;
    __debugbreak();
}

__declspec(noinline) void* reserve_large()
{
    // 256 MiB, reserved only: nothing is committed.
    return VirtualAlloc(nullptr, SIZE_T{256} << 20, MEM_RESERVE, PAGE_NOACCESS);
}
}  // namespace

int main()
{
    first_assert();
    second_assert();
    void* const range = reserve_large();
    std::printf("debug-probe reserved %s\n", range ? "ok" : "failed");
    std::fflush(stdout);
    Sleep(3000);
    return 0;
}
