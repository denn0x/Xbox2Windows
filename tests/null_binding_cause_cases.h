#pragma once

#include "../src/startup_stubs/null_binding_cause.h"

// Pure checks of the null-binding cause classification; no device needed.
inline bool null_binding_cause_cases()
{
    using namespace x2w_local;
    long by_cause[null_cause_count] = {};
    long total = 0;
    auto count = [&](const BindingFacts& facts, NullCause expected) {
        const NullCause cause = classify_null_binding(facts);
        if (cause != expected) return false;
        if (cause != NullCause::none) {
            ++by_cause[static_cast<int>(cause)];
            ++total;
        }
        return true;
    };
    BindingFacts tex;
    tex.texture = tex.from_table = tex.entry_readable = tex.token_magic = tex.token_known = tex.resource_wrapper = true;
    BindingFacts buf;
    buf.from_table = buf.entry_readable = buf.bound = true;

    // Negative controls: valid bindings add nothing.
    if (!count(tex, NullCause::none) || !count(buf, NullCause::none) || total != 0) return false;
    BindingFacts hardware = buf;  // a buffer from a vertex or root source has no table entry
    hardware.from_table = hardware.entry_readable = false;
    if (!count(hardware, NullCause::none)) return false;

    BindingFacts f = tex;
    f.entry_readable = false;
    if (!count(f, NullCause::table_entry)) return false;
    f = buf;
    f.entry_readable = false;
    f.bound = true;  // the identity fallback bound an upload-ring view in place of the unreadable entry
    if (!count(f, NullCause::none)) return false;
    f.bound = false;
    if (!count(f, NullCause::table_entry)) return false;
    f = buf;
    f.bound = false;
    if (!count(f, NullCause::buffer_unreadable)) return false;
    f = hardware;
    f.bound = false;
    if (!count(f, NullCause::buffer_unreadable)) return false;
    f = buf;
    f.token_magic = true;
    f.token_known = false;
    f.bound = true;  // same, for an unknown token
    if (!count(f, NullCause::none)) return false;
    f.bound = false;
    if (!count(f, NullCause::unknown_token)) return false;
    f = tex;
    f.token_known = false;
    f.resource_wrapper = false;
    if (!count(f, NullCause::unknown_token)) return false;
    f = tex;
    f.token_magic = f.token_known = false;
    if (!count(f, NullCause::unknown_token)) return false;
    f = tex;
    f.resource_wrapper = false;
    if (!count(f, NullCause::texture_wrapper)) return false;
    f.dimension_matches = false;  // a missing wrapper is counted once, not again as a mismatch
    if (!count(f, NullCause::texture_wrapper)) return false;
    f = tex;
    f.dimension_matches = false;
    if (!count(f, NullCause::dimension_mismatch)) return false;

    long sum = 0;
    for (int i = 0; i < null_cause_count; ++i) sum += by_cause[i];
    return total == 10 && sum == total && by_cause[static_cast<int>(NullCause::table_entry)] == 2 &&
           by_cause[static_cast<int>(NullCause::buffer_unreadable)] == 2 &&
           by_cause[static_cast<int>(NullCause::unknown_token)] == 3 &&
           by_cause[static_cast<int>(NullCause::texture_wrapper)] == 2 &&
           by_cause[static_cast<int>(NullCause::dimension_mismatch)] == 1;
}
