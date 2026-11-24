// The DirectStorage stand-in's status wait diagnostics (src/startup_stubs/dstorage_status_stats.h):
// completions and their wait, IsComplete polls, an index enqueued again before its status was written
// ("reused") and the older status's write landing afterwards ("stale"), and the listing of an index
// pending for over a second. `--mutate no-stale` skips the stale check in on_complete's caller
// contract (it passes generation 0 for every write), so the stale count stays 0: the negative control.
#include "startup_stubs/dstorage_status_stats.h"

#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> lines;

void capture(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    lines.emplace_back(line);
    std::printf("  | %s\n", line);
}

int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

bool logged(const char* text)
{
    for (const auto& l : lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

constexpr LONG pending = static_cast<LONG>(0x8000000A);

}  // namespace

int main(int argc, char** argv)
{
    const bool no_stale = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "no-stale") == 0;
    namespace st = x2w_dstorage_stats;
    auto queue_text = [](void* queue, char* out, std::size_t size) { std::snprintf(out, size, "queue %p", queue); };
    const auto report = [&](int second) {
        lines.clear();
        st::report(second, pending, [](const char* f, auto... a) { capture(f, a...); }, queue_text);
    };

    volatile LONG results[2] = {pending, pending};
    st::StatusStats* s = st::make(2, results);
    expect(s != nullptr, "make: stats for a capacity-2 array");
    if (!s) return 1;
    int queue = 0;

    // One normal batch: enqueue, polled pending three times, completed, polled complete once.
    LONG gen = st::on_enqueue(s, 0, &queue);
    results[0] = pending;
    for (int i = 0; i < 3; ++i) st::on_poll(s, false, nullptr);
    st::on_complete(s, 0, no_stale ? 0 : gen);
    results[0] = S_OK;
    st::on_poll(s, true, nullptr);
    report(10);
    expect(logged(no_stale ? "completions 0" : "completions 1,"), "a completed status is counted");
    expect(logged("polls pending 3 complete 1"), "IsComplete polls are counted by outcome");
    expect(logged("reused before complete 0, stale writes 0"), "no reuse in a normal batch");

    // Index 0 enqueued again (gen a), then again before a's status was written (gen b): reused. a's write
    // then lands on the index b set pending: stale (the title would see b complete early).
    const LONG a = st::on_enqueue(s, 0, &queue);
    results[0] = pending;
    const LONG b = st::on_enqueue(s, 0, &queue);
    results[0] = pending;
    st::on_complete(s, 0, no_stale ? 0 : a);
    results[0] = S_OK;
    report(20);
    expect(logged("reused before complete 1"), "an index enqueued again before its status was written counts as reused");
    expect(logged("stale writes 1"), "the older status's write after that counts as stale");
    st::on_complete(s, 0, no_stale ? 0 : b);

    // Index 1 pending for over a second is listed with its queue; index 0 (complete) is not.
    st::on_enqueue(s, 1, &queue);
    results[1] = pending;
    Sleep(1100);
    report(30);
    expect(logged("status PENDING array #0 index 1 for 1."), "an index pending for over a second is listed");
    expect(!logged("index 0 for"), "a complete index is not listed");

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
