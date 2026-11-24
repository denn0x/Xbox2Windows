// Input for the startup debugger's sampling-profiler check (tests/startup_profile_check.py). Three
// named threads run for about 3 s: "spin" burns CPU in spin_work, "waiter" blocks in wait_here on an
// event, and "faults" takes caught access violations in a loop (each one a debug event under the
// debugger, as the stand-ins' readability probe is in a title run). The main thread sleeps, then
// releases the waiter, joins all three and exits 0. No GPU and no other input is involved.
#include <windows.h>

#include <cstdio>

namespace {
volatile LONG stop;
volatile unsigned long long sink;
HANDLE release;

__declspec(noinline) unsigned long long spin_work()
{
    unsigned long long x = 1;
    while (!stop) {
        for (int i = 0; i < 100000; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
        sink = x;
    }
    return x;
}

__declspec(noinline) void wait_here() { WaitForSingleObject(release, INFINITE); }

__declspec(noinline) int fault_once()
{
    __try {
        return *reinterpret_cast<volatile int*>(sizeof(int));  // unmapped page: access violation
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

DWORD WINAPI spin_thread(void*)
{
    SetThreadDescription(GetCurrentThread(), L"spin");
    spin_work();
    return 0;
}

DWORD WINAPI wait_thread(void*)
{
    SetThreadDescription(GetCurrentThread(), L"waiter");
    wait_here();
    return 0;
}

DWORD WINAPI fault_thread(void*)
{
    SetThreadDescription(GetCurrentThread(), L"faults");
    while (!stop) {
        fault_once();
        Sleep(1);
    }
    return 0;
}
}  // namespace

int main()
{
    release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE threads[] = {CreateThread(nullptr, 0, spin_thread, nullptr, 0, nullptr),
                        CreateThread(nullptr, 0, wait_thread, nullptr, 0, nullptr),
                        CreateThread(nullptr, 0, fault_thread, nullptr, 0, nullptr)};
    Sleep(3000);
    InterlockedExchange(&stop, 1);
    SetEvent(release);
    WaitForMultipleObjects(3, threads, TRUE, INFINITE);
    std::printf("profile-probe done %llu\n", sink & 1);
    return 0;
}
