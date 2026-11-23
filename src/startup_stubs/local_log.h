// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed): opt-in switches and logging shared by the
// permissive startup paths. Lines go to stderr and are appended to %X2W_TRAP_LOG% when set.
#pragma once

#include "startup_stubs/trap.h"

#include <cstdarg>
#include <cstdio>

namespace x2w_local {

inline bool env_on(const char* name) noexcept
{
    char value[4]{};
    return GetEnvironmentVariableA(name, value, sizeof(value)) && value[0] == '1';
}

inline void logf(const char* format, ...) noexcept
{
    char line[1536];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 3) n = sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    const DWORD error = GetLastError();
    x2w_trap::write_all(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n));
    wchar_t log_path[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path, MAX_PATH);
    if (length && length < MAX_PATH) {
        HANDLE out = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            x2w_trap::write_all(out, line, static_cast<DWORD>(n));
            CloseHandle(out);
        }
    }
    SetLastError(error);
}

inline bool first_calls(LONG& counter, LONG limit) noexcept { return InterlockedIncrement(&counter) <= limit; }

// Readability check without IsBadReadPtr (which probes by faulting: a first-chance exception per
// call, very slow under a debugger). One VirtualQuery per check; nothing is cached, since regions can be freed.
inline bool accessible(const void* pointer, std::size_t bytes, bool write) noexcept
{
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start < 0x10000 || start > 0x00007FFFFFFEFFFFull || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    const DWORD protect = info.Protect & 0xFF;
    if ((info.Protect & PAGE_GUARD) || protect == PAGE_NOACCESS || protect == 0) return false;
    const bool writable = protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_WRITECOPY;
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = begin + info.RegionSize;
    if (start + bytes > end || (write && !writable)) return false;
    return true;
}
inline bool readable(const void* pointer, std::size_t bytes) noexcept { return accessible(pointer, bytes, false); }
inline bool writable(const void* pointer, std::size_t bytes) noexcept { return accessible(pointer, bytes, true); }

}  // namespace x2w_local
