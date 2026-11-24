// Experimental (docs/startup-runtime.md): how long DirectStorage status entries stay pending, for the
// XCRDAPI stand-in's heartbeat. LOCAL diagnostics; X2W_DSTORAGE_STATUS_STATS=0 turns them off.
//
// Per status array: the QPC time each index's EnqueueStatus happened, completions and their wait
// (EnqueueStatus to the result write) per heartbeat interval, the title's IsComplete polls that found
// the entry pending or complete, the first poller's return address, and two slot-reuse counts: an index
// enqueued again before its previous status was written ("reused"), and a result write for an older
// status landing on an index that a newer EnqueueStatus had set pending again ("stale": IsComplete then
// reports the newer status complete early). The heartbeat lists every index pending for over a second
// with its queue's entry counts, so a stuck status shows as such and a busy wait does not.
#pragma once

#include <windows.h>

#include <cstdio>
#include <new>

namespace x2w_dstorage_stats {

struct StatusStats {
    unsigned capacity = 0;
    unsigned serial = 0;
    const volatile LONG* results = nullptr;  // the array's results (owned by the stand-in)
    LONG64* enqueued_at = nullptr;  // QPC ticks of the latest EnqueueStatus per index
    LONG* enqueued_gen = nullptr;   // EnqueueStatus calls per index
    LONG* completed_gen = nullptr;  // generation of the latest result written per index
    void* volatile queue = nullptr; // the queue of the latest EnqueueStatus (opaque here)
    void* volatile poller = nullptr;
    volatile LONG64 polls_pending = 0, polls_complete = 0;
    volatile LONG64 completions = 0, wait_ticks = 0, wait_max = 0;  // reset by each report
    volatile LONG64 completions_total = 0;
    volatile LONG reused = 0, stale = 0;
};

inline bool enabled() noexcept
{
    static const bool on = [] {
        const DWORD saved = GetLastError();
        char value[4]{};
        const bool off = GetEnvironmentVariableA("X2W_DSTORAGE_STATUS_STATS", value, sizeof(value)) && value[0] == '0';
        SetLastError(saved);
        return !off;
    }();
    return on;
}

inline LONG64 now() noexcept
{
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline double ms(LONG64 ticks) noexcept
{
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? static_cast<double>(f.QuadPart) : 1.0;
    }();
    return static_cast<double>(ticks) * 1000.0 / frequency;
}

constexpr LONG max_arrays = 64;
inline StatusStats* arrays[max_arrays];
inline volatile LONG array_count = 0;

// Null when off or out of memory; every other call accepts null.
inline StatusStats* make(unsigned capacity, const volatile LONG* results) noexcept
{
    if (!enabled() || !capacity || !results) return nullptr;
    auto* s = new (std::nothrow) StatusStats;
    if (!s) return nullptr;
    s->capacity = capacity;
    s->results = results;
    s->enqueued_at = new (std::nothrow) LONG64[capacity]();
    s->enqueued_gen = new (std::nothrow) LONG[capacity]();
    s->completed_gen = new (std::nothrow) LONG[capacity]();
    if (!s->enqueued_at || !s->enqueued_gen || !s->completed_gen) {
        delete[] s->enqueued_at;
        delete[] s->enqueued_gen;
        delete[] s->completed_gen;
        delete s;
        return nullptr;
    }
    const LONG slot = InterlockedIncrement(&array_count) - 1;
    s->serial = static_cast<unsigned>(slot);
    if (slot < max_arrays) arrays[slot] = s;  // later arrays are tracked but not reported
    return s;
}

// At EnqueueStatus, before the index is set pending. Returns the entry's generation (0 when off).
inline LONG on_enqueue(StatusStats* s, unsigned index, void* queue) noexcept
{
    if (!s || index >= s->capacity) return 0;
    const LONG gen = InterlockedIncrement(&s->enqueued_gen[index]);
    if (s->completed_gen[index] != gen - 1) InterlockedIncrement(&s->reused);
    s->queue = queue;
    InterlockedExchange64(&s->enqueued_at[index], now());
    return gen;
}

// Right before the result is written (afterwards the title may enqueue the index again).
inline void on_complete(StatusStats* s, unsigned index, LONG gen) noexcept
{
    if (!s || index >= s->capacity || !gen) return;
    if (gen != s->enqueued_gen[index]) {
        InterlockedIncrement(&s->stale);
        return;
    }
    const LONG64 wait = now() - s->enqueued_at[index];
    InterlockedIncrement64(&s->completions);
    InterlockedIncrement64(&s->completions_total);
    InterlockedAdd64(&s->wait_ticks, wait);
    for (LONG64 seen = s->wait_max; wait > seen;) {
        const LONG64 prior = InterlockedCompareExchange64(&s->wait_max, wait, seen);
        if (prior == seen) break;
        seen = prior;
    }
    InterlockedExchange(&s->completed_gen[index], gen);
}

// IsComplete runs at tens of millions of calls per second while the title waits, so the poll counts are
// plain (unlocked) increments on the array's own counters: approximate when several threads poll one
// array or a report resets them mid-increment, but no locked instruction on the title's poll path.
inline void on_poll(StatusStats* s, bool complete, void* caller) noexcept
{
    if (!s) return;
    volatile LONG64& polls = complete ? s->polls_complete : s->polls_pending;
    polls = polls + 1;
    if (!s->poller) InterlockedCompareExchangePointer(&s->poller, caller, nullptr);
}

inline void module_offset(const void* address, char* out, std::size_t size) noexcept
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<const char*>(address), &module) && module) {
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') name = p + 1;
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    } else {
        std::snprintf(out, size, "%p", address);
    }
}

