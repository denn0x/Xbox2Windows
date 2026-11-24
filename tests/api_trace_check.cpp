// Source-built control for the API trace's decision about what a provider call wrote.
//
// The trace compares only the outputs a contract declares, over each one's own width, and reports what
// it observed. The four cases here are the ones a reviewer asked for: argument index 0 (which must not
// double as "nothing changed"), two outputs changed at once (both indices reported), an exact and a
// short caller buffer (the width must come from the declaration and from the caller's capacity), and
// the same-value case (a provider may write the value that was already there, which is "same", not
// "not written").
#include "startup_stubs/api_trace.h"

#include <cstdio>
#include <cstring>

namespace trace = x2w_api_trace;

namespace {
int failures;
using Bytes = unsigned char[trace::max_snapshot];

void check(const char* label, bool condition)
{
    std::printf("%s: %s\n", label, condition ? "ok" : "MISMATCH");
    if (!condition) ++failures;
}

// One declared output at argument index 0 with a 16-byte width (the connectivity hint's shape).
void index_zero()
{
    const trace::DeclaredOut contract{0x37e56907, 15, 0, 16, trace::no_argument, trace::no_argument, 0};
    trace::Report report{};
    bool compared[2] = {true, false};
    std::size_t bytes[2] = {16, 0};
    unsigned char before[2][trace::max_snapshot] = {};
    unsigned char after[2][trace::max_snapshot] = {};
    after[0][0] = 1;  // only the output changed
    report = trace::decide(&contract, compared, before, after, bytes);
    char text[48] = {};
    trace::format(report, &contract, text, sizeof(text));
    check("index 0 change is reported", report.outcome == trace::Outcome::Changed && std::strcmp(text, "changed:0") == 0);
}

// Two declared outputs whose bytes both change: both argument indices must be listed.
void two_outputs()
{
    const trace::DeclaredOut contract{0x01acd177, 20, 3, 1, trace::no_argument, 4, 4};
    bool compared[2] = {true, true};
    std::size_t bytes[2] = {1, 4};
    unsigned char before[2][trace::max_snapshot] = {};
    unsigned char after[2][trace::max_snapshot] = {};
    after[0][0] = 1;
    after[1][0] = 2;
    const trace::Report report = trace::decide(&contract, compared, before, after, bytes);
    char text[48] = {};
    trace::format(report, &contract, text, sizeof(text));
    check("both changed outputs are listed", report.changed_count == 2 && std::strcmp(text, "changed:3,4") == 0);
}

// A one-byte output must be compared over one byte: a difference in the byte after it is not its change.
void exact_and_short_buffer()
{
    const trace::DeclaredOut flag{0x01acd177, 13, 1, 1, trace::no_argument, trace::no_argument, 0};
    bool compared[2] = {true, false};
    std::size_t bytes[2] = {1, 0};
    unsigned char before[2][trace::max_snapshot] = {};
    unsigned char after[2][trace::max_snapshot] = {};
    after[0][1] = 0x7f;  // outside the declared width
    trace::Report report = trace::decide(&flag, compared, before, after, bytes);
    check("a byte past the width is not this output's change", report.outcome == trace::Outcome::Same);

    // A caller buffer shorter than the declared width is compared over the caller's capacity only.
    const trace::DeclaredOut string{0x01acd177, 15, 3, 0, 2, 4, 8};
    std::size_t short_bytes[2] = {4, 8};
    unsigned char short_before[2][trace::max_snapshot] = {};
    unsigned char short_after[2][trace::max_snapshot] = {};
    short_after[0][4] = 0x11;  // past the four bytes the caller offered
    report = trace::decide(&string, compared, short_before, short_after, short_bytes);
    check("a short caller buffer is compared over its capacity", report.outcome == trace::Outcome::Same);
}

// A provider that writes the value already there is "same", and a declared output that could not be
// compared is "partial", not "same".
void same_value_and_partial()
{
    const trace::DeclaredOut contract{0x01acd177, 20, 3, 1, trace::no_argument, 4, 4};
    bool compared[2] = {true, true};
    std::size_t bytes[2] = {1, 4};
    unsigned char before[2][trace::max_snapshot] = {};
    unsigned char after[2][trace::max_snapshot] = {};
    trace::Report report = trace::decide(&contract, compared, before, after, bytes);
    char text[48] = {};
    trace::format(report, &contract, text, sizeof(text));
    check("identical bytes are reported as same", report.outcome == trace::Outcome::Same && std::strcmp(text, "same") == 0);

    bool partial_compared[2] = {true, false};
    std::size_t partial_bytes[2] = {1, 0};
    report = trace::decide(&contract, partial_compared, before, after, partial_bytes);
    trace::format(report, &contract, text, sizeof(text));
    check("one comparable output is partial", report.outcome == trace::Outcome::Same && std::strcmp(text, "same,partial") == 0);

    bool none[2] = {false, false};
    std::size_t none_bytes[2] = {0, 0};
    report = trace::decide(&contract, none, before, after, none_bytes);
    trace::format(report, &contract, text, sizeof(text));
    check("no comparable output is unreadable", report.outcome == trace::Outcome::Unreadable &&
                                                      std::strcmp(text, "unreadable") == 0);
}

// A slot with no contract is not read at all and reports "?".
void unknown_contract()
{
    trace::Report report{};
    bool compared[2] = {};
    std::size_t bytes[2] = {};
    unsigned char before[2][trace::max_snapshot] = {};
    unsigned char after[2][trace::max_snapshot] = {};
    report = trace::decide(nullptr, compared, before, after, bytes);
    char text[48] = {};
    trace::format(report, nullptr, text, sizeof(text));
    check("undeclared slot reports unknown", report.outcome == trace::Outcome::Unknown && std::strcmp(text, "?") == 0);
}

// The declared table must answer for the slots the stand-in actually answers, including the two the
// review found missing.
void declared_table()
{
    check("XUserGetId is slot 11", trace::declared(0x01acd177, 11) != nullptr);
    check("XUserFindUserById is slot 12", trace::declared(0x01acd177, 12) != nullptr);
    check("secondary gamertag interface is declared", trace::declared(0xcef4fac0, 3) != nullptr);
    check("connectivity hint is declared", trace::declared(0x37e56907, 15) != nullptr);
    check("undeclared slot has no contract", trace::declared(0x01acd177, 33) == nullptr);
}
}  // namespace

int main()
{
    index_zero();
    two_outputs();
    exact_and_short_buffer();
    same_value_and_partial();
    unknown_contract();
    declared_table();
    if (failures) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}