// Source-built CRT-free provider with a NONAME export. No Xbox/game data.
#include <windows.h>
#include <cstdint>

// Debug /RTC inserts CRT initialization calls that a /NODEFAULTLIB fixture
// cannot serve. Keep this synthetic fixture CRT-free in Debug and Release.
#pragma runtime_checks("", off)

extern "C" BOOL WINAPI ordinal_provider_entry(HINSTANCE, DWORD, void*) { return TRUE; }

#ifdef LIVE_WORKER
// Control provider (#1375): mix_args leaves a thread running that outlives the caller's entry point, as an
// idle loader or thread-pool worker can. Only a process exit ends it.
static DWORD WINAPI linger_forever(void*) {
    Sleep(INFINITE);
    return 0;
}
#endif

extern "C" std::uint64_t mix_args(std::uint64_t a, std::uint64_t b, std::uint64_t* words) {
#ifdef LIVE_WORKER
    if (HANDLE thread = CreateThread(nullptr, 0, linger_forever, nullptr, 0, nullptr)) CloseHandle(thread);
    else return 0;  // the caller then reports 0xBAD00001
#endif
    words[0] = a;
    words[1] = b;
    words[2] = a - b + 11;
#ifdef ALTER_RETURN
    return words[2] + 1;
#else
    return words[2];
#endif
}
