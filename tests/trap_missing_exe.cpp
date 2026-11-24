// Source-built EXE for `pe_run_package.py --trap-callable`. It imports two functions
// from trap_fixture.dll, a module that no build product provides, plus one kernel32
// function to read its mode:
//   TRAP_FIXTURE_MODE unset or "alpha": call trap_fixture_alpha with six arguments
//   TRAP_FIXTURE_MODE "beta": call trap_fixture_beta with none
//   TRAP_FIXTURE_MODE "none": call neither and return 0x7A0
// A trap must never return, so a return from either call exits with 0xBAD instead.
// Built with TRAP_DATA_IMPORT (trap_missing_data_exe), it also imports trap_fixture_data
// as data and reads it in mode "data", so its import table holds a name nothing calls.
#include <windows.h>

extern "C" __declspec(dllimport) UINT64 WINAPI trap_fixture_alpha(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64);
extern "C" __declspec(dllimport) UINT64 WINAPI trap_fixture_beta();
#ifdef TRAP_DATA_IMPORT
extern "C" __declspec(dllimport) UINT64 trap_fixture_data;
#endif

// The Debug preset enables /RTC1. This CRT-free fixture still references the two RTC
// bookkeeping symbols, so provide inert definitions instead of linking the runtime.
extern "C" void _RTC_InitBase() {}
extern "C" void _RTC_Shutdown() {}

static bool equals(const char* value, const char* expected)
{
    while (*value && *value == *expected) {
        ++value;
        ++expected;
    }
    return *value == *expected;
}

extern "C" DWORD WINAPI fixture_entry()
{
    // Static, not a stack array: /RTC1 would otherwise need _RTC_CheckStackVars from the CRT.
    static char mode[16];
    const DWORD length = GetEnvironmentVariableA("TRAP_FIXTURE_MODE", mode, sizeof(mode));
    if (length >= sizeof(mode))
        return 0xE01;
    if (equals(mode, "none"))
        return 0x7A0;
#ifdef TRAP_DATA_IMPORT
    if (equals(mode, "data"))
        return static_cast<DWORD>(trap_fixture_data);
#endif
    if (equals(mode, "beta"))
        trap_fixture_beta();
    else if (length == 0 || equals(mode, "alpha"))
        trap_fixture_alpha(1, 2, 3, 4, 5, 6);
    else
        return 0xE02;
    return 0xBAD;
}
