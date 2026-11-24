// Checks src/startup_stubs/local_log.h without a GPU device: GetLastError-preserving env_on
// gating, atomic first_calls countdown, logf formatting/truncation/X2W_TRAP_LOG append,
// probe_readable page truncation and PAGE_GUARD re-arming, and readable/writable boundary checks.
#include "startup_stubs/local_log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int cases = 0;

void check(bool cond, const char* name)
{
    ++cases;
    if (!cond) {
        std::printf("FAIL %s (GetLastError=%lu)\n", name, static_cast<unsigned long>(GetLastError()));
        std::fflush(stdout);
        ++failures;
    }
}

std::string read_file_bytes(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, buf + got);
    }
    CloseHandle(file);
    return out;
}

void test_env_on()
{
    const char* const var_name = "X2W_TEST_LOCAL_LOG_SWITCH";

    SetEnvironmentVariableA(var_name, nullptr);
    SetLastError(0x12345678u);
    const bool unset_res = x2w_local::env_on(var_name);
    const DWORD unset_err = GetLastError();
    check(!unset_res && unset_err == 0x12345678u, "env_on unset returns false and preserves GetLastError");

    for (const char* val : {"", "0", "2", "01", " 1", "true", "on", "yes"}) {
        SetEnvironmentVariableA(var_name, val);
        SetLastError(0x0BAD0001u);
        const bool res = x2w_local::env_on(var_name);
        const DWORD err = GetLastError();
        check(!res && err == 0x0BAD0001u, "env_on value without a leading 1 returns false and preserves GetLastError");
    }

    for (const char* val : {"1", "10", "1x", "100"}) {
        SetEnvironmentVariableA(var_name, val);
        SetLastError(0x0CAFE002u);
        const bool on_res = x2w_local::env_on(var_name);
        const DWORD on_err = GetLastError();
        check(on_res && on_err == 0x0CAFE002u, "env_on leading 1 returns true and preserves GetLastError");
    }

    SetEnvironmentVariableA(var_name, nullptr);
}

void test_first_calls()
{
    LONG counter = 0;
    check(x2w_local::first_calls(counter, 3) && counter == 1, "first_calls 1/3");
    check(x2w_local::first_calls(counter, 3) && counter == 2, "first_calls 2/3");
    check(x2w_local::first_calls(counter, 3) && counter == 3, "first_calls 3/3");
    check(!x2w_local::first_calls(counter, 3) && counter == 4, "first_calls 4/3 false");
    check(!x2w_local::first_calls(counter, 3) && counter == 5, "first_calls 5/3 false");

    LONG zero_limit = 0;
    check(!x2w_local::first_calls(zero_limit, 0) && zero_limit == 1, "first_calls limit=0 false");

    LONG neg_limit = 0;
    check(!x2w_local::first_calls(neg_limit, -1) && neg_limit == 1, "first_calls limit=-1 false");

    LONG shared = 0;
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                if (x2w_local::first_calls(shared, 10)) accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads) th.join();
    check(shared == 400 && accepted.load(std::memory_order_relaxed) == 10,
          "first_calls concurrent InterlockedIncrement exactness");
}