// Heartbeat. `queue_text` describes a queue's entry counts into a buffer (the stand-in's own state).
// `pending_result` is the value an index holds while pending.
template <class Log, class QueueText>
void report(int second, LONG pending_result, Log log, QueueText queue_text)
{
    if (!enabled()) return;
    const LONG count = array_count < max_arrays ? array_count : max_arrays;
    LONG64 completions = 0, wait_ticks = 0, wait_max = 0, polls_pending = 0, polls_complete = 0;
    LONG reused = 0, stale = 0, max_at = -1, active = 0;
    char pollers[256] = "";
    int used = 0;
    void* seen_pollers[4] = {};
    for (LONG i = 0; i < count; ++i) {
        StatusStats* s = arrays[i];
        if (!s) continue;
        const LONG64 c = InterlockedExchange64(&s->completions, 0);
        const LONG64 t = InterlockedExchange64(&s->wait_ticks, 0);
        const LONG64 m = InterlockedExchange64(&s->wait_max, 0);
        const LONG64 pp = InterlockedExchange64(&s->polls_pending, 0);
        const LONG64 pc = InterlockedExchange64(&s->polls_complete, 0);
        if (c || pp || pc) ++active;
        completions += c;
        wait_ticks += t;
        if (m > wait_max) {
            wait_max = m;
            max_at = i;
        }
        polls_pending += pp;
        polls_complete += pc;
        reused += s->reused;
        stale += s->stale;
        void* poller = s->poller;
        bool known = !poller;
        for (void*& p : seen_pollers) {
            if (p == poller) known = true;
            if (!known && !p) {
                p = poller;
                char where[160];
                module_offset(poller, where, sizeof(where));
                if (used < static_cast<int>(sizeof(pollers)) - 1)
                    used += std::snprintf(pollers + used, sizeof(pollers) - used, " %s", where);
                known = true;
            }
        }
    }
    if (completions || polls_pending || polls_complete || reused || stale)
        log("x2w-dstorage status %d s: arrays %ld (active %ld), completions %lld, wait mean %.3f ms max %.3f ms (array #%ld); "
            "IsComplete polls (approximate) pending %lld complete %lld; reused before complete %ld, stale writes %ld (since start); first pollers:%s",
            second, static_cast<long>(array_count), static_cast<long>(active), completions,
            completions ? ms(wait_ticks) / static_cast<double>(completions) : 0.0, ms(wait_max), static_cast<long>(max_at),
            polls_pending, polls_complete, static_cast<long>(reused), static_cast<long>(stale), used ? pollers : " none");
    int shown = 0;
    const LONG64 t = now();
    for (LONG i = 0; i < count && shown < 8; ++i) {
        StatusStats* s = arrays[i];
        if (!s) continue;
        for (unsigned index = 0; index < s->capacity && shown < 8; ++index) {
            if (s->results[index] != pending_result || !s->enqueued_gen[index]) continue;
            const double age = ms(t - s->enqueued_at[index]);
            if (age < 1000.0) continue;
            char queue[160] = "";
            queue_text(s->queue, queue, sizeof(queue));
            log("x2w-dstorage status PENDING array #%ld index %u for %.1f s (enqueued %ld, completed %ld; %s)", static_cast<long>(i), index,
                age / 1000.0, static_cast<long>(s->enqueued_gen[index]), static_cast<long>(s->completed_gen[index]), queue);
            ++shown;
        }
    }
}

}  // namespace x2w_dstorage_stats
