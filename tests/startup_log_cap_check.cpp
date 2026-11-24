// x2w_local::first_calls (src/startup_stubs/log_cap.h): a log cap that never wraps. Checks the first
// `limit` calls pass and no later one does, from several threads at once (exact count), that the counter
// stops at limit + 1, and that a counter near LONG_MAX stays closed. `--mutate wrap` runs the same
// checks against the old `InterlockedIncrement(&c) <= limit` pattern, which wraps: the negative control.
#include "startup_stubs/log_cap.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

bool use_old = false;

bool cap(volatile LONG& counter, LONG limit)
{
    return use_old ? InterlockedIncrement(&counter) <= limit : x2w_local::first_calls(counter, limit);
}

int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv)
{
    use_old = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "wrap") == 0;

    {
        volatile LONG counter = 0;
        int passed = 0;
        for (int i = 0; i < 100; ++i) passed += cap(counter, 4);
        expect(passed == 4, "single thread: 4 of 100 calls pass a cap of 4");
        expect(use_old || counter == 5, "single thread: the counter stops at limit + 1");
    }
    {
        constexpr int threads = 8, calls = 200000;
        constexpr LONG limit = 1000;
        volatile LONG counter = 0;
        std::vector<long> passed(threads, 0);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] { for (int i = 0; i < calls; ++i) passed[t] += cap(counter, limit); });
        for (auto& th : pool) th.join();
        long total = 0;
        for (long p : passed) total += p;
        expect(total == limit, "8 threads: exactly 1000 of 1.6 million calls pass a cap of 1000");
        expect(use_old || counter == limit + 1, "8 threads: the counter stops at limit + 1");
    }
    {
        // A counter that has already counted to LONG_MAX (2^31 - 1 calls): the next calls must stay closed.
        volatile LONG counter = LONG_MAX;
        int passed = 0;
        for (int i = 0; i < 1000; ++i) passed += cap(counter, 4);
        expect(passed == 0, "a counter at LONG_MAX stays closed (the old pattern wraps and passes here)");
    }
    {
        expect(x2w_local::first_of(1, 4) && x2w_local::first_of(4, 4), "first_of: counts 1..limit pass");
        expect(!x2w_local::first_of(5, 4) && !x2w_local::first_of(LONG_MIN, 4) && !x2w_local::first_of(-1, 4) && !x2w_local::first_of(0, 4),
               "first_of: past the limit and wrapped counts do not");
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
