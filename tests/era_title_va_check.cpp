#include "era_title_va.h"

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace {
int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint64_t kib = 1024;

int check_translate()
{
    era_title_va::Table table;
    expect(table.add(0x10000, 64 * kib, 0x7000'0000), "add first range");
    expect(table.add(0x40000, 4 * kib, 0x9000'0000), "add second range");
    expect(table.translate(0x10000) == 0x7000'0000, "range start");
    expect(table.translate(0x10000 + 0x1234, 4) == 0x7000'1234, "offset inside range");
    expect(table.translate(0x10000 + 64 * kib - 4, 4) == 0x7000'0000 + 64 * kib - 4, "last word");
    expect(table.translate(0x40000 + 8) == 0x9000'0008, "second range");
    expect(table.translate(0x10000, 0) == 0x7000'0000, "zero byte count treated as one");
    return failures;
}

int check_bounds()
{
    era_title_va::Table table;
    expect(table.add(0x10000, 64 * kib, 0x7000'0000), "add range");
    expect(table.translate(0xffff) == 0, "byte before range");
    expect(table.translate(0x10000 + 64 * kib) == 0, "byte after range");
    expect(table.translate(0x10000 + 64 * kib - 2, 4) == 0, "span crossing the end");
    expect(table.translate(0x10000, 64 * kib + 1) == 0, "span longer than the range");
    expect(table.translate(UINT64_MAX - 1, 4) == 0, "wrapping span");
    expect(era_title_va::Table{}.translate(0x10000) == 0, "empty table");
    return failures;
}

int check_registry()
{
    era_title_va::Table table;
    expect(!table.add(0x10000, 0, 0x7000'0000), "empty range rejected");
    expect(!table.add(UINT64_MAX - 15, 32, 0x7000'0000), "wrapping title range rejected");
    expect(!table.add(0x10000, 16, 0), "zero GPU address rejected");
    expect(!table.add(0x10000, 32, UINT64_MAX - 15), "wrapping GPU range rejected");
    expect(table.add(0x20000, 0x1000, 0x7000'0000), "add middle range");
    expect(!table.add(0x1f000, 0x1001, 0x8000'0000), "overlap from below rejected");
    expect(!table.add(0x20fff, 0x10, 0x8000'0000), "overlap from above rejected");
    expect(!table.add(0x20000, 0x1000, 0x8000'0000), "duplicate rejected");
    expect(table.add(0x1f000, 0x1000, 0x8000'0000), "adjacent below accepted");
    expect(table.add(0x21000, 0x1000, 0x9000'0000), "adjacent above accepted");
    const auto ranges = table.ranges();
    expect(ranges.size() == 3, "three ranges listed");
    expect(ranges.size() == 3 && ranges[0].title_begin == 0x1f000 && ranges[1].title_begin == 0x20000 &&
               ranges[2].title_begin == 0x21000, "ranges sorted by title address");
    expect(ranges.size() == 3 && ranges[1].title_end == 0x21000 && ranges[1].gpu_begin == 0x7000'0000,
           "range fields");
    expect(table.remove(0x20000), "remove middle range");
    expect(!table.remove(0x20000), "second remove fails");
    expect(!table.remove(0x1f800), "remove needs the range start");
    expect(table.translate(0x20010) == 0, "removed range no longer translates");
    expect(table.translate(0x21010) == 0x9000'0010, "other ranges still translate");
    return failures;
}
}

int check_shader()
{
    era_title_va::Table table;
    expect(table.add(0x1'ffff'0000, 0x20000, 0x7000'0000), "add range crossing a 4 GiB line");
    expect(table.add(0x10000, 0x1000, 0x9000'0000), "add low range");
    std::vector<era_title_va::ShaderRange> packed;
    expect(era_title_va::shader_ranges(table.ranges(), packed), "pack table ranges");
    expect(packed.size() == 2, "two packed ranges");
    if (packed.size() == 2) {
        expect(packed[0].begin_low == 0x10000 && packed[0].begin_high == 0 && packed[0].end_low == 0x11000 &&
                   packed[0].end_high == 0 && packed[0].slot == 0 && packed[0].reserved == 0,
               "low range words and slot");
        expect(packed[1].begin_low == 0xffff0000 && packed[1].begin_high == 1 && packed[1].end_low == 0x10000 &&
                   packed[1].end_high == 2 && packed[1].slot == 1,
               "high range split into low and high words");
    }
    // Adjacent ranges, which Table::add accepts, pack too; here the first one ends on a 4 GiB line.
    era_title_va::Table adjacent;
    expect(adjacent.add(0xffff'f000, 0x1000, 0x7000'0000), "add range ending on a 4 GiB line");
    expect(adjacent.add(0x1'0000'0000, 0x1000, 0x9000'0000), "add adjacent range starting on it");
    expect(era_title_va::shader_ranges(adjacent.ranges(), packed) && packed.size() == 2, "adjacent ranges packed");
    if (packed.size() == 2) {
        expect(packed[0].begin_low == 0xfffff000 && packed[0].begin_high == 0 && packed[0].end_low == 0 &&
                   packed[0].end_high == 1 && packed[0].slot == 0 && packed[0].reserved == 0,
               "range ending on the line");
        expect(packed[1].begin_low == 0 && packed[1].begin_high == 1 && packed[1].end_low == 0x1000 &&
                   packed[1].end_high == 1 && packed[1].slot == 1 && packed[1].reserved == 0,
               "range starting on the line");
    }
    using R = era_title_va::Range;
    expect(!era_title_va::shader_ranges({R{0x20000, 0x21000, 1}, R{0x10000, 0x11000, 2}}, packed) && packed.empty(),
           "unsorted ranges rejected");
    expect(!era_title_va::shader_ranges({R{0x10000, 0x21000, 1}, R{0x20000, 0x30000, 2}}, packed) && packed.empty(),
           "overlapping ranges rejected");
    expect(!era_title_va::shader_ranges({R{0x10000, 0x10000 + (1ull << 32), 1}}, packed) && packed.empty(),
           "4 GiB range rejected");
    expect(era_title_va::shader_ranges({R{0x10000, 0x10000 + (1ull << 32) - 4, 1}}, packed) && packed.size() == 1,
           "range just under 4 GiB accepted");
    expect(!era_title_va::shader_ranges({R{0x10000, 0x10000, 1}}, packed) && packed.empty(), "empty range rejected");
    expect(!era_title_va::shader_ranges({R{0x10002, 0x11000, 1}}, packed) && packed.empty(),
           "range not starting on a 4-byte boundary rejected");
    expect(era_title_va::shader_ranges({R{0x10000, 0x10ffe, 1}}, packed) && packed.size() == 1,
           "range whose length is not a multiple of 4 accepted");
    expect(era_title_va::shader_ranges({}, packed) && packed.empty(), "no ranges packs to nothing");
    return failures;
}

int main(int argc, char** argv)
{
    const std::string_view scenario = argc == 2 ? argv[1] : ""sv;
    if (scenario == "translate"sv) return check_translate();
    if (scenario == "bounds"sv) return check_bounds();
    if (scenario == "registry"sv) return check_registry();
    if (scenario == "shader"sv) return check_shader();
    std::fprintf(stderr, "usage: era_title_va_check translate|bounds|registry|shader\n");
    return 2;
}
