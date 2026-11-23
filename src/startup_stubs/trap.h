// Trap-by-default exports for startup imports whose ABI has no public evidence yet.
//
// Each export takes the four x64 integer argument registers (always safe to declare,
// whatever the real signature is), records the call and then terminates the process.
// Nothing returns to the caller, so no caller reads an out-parameter or status that this
// desktop library never wrote.
//
// Log line (stderr, and appended to %X2W_TRAP_LOG% when that variable is set):
//   x2w-trap <module>!<export> rcx=... rdx=... r8=... r9=... ret=<module>+0x<offset>
// Exit code: 0xE2570000 | (module id << 8) | export index.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x2w_trap {

constexpr DWORD exit_code(unsigned module_id, unsigned index) noexcept
{
    return 0xE2570000u | ((module_id & 0xFFu) << 8) | (index & 0xFFu);
}

inline void write_all(HANDLE handle, const char* text, DWORD length) noexcept
{
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(handle, text, length, &written, nullptr);
}

// Local experiment switch: %X2W_TRAP_CONTINUE% lists export names (comma-separated, or *) whose
// trap logs the call with "x2w-continue" and returns 0 instead of terminating. Off when unset.
inline bool listed(const char* variable, const char* name) noexcept
{
    char list[2048];
    const DWORD n = GetEnvironmentVariableA(variable, list, sizeof(list));
    if (!n || n >= sizeof(list)) return false;
    if (list[0] == '*' && list[1] == 0) return true;
    const std::size_t want = std::strlen(name);
    for (const char* item = list; *item;) {
        const char* end = item;
        while (*end && *end != ',') ++end;
        if (static_cast<std::size_t>(end - item) == want && !std::strncmp(item, name, want)) return true;
        item = *end ? end + 1 : end;
    }
    return false;
}

inline bool continues(const char* name) noexcept { return listed("X2W_TRAP_CONTINUE", name); }
// %X2W_TRAP_FAIL% lists export names whose trap logs "x2w-fail" and returns E_FAIL (0x80004005).
inline bool fails(const char* name) noexcept { return listed("X2W_TRAP_FAIL", name); }

__declspec(noinline) inline void record(const char* tag, const char* module, unsigned module_id,
    const char* name, unsigned index, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8,
    std::uint64_t r9, void* return_address) noexcept
{
    char caller[MAX_PATH] = "?";
    std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(return_address);
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(return_address), &owner) && owner) {
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(owner, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            const char* base = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') base = p + 1;
            std::snprintf(caller, sizeof(caller), "%s", base);
        }
        offset -= reinterpret_cast<std::uintptr_t>(owner);
    }

    char line[1024];
    const int length = std::snprintf(line, sizeof(line),
        "%s %s!%s rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx ret=%s+0x%llx exit=0x%08lx\r\n",
        tag, module, name, static_cast<unsigned long long>(rcx), static_cast<unsigned long long>(rdx),
        static_cast<unsigned long long>(r8), static_cast<unsigned long long>(r9), caller,
        static_cast<unsigned long long>(offset), static_cast<unsigned long>(exit_code(module_id, index)));
    const DWORD size = length > 0 ? static_cast<DWORD>(length < static_cast<int>(sizeof(line)) ? length : sizeof(line) - 1) : 0;

    write_all(GetStdHandle(STD_ERROR_HANDLE), line, size);

    wchar_t log_path[MAX_PATH];
    const DWORD path_length = GetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path, MAX_PATH);
    if (path_length && path_length < MAX_PATH) {
        HANDLE log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            write_all(log, line, size);
            FlushFileBuffers(log);
            CloseHandle(log);
        }
    }
}

[[noreturn]] __declspec(noinline) inline void fire(const char* module, unsigned module_id,
    const char* name, unsigned index, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8,
    std::uint64_t r9, void* return_address) noexcept
{
    record("x2w-trap", module, module_id, name, index, rcx, rdx, r8, r9, return_address);
    TerminateProcess(GetCurrentProcess(), exit_code(module_id, index));
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

}  // namespace x2w_trap

// X2W_TRAP_MODULE (string) and X2W_TRAP_MODULE_ID (integer) must be defined before use.
#define X2W_TRAP_EXPORT(index, name)                                                                   \
    extern "C" __declspec(noinline) std::uint64_t name(std::uint64_t rcx, std::uint64_t rdx,          \
        std::uint64_t r8, std::uint64_t r9)                                                            \
    {                                                                                                  \
        if (x2w_trap::continues(#name)) {                                                          \
            x2w_trap::record("x2w-continue", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, #name, index,    \
                rcx, rdx, r8, r9, _ReturnAddress());                                               \
            return 0;                                                                              \
        }                                                                                          \
        if (x2w_trap::fails(#name)) {                                                              \
            x2w_trap::record("x2w-fail", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, #name, index,        \
                rcx, rdx, r8, r9, _ReturnAddress());                                               \
            return 0x80004005u;                                                                    \
        }                                                                                          \
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, #name, index, rcx, rdx, r8, r9,            \
            _ReturnAddress());                                                                         \
    }
