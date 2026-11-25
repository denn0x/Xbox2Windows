// LOCAL (textures): src/startup_stubs/buffer_address_index.h against a brute-force walk.
//   buffer_address_index_check            explicit overlap / move / erase cases, then 200000 random operations on
//                                         overlapping ranges of every width class, each query compared with the walk
//   buffer_address_index_check --mutate   the same with the exact-start window (negative control: must fail)
// Synthetic ranges only.
#include "startup_stubs/buffer_address_index.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

using Index = x2w_startup::AddressIndex<int>;

namespace {
int failures;
void fail(const char* what, std::uint64_t a, std::uint64_t b)
{
    if (failures++ < 10) std::printf("FAIL %s (0x%llx, %llu)\n", what, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

std::set<int> query(const Index& index, std::uint64_t address, std::uint64_t bytes)
{
    std::set<int> out;
    index.containing(address, bytes, [&](int key, std::uint64_t, std::uint64_t) {
        out.insert(key);
        return true;
    });
    return out;
}
std::set<int> walk(const std::map<int, std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t address, std::uint64_t bytes)
{
    std::set<int> out;
    for (const auto& [key, r] : ranges) {
        const auto [base, width] = r;
        if (address < base || address - base >= width || bytes > width - (address - base)) continue;
        out.insert(key);
    }
    return out;
}

void explicit_cases(Index::Mutation m)
{
    Index index(m);
    index.insert(1, 0x10000, 0x1000);    // small
    index.insert(2, 0x10800, 0x100);     // nested in 1
    index.insert(3, 0x0, 0x40000000);    // 1 GiB, covers everything below
    if (query(index, 0x10850, 4) != std::set<int>{1, 2, 3}) fail("overlap: nested ranges", 0x10850, 4);
    if (query(index, 0x10fff, 1) != std::set<int>{1, 3}) fail("overlap: last byte of 1", 0x10fff, 1);
    if (query(index, 0x10ffe, 4) != std::set<int>{3}) fail("crossing the end of 1", 0x10ffe, 4);
    if (query(index, 0x11000, 1) != std::set<int>{3}) fail("one past 1", 0x11000, 1);
    index.erase(2);
    if (query(index, 0x10850, 4) != std::set<int>{1, 3}) fail("removal of 2", 0x10850, 4);
    index.insert(1, 0x20000, 0x1000);  // move
    if (query(index, 0x10850, 4) != std::set<int>{3} || query(index, 0x20010, 4) != std::set<int>{1, 3}) fail("move of 1", 0x20010, 4);
    index.erase(3);
    if (!query(index, 0x10, 1).empty() || index.size() != 1) fail("removal of 3", 0x10, 1);
    if (index.erase(3)) fail("second erase", 3, 0);
}

void random_cases(Index::Mutation m, unsigned operations)
{
    Index index(m);
    std::map<int, std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::mt19937_64 rng(20261125);
    const std::uint64_t widths[] = {16, 256, 4096, 65536, 65537, 1ull << 20, (1ull << 20) + 16, 1ull << 24, 3ull << 24, 1ull << 28, 1ull << 30};
    const std::uint64_t space = 1ull << 32;  // dense: ranges overlap often
    unsigned queries = 0, hits = 0;
    for (unsigned op = 0; op < operations; ++op) {
        const unsigned kind = rng() % 10;
        const int key = static_cast<int>(rng() % 4000);
        if (kind < 3) {  // insert or move
            const std::uint64_t width = widths[rng() % std::size(widths)] - (rng() % 2 ? rng() % 8 : 0);
            const std::uint64_t base = (rng() % space) & ~(rng() % 2 ? std::uint64_t{0xfff} : std::uint64_t{0});
            index.insert(key, base, width);
            ranges[key] = {base, width};
        } else if (kind < 4) {
            const bool was = ranges.erase(key) != 0;
            if (index.erase(key) != was) fail("erase result", static_cast<std::uint64_t>(key), was);
        } else {  // query: inside a known range (start, interior, last byte), or anywhere
            std::uint64_t address = rng() % space;
            if (!ranges.empty() && rng() % 2) {
                auto it = ranges.begin();
                std::advance(it, static_cast<long>(rng() % ranges.size()));
                const auto [base, width] = it->second;
                const unsigned where = rng() % 3;
                address = where == 0 ? base : where == 1 ? base + rng() % width : base + width - 1;
            }
            const std::uint64_t bytes = rng() % 3 == 0 ? 1 : 1 + rng() % 8192;
            const auto got = query(index, address, bytes), want = walk(ranges, address, bytes);
            ++queries;
            hits += !want.empty();
            if (got != want) fail("random query differs from the walk", address, bytes);
        }
    }
    if (index.size() != ranges.size()) fail("size", index.size(), ranges.size());
    std::printf("buffer_address_index%s: %u operations, %u queries (%u with a containing range), %zu ranges at the end, %d failures\n",
                m == Index::Mutation::exact_start ? " --mutate" : "", operations, queries, hits, ranges.size(), failures);
}
}  // namespace

int main(int argc, char** argv)
{
    const auto m = argc == 2 && std::strcmp(argv[1], "--mutate") == 0 ? Index::Mutation::exact_start : Index::Mutation::none;
    if (argc > 2 || (argc == 2 && m == Index::Mutation::none)) {
        std::puts("usage: buffer_address_index_check [--mutate]");
        return 2;
    }
    explicit_cases(m);
    random_cases(m, 200000);
    return failures ? 1 : 0;
}