void test_logf()
{
    wchar_t prev_trap_log[MAX_PATH]{};
    const DWORD prev_len = GetEnvironmentVariableW(L"X2W_TRAP_LOG", prev_trap_log, MAX_PATH);

    wchar_t temp_dir[MAX_PATH]{};
    const DWORD dir_len = GetTempPathW(MAX_PATH, temp_dir);
    check(dir_len > 0 && dir_len < MAX_PATH, "GetTempPathW succeeds");
    if (!dir_len || dir_len >= MAX_PATH) return;
    const auto folder = std::wstring(temp_dir) + L"x2w_local_log_" + std::to_wstring(GetCurrentProcessId());
    const auto log_path = folder + L"\\trap.log";
    if (log_path.size() >= MAX_PATH) {
        check(false, "temporary folder has space for the log names");
        return;
    }
    if (!CreateDirectoryW(folder.c_str(), nullptr)) {
        check(false, "create temporary log folder");
        return;
    }
    const auto err_path = folder + L"\\stderr";
    const auto max_name = MAX_PATH - 2 - folder.size();
    const auto fit_path = folder + L"\\" + std::wstring(max_name, L'f');
    const auto long_path = folder + L"\\" + std::wstring(max_name + 1, L'g');
    const HANDLE saved_stderr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE stderr_file = CreateFileW(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool captured = stderr_file != INVALID_HANDLE_VALUE &&
                          SetStdHandle(STD_ERROR_HANDLE, stderr_file) != FALSE;
    check(captured, "capture the stderr stream");

    SetEnvironmentVariableW(L"X2W_TRAP_LOG", nullptr);
    SetLastError(0x13572468u);
    x2w_local::logf("unset-path-%d", 1);
    check(GetLastError() == 0x13572468u, "logf preserves GetLastError when X2W_TRAP_LOG is unset");

    SetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path.c_str());
    SetLastError(0x0BADF00Du);
    x2w_local::logf("first-%d", 42);
    check(GetLastError() == 0x0BADF00Du, "logf preserves GetLastError when X2W_TRAP_LOG is set");

    x2w_local::logf("second-%s", "line");

    const std::string two_lines = read_file_bytes(log_path.c_str());
    check(two_lines == "first-42\r\nsecond-line\r\n", "logf appends CRLF lines to X2W_TRAP_LOG");

    std::string expected_log = "first-42\r\nsecond-line\r\n";
    for (std::size_t length : {1532, 1533, 1534, 2000}) {
        const std::string message(length, 'A');
        x2w_local::logf("%s", message.c_str());
        expected_log += message.substr(0, 1533) + "\r\n";
    }
    check(read_file_bytes(log_path.c_str()) == expected_log, "logf clamps only beyond 1533 payload bytes");

    SetEnvironmentVariableW(L"X2W_TRAP_LOG", fit_path.c_str());
    x2w_local::logf("path at limit");
    check(read_file_bytes(fit_path.c_str()) == "path at limit\r\n", "logf accepts a path of MAX_PATH minus one");
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", long_path.c_str());
    x2w_local::logf("path over limit");
    check(read_file_bytes(log_path.c_str()) == expected_log &&
              GetFileAttributesW(long_path.substr(0, MAX_PATH - 1).c_str()) == INVALID_FILE_ATTRIBUTES,
          "logf leaves prior logs alone and declines an overlong path");
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", folder.c_str());
    SetLastError(0x13572468u);
    x2w_local::logf("path is a directory");
    check(GetLastError() == 0x13572468u, "logf preserves the last error on open failure");

    if (prev_len > 0 && prev_len < MAX_PATH) {
        SetEnvironmentVariableW(L"X2W_TRAP_LOG", prev_trap_log);
    } else {
        SetEnvironmentVariableW(L"X2W_TRAP_LOG", nullptr);
    }
    if (captured) {
        SetStdHandle(STD_ERROR_HANDLE, saved_stderr);
    }
    if (stderr_file != INVALID_HANDLE_VALUE) CloseHandle(stderr_file);
    check(read_file_bytes(err_path.c_str()) == "unset-path-1\r\n" + expected_log +
              "path at limit\r\npath over limit\r\npath is a directory\r\n",
          "stderr still receives every formatted line");
    for (const auto* path : {&err_path, &log_path, &fit_path}) DeleteFileW(path->c_str());
    check(RemoveDirectoryW(folder.c_str()) != FALSE, "clean up only the expected files");
}

