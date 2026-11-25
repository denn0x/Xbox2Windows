// LOCAL stall: draw_lock hold and wait times by site (d3d12_draw.inl, X2W_DRAW_LOCK_TIMES).
//
//   startup_draw_lock_times <on|off>
//   (on: X2W_DRAW_LOCK_TIMES=1; the test sets X2W_TRAP_LOG itself)
//
// Three threads take draw_lock through the stand-in's own lock_draw (export x2w_draw_lock_times_test op 0), 50 times
// each: site 999001 holds 2 ms, site 999002 holds 2 ms, site 999003 holds 0 ms. Then op 1 writes the report.
// on: one "x2w-draw lock times (fixture" line; 999001 and 999002 each 50 holds, total 100-1000 ms, max >= 2 ms;
//     999003 50 holds; the waits over all sites > 0 with a total above 50 ms (the 2 ms holders contend).
// off (control): op 1 returns 0 and no such line is written. No GPU is used (the export never touches the device).
// Exit 0 pass, other values name the step.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using Test = std::uint64_t(WINAPI*)(int, std::uint64_t, std::uint64_t);

namespace {
struct SiteRow { long holds = -1; double hold_total = 0, hold_max = 0; long waits = 0; double wait_total = 0, wait_max = 0; };
SiteRow find_site(const std::string& line, long id)
{
    SiteRow row;
    const std::string key = " " + std::to_string(id) + " fixture ";
    const std::size_t p = line.find(key);
    if (p == std::string::npos) return row;
    std::sscanf(line.c_str() + p + key.size(), "%ld/%lf/%lf, %ld/%lf/%lf", &row.holds, &row.hold_total, &row.hold_max, &row.waits,
                &row.wait_total, &row.wait_max);
    return row;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode != "on" && mode != "off") return 2;
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 3;
    const std::wstring log_path = std::wstring(temp) + L"x2w_draw_lock_times_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                  std::to_wstring(GetTickCount64()) + L".log";
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path.c_str());
    HMODULE module = LoadLibraryW(L"d3d12_xs.dll");
    if (!module) return 4;
    const auto test = reinterpret_cast<Test>(GetProcAddress(module, "x2w_draw_lock_times_test"));
    if (!test) return 5;
    std::vector<std::thread> threads;
    for (int t = 1; t <= 3; ++t)
        threads.emplace_back([test, t] {
            for (int i = 0; i < 50; ++i) test(0, t == 3 ? 0 : 2000, static_cast<std::uint64_t>(t));
        });
    for (auto& thread : threads) thread.join();
    const std::uint64_t reported = test(1, 0, 0);

    std::ifstream file(log_path, std::ios::binary);
    const std::string log{std::istreambuf_iterator<char>(file), {}};
    const std::size_t at = log.find("x2w-draw lock times (fixture");
    const std::string line = at == std::string::npos ? std::string() : log.substr(at, log.find('\n', at) - at);
    int failures = 0;
    if (mode == "off") {
        std::printf("op 1 -> %llu (want 0); report line %s (want none)\n", static_cast<unsigned long long>(reported), line.empty() ? "none" : "present");
        if (reported != 0 || !line.empty()) ++failures;
    } else {
        std::printf("%s\n", line.empty() ? "no report line" : line.c_str());
        if (reported != 1 || line.empty()) ++failures;
        double waited = 0;
        for (long id : {999001L, 999002L, 999003L}) {
            const SiteRow r = find_site(line, id);
            std::printf("site %ld: holds %ld total %.1f ms max %.2f ms; waits %ld total %.1f ms max %.2f ms\n", id, r.holds, r.hold_total, r.hold_max,
                        r.waits, r.wait_total, r.wait_max);
            waited += r.wait_total;
            if (r.holds != 50) ++failures;
            if (id != 999003 && (r.hold_total < 100.0 || r.hold_total > 1000.0 || r.hold_max < 2.0)) ++failures;
        }
        std::printf("waited over the three sites %.1f ms (want > 50)\n", waited);
        if (waited <= 50.0) ++failures;
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 16 : 0;
}
