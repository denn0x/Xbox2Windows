// Experimental (docs/startup-runtime.md): caps for diagnostic log lines in the startup stand-ins.
//
// The old pattern `static LONG calls; if (InterlockedIncrement(&calls) <= N) log(...)` wraps after
// 2^31 calls: the counter turns negative, `<= N` holds again, and the line is logged on every call for
// the next two billion calls. A title that polls a stand-in at tens of millions of calls per second
// reaches that in under a minute (a status poll did, and wrote 7.8 million identical lines).
#pragma once

#include <windows.h>

namespace x2w_local {

// True for the first `limit` calls that reach it, false for every later one, from any number of
// threads. The counter never goes past limit + 1, and once it is there a call only reads it: no
// interlocked write, so a hot caller does not keep moving the counter's cache line between cores.
// For a pure log budget only: a counter that is also reported as a total must keep counting (see
// first_of below).
inline bool first_calls(volatile LONG& counter, LONG limit) noexcept
{
    LONG seen = counter;
    for (;;) {
        if (seen > limit) return false;
        const LONG prior = InterlockedCompareExchange(&counter, seen + 1, seen);
        if (prior == seen) return seen + 1 <= limit;
        seen = prior;
    }
}

// For a count that is also a statistic (it must keep counting): true when the value an increment
// returned is one of the first `limit`, false once the count has wrapped negative.
inline bool first_of(LONG count, LONG limit) noexcept { return count > 0 && count <= limit; }

}  // namespace x2w_local