void test_probe_and_access(bool guard_drop_mutate)
{
    constexpr std::size_t kPage = 0x1000;
    constexpr std::size_t kPages = 4;

    void* const raw = VirtualAlloc(nullptr, kPages * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(raw != nullptr, "VirtualAlloc 4 pages succeeds");
    if (!raw) return;

    const auto base = reinterpret_cast<std::uintptr_t>(raw);
    std::memset(raw, 0x5A, kPages * kPage);

    // Invalid addresses and zero-byte lengths.
    check(x2w_local::probe_readable(0, 16) == 0, "probe_readable rejects null");
    check(x2w_local::probe_readable(0xFFFFu, 16) == 0, "probe_readable rejects < 0x10000");
    check(x2w_local::probe_readable(0x00007FFFFFFF0000ull, 16) == 0, "probe_readable rejects > user limit");
    check(x2w_local::probe_readable(base, 0) == 0, "probe_readable rejects 0 bytes");

    check(!x2w_local::readable(nullptr, 1) && !x2w_local::writable(nullptr, 1), "accessible rejects null");
    const auto low_ptr = reinterpret_cast<const void*>(std::uintptr_t{0xFFFF});
    const auto high_ptr = reinterpret_cast<const void*>(std::uintptr_t{0x00007FFFFFFF0000ull});
    check(!x2w_local::readable(low_ptr, 1) && !x2w_local::writable(low_ptr, 1), "accessible rejects < 0x10000");
    check(!x2w_local::readable(high_ptr, 1) && !x2w_local::writable(high_ptr, 1), "accessible rejects > user limit");
    check(!x2w_local::readable(raw, 0) && !x2w_local::writable(raw, 0), "accessible rejects 0 bytes");

    // Multi-page readable range and unaligned slice.
    check(x2w_local::probe_readable(base, kPages * kPage) == kPages * kPage, "probe_readable full 4 pages");
    check(x2w_local::probe_readable(base + 0xF00u, 0x2200u) == 0x2200u, "probe_readable unaligned across pages");
    check(x2w_local::readable(raw, kPages * kPage) && x2w_local::writable(raw, kPages * kPage),
          "4-page RW region is readable and writable");

    // A size so large that start + bytes wraps around the address space is not a range.
    check(!x2w_local::readable(raw, SIZE_MAX - 8) && !x2w_local::writable(raw, SIZE_MAX - 8),
          "accessible rejects a size that wraps start + bytes");
    check(!x2w_local::writable(raw, SIZE_MAX), "writable rejects SIZE_MAX bytes");
    check(!x2w_local::writable(reinterpret_cast<const void*>(base + kPage - 1), SIZE_MAX - kPage),
          "writable rejects a wrapping size from inside the region");

    // PAGE_READONLY on page 1: readable across page 0 -> page 1, writable stops at page 0 end.
    DWORD old_prot = 0;
    check(VirtualProtect(reinterpret_cast<void*>(base + kPage), kPage, PAGE_READONLY, &old_prot) != 0,
          "VirtualProtect page 1 PAGE_READONLY");
    check(x2w_local::readable(reinterpret_cast<const void*>(base + kPage), kPage), "PAGE_READONLY is readable");
    check(!x2w_local::writable(reinterpret_cast<const void*>(base + kPage), 1), "PAGE_READONLY is not writable");
    check(x2w_local::readable(reinterpret_cast<const void*>(base + 0xFF0u), 32),
          "readable crosses RW->RO page boundary");
    check(x2w_local::writable(reinterpret_cast<const void*>(base + 0xFF0u), 16),
          "writable accepts up to RW region end");
    check(!x2w_local::writable(reinterpret_cast<const void*>(base + 0xFF0u), 17),
          "writable rejects crossing RW->RO region boundary");

    // PAGE_WRITECOPY via a section copy-on-write mapping: both readable and writable.
    HANDLE cow_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kPage), nullptr);
    check(cow_map != nullptr, "CreateFileMappingW for PAGE_WRITECOPY succeeds");
    if (cow_map) {
        void* const cow_view = MapViewOfFile(cow_map, FILE_MAP_COPY, 0, 0, kPage);
        check(cow_view != nullptr, "MapViewOfFile FILE_MAP_COPY succeeds");
        if (cow_view) {
            MEMORY_BASIC_INFORMATION cow_mbi{};
            check(VirtualQuery(cow_view, &cow_mbi, sizeof(cow_mbi)) != 0 &&
                  (cow_mbi.Protect & 0xFF) == PAGE_WRITECOPY,
                  "MapViewOfFile FILE_MAP_COPY has PAGE_WRITECOPY protection");
            check(x2w_local::readable(cow_view, kPage), "PAGE_WRITECOPY is readable");
            check(x2w_local::writable(cow_view, kPage), "PAGE_WRITECOPY is writable");
            UnmapViewOfFile(cow_view);
        }
        CloseHandle(cow_map);
    }

    // PAGE_NOACCESS on page 2: truncates probe_readable prefix cleanly.
    check(VirtualProtect(reinterpret_cast<void*>(base + 2 * kPage), kPage, PAGE_NOACCESS, &old_prot) != 0,
          "VirtualProtect page 2 PAGE_NOACCESS");
    check(x2w_local::probe_readable(base, kPages * kPage) == 2 * kPage,
          "probe_readable truncates before PAGE_NOACCESS");
    check(x2w_local::probe_readable(base + 0x123u, kPages * kPage - 0x123u) == 2 * kPage - 0x123u,
          "probe_readable unaligned truncates before PAGE_NOACCESS");
    check(x2w_local::probe_readable(base + 2 * kPage, kPage) == 0,
          "probe_readable on PAGE_NOACCESS returns 0");
    check(!x2w_local::readable(reinterpret_cast<const void*>(base + 2 * kPage), 1) &&
          !x2w_local::writable(reinterpret_cast<const void*>(base + 2 * kPage), 1),
          "PAGE_NOACCESS is neither readable nor writable");
    check(!x2w_local::readable(reinterpret_cast<const void*>(base + 2 * kPage - 16), 17),
          "readable rejects crossing into PAGE_NOACCESS");

    // PAGE_GUARD on page 1: probe stops before guard page and re-arms PAGE_GUARD.
    check(VirtualProtect(reinterpret_cast<void*>(base + kPage), kPage, PAGE_READWRITE | PAGE_GUARD, &old_prot) != 0,
          "VirtualProtect page 1 PAGE_GUARD");
    check(x2w_local::probe_readable(base, 2 * kPage) == kPage,
          "probe_readable stops at PAGE_GUARD page");
    MEMORY_BASIC_INFORMATION mbi{};
    check(VirtualQuery(reinterpret_cast<const void*>(base + kPage), &mbi, sizeof(mbi)) != 0,
          "VirtualQuery after first PAGE_GUARD probe");
    if (guard_drop_mutate) {
        check((mbi.Protect & PAGE_GUARD) == 0, "guard-drop control expects PAGE_GUARD cleared");
    } else {
        check((mbi.Protect & PAGE_GUARD) != 0, "probe_readable re-arms PAGE_GUARD after trip");
        // Second probe must hit the re-armed guard page at the same offset and keep it armed.
        check(x2w_local::probe_readable(base, 2 * kPage) == kPage,
              "second probe_readable still stops at re-armed PAGE_GUARD");
        MEMORY_BASIC_INFORMATION mbi2{};
        check(VirtualQuery(reinterpret_cast<const void*>(base + kPage), &mbi2, sizeof(mbi2)) != 0 &&
              (mbi2.Protect & PAGE_GUARD) != 0,
              "second probe_readable keeps PAGE_GUARD armed");
        check(!x2w_local::readable(reinterpret_cast<const void*>(base + kPage + 0x10u), 16),
              "readable returns false on PAGE_GUARD");
        check(!x2w_local::writable(reinterpret_cast<const void*>(base + kPage), 16),
              "writable returns false on PAGE_GUARD");
        MEMORY_BASIC_INFORMATION mbi3{};
        check(VirtualQuery(reinterpret_cast<const void*>(base + kPage), &mbi3, sizeof(mbi3)) != 0 &&
              (mbi3.Protect & PAGE_GUARD) != 0,
              "readable/writable keeps PAGE_GUARD armed");
    }

    VirtualFree(raw, 0, MEM_RELEASE);

    // MEM_RESERVE (uncommitted) tail boundary.
    void* const reserved = VirtualAlloc(nullptr, 2 * kPage, MEM_RESERVE, PAGE_READWRITE);
    check(reserved != nullptr, "VirtualAlloc MEM_RESERVE 2 pages succeeds");
    if (reserved) {
        void* const first_committed = VirtualAlloc(reserved, kPage, MEM_COMMIT, PAGE_READWRITE);
        check(first_committed == reserved, "commit first page of reserved range");
        const auto rbase = reinterpret_cast<std::uintptr_t>(reserved);
        check(x2w_local::probe_readable(rbase, 2 * kPage) == kPage,
              "probe_readable truncates before MEM_RESERVE");
        check(x2w_local::readable(reserved, kPage) && x2w_local::writable(reserved, kPage),
              "committed head of reserved range is readable and writable");
        check(!x2w_local::readable(reinterpret_cast<const void*>(rbase + kPage), 1) &&
              !x2w_local::writable(reinterpret_cast<const void*>(rbase + kPage), 1),
              "MEM_RESERVE page is neither readable nor writable");
        VirtualFree(reserved, 0, MEM_RELEASE);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    // --mutate guard-drop: negative control expecting PAGE_GUARD to be dropped rather than re-armed.
    const bool guard_drop_mutate =
        argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "guard-drop") == 0;
    if (argc > 1 && !guard_drop_mutate) {
        std::printf("unknown mutation\n");
        return 2;
    }

    test_env_on();
    test_first_calls();
    test_logf();
    test_probe_and_access(guard_drop_mutate);

    if (failures) {
        std::printf("startup_local_log: %d of %d FAILED\n", failures, cases);
        return 1;
    }
    std::printf("startup_local_log: PASS (%d cases)\n", cases);
    return 0;
}
