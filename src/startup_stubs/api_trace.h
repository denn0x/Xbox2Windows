// Declared provider outputs for the opt-in API trace (X2W_TRACE_API), kept in one place so the
// decision about what was observed can be checked without a running title.
//
// The contracts below follow the answers in runtime_proxy.cpp. Only declared outputs are ever read:
// arguments are not probed as pointers, because a handle or a size that happens to look like an
// address would be read as if it were one.
//
// A width of 0 means the caller supplies the capacity in `size_argument`, so the comparison is bounded
// by that capacity. "same" therefore means every declared output was compared and held the same value;
// it does not mean the provider skipped its write, because a provider may legitimately write the value
// that was already there.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace x2w_api_trace {

constexpr unsigned char no_argument = 0xff;
constexpr int max_outputs = 2;

// The widest output a declaration describes; a caller-sized buffer is compared within this bound.
constexpr std::size_t max_snapshot = 64;

struct DeclaredOut {
    unsigned long provider;
    int slot;
    unsigned char first;          // argument index of the first output
    unsigned char first_bytes;    // its width, or 0 when the caller supplies the capacity
    unsigned char size_argument;  // argument index holding that capacity, or no_argument
    unsigned char second;         // second output, or no_argument
    unsigned char second_bytes;   // its width
};

inline const DeclaredOut* declared(unsigned long provider, int slot)
{
    static constexpr DeclaredOut table[] = {
        {0x01acd177, 3, 1, 8, no_argument, no_argument, 0},     // duplicate handle
        {0x01acd177, 8, 1, 8, no_argument, no_argument, 0},     // user add result: &handle
        {0x01acd177, 9, 1, 8, no_argument, no_argument, 0},     // local id
        {0x01acd177, 10, 1, 8, no_argument, no_argument, 0},    // find by local id
        {0x01acd177, 11, 1, 8, no_argument, no_argument, 0},    // XUserGetId: &id
        {0x01acd177, 12, 1, 8, no_argument, no_argument, 0},    // XUserFindUserById
        {0x01acd177, 13, 1, 1, no_argument, no_argument, 0},    // is guest: one byte
        {0x01acd177, 14, 1, 4, no_argument, no_argument, 0},    // state: DWORD
        {0x01acd177, 15, 3, 0, 2, 4, 8},                        // gamertag buffer (capacity in arg 2), &used
        {0x01acd177, 19, 1, 4, no_argument, no_argument, 0},    // age group: DWORD
        {0x01acd177, 20, 3, 1, no_argument, 4, 4},              // privilege: &has, &deny reason
        {0x01acd177, 40, 1, 8, no_argument, no_argument, 0},    // find for device
        {0xcef4fac0, 3, 3, 0, 2, 4, 8},                         // gamertag interface: buffer, &used
        {0x37e56907, 15, 0, 16, no_argument, no_argument, 0},   // connectivity hint: four words
        {0x0dd112ac, 3, 1, 8, no_argument, no_argument, 0},     // XStoreCreateContext: &context
    };
    for (const DeclaredOut& entry : table)
        if (entry.provider == provider && entry.slot == slot) return &entry;
    return nullptr;
}

enum class Outcome {
    Unknown,     // no declared contract: nothing was read
    Unreadable,  // a contract exists but no declared output could be compared
    Same,        // every declared output was compared and held the same value
    Changed,     // at least one declared output differs
};

struct Report {
    Outcome outcome = Outcome::Unknown;
    int changed_count = 0;
    int changed[max_outputs] = {};
    bool partial = false;  // some declared output could not be compared
};

// `compared` says whether each declared output's bytes were captured; `before` and `after` hold the
// bytes for the outputs that were captured, and `bytes` their widths.
inline Report decide(const DeclaredOut* contract, const bool* compared, const unsigned char (*before)[max_snapshot],
                     const unsigned char (*after)[max_snapshot], const std::size_t* bytes)
{
    Report report{};
    if (!contract) return report;
    const int declared_count = contract->second == no_argument ? 1 : 2;
    int checked = 0;
    for (int i = 0; i < declared_count; ++i) {
        if (!compared[i] || !bytes[i]) continue;
        ++checked;
        if (std::memcmp(before[i], after[i], bytes[i]) != 0) report.changed[report.changed_count++] = static_cast<int>(i);
    }
    report.partial = checked != declared_count;
    if (!checked) report.outcome = Outcome::Unreadable;
    else if (report.changed_count) report.outcome = Outcome::Changed;
    else report.outcome = Outcome::Same;
    return report;
}

// Renders the report: "?", "unreadable", "same", "changed", or "changed:0,1" with ",partial" appended
// when some declared output could not be compared. The argument indices in the text are the ones the
// caller passed, so index 0 is listed like any other.
inline void format(const Report& report, const DeclaredOut* contract, char* out, std::size_t size)
{
    switch (report.outcome) {
    case Outcome::Unknown: std::snprintf(out, size, "?"); return;
    case Outcome::Unreadable: std::snprintf(out, size, "unreadable"); return;
    case Outcome::Same: std::snprintf(out, size, "same%s", report.partial ? ",partial" : ""); return;
    case Outcome::Changed: {
        const int first = contract ? contract->first : 0;
        const int second = contract && contract->second != no_argument ? contract->second : -1;
        const int a0 = report.changed[0] == 0 ? first : second;
        if (report.changed_count > 1) {
            const int a1 = report.changed[1] == 0 ? first : second;
            std::snprintf(out, size, "changed:%d,%d%s", a0, a1, report.partial ? ",partial" : "");
        } else {
            std::snprintf(out, size, "changed:%d%s", a0, report.partial ? ",partial" : "");
        }
        return;
    }
    }
}

}  // namespace x2w_api_trace