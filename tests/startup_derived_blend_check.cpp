// Checks src/startup_stubs/derived_blend.h without a device: which blend patch of a slot 55 patch
// list is found, the offset it is reported at, and the lists that have none.
#include "startup_stubs/derived_blend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using x2w_derived_blend::record_size;
int failures, cases;

// A record whose 328 descriptor bytes count up from seed, so a copy that starts a word early or
// late, or comes from another record, differs in every byte.
void add(std::vector<unsigned char>& list, std::uint32_t tag, unsigned char seed)
{
    const std::size_t at = list.size();
    list.resize(at + record_size);
    std::memcpy(list.data() + at, &tag, sizeof(tag));
    for (std::size_t i = sizeof(tag); i < record_size; ++i) list[at + i] = static_cast<unsigned char>(seed + i);
}

void found(const char* name, const std::vector<unsigned char>& list, std::size_t record)
{
    ++cases;
    D3D12_BLEND_DESC blend;
    std::memset(&blend, 0xee, sizeof(blend));
    std::size_t offset = ~std::size_t{0};
    const bool ok = x2w_derived_blend::last_blend(list.data(), list.size(), blend, offset);
    const std::size_t expected = record * record_size + sizeof(std::uint32_t);
    if (!ok || offset != expected || std::memcmp(&blend, list.data() + expected, sizeof(blend))) {
        std::printf("FAIL %s: found %d at +0x%zx, expected record %zu at +0x%zx%s\n", name, ok, offset, record, expected,
                    ok && offset == expected ? " (descriptor bytes differ)" : "");
        ++failures;
    }
}

void none(const char* name, const unsigned char* records, std::size_t bytes)
{
    ++cases;
    D3D12_BLEND_DESC blend{};
    std::size_t offset = 0;
    if (x2w_derived_blend::last_blend(records, bytes, blend, offset)) {
        std::printf("FAIL %s: found a blend patch at +0x%zx\n", name, offset);
        std::fflush(stdout);  // a scan that accepts a partial record may crash on a later case
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // --mutate first-wins: the expectations of a scan that keeps the first blend patch (a negative
    // control: the check must then fail on the lists with two of them).
    const bool first_wins = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "first-wins") == 0;
    if (argc > 1 && !first_wins) {
        std::printf("unknown mutation\n");
        return 2;
    }

    std::vector<unsigned char> list;
    add(list, 2, 0x10);
    found("one blend patch", list, 0);
    add(list, 2, 0x80);
    found("two blend patches", list, first_wins ? 0 : 1);
    add(list, 3, 0x40);
    add(list, 1, 0x50);
    found("two blend patches, then parts 3 and 1", list, first_wins ? 0 : 1);

    list.clear();
    add(list, 2, 0x10);
    add(list, 3, 0x20);
    add(list, 0, 0x30);
    found("blend patch first, then parts 3 and 0", list, 0);
    list.clear();
    add(list, 3, 0x20);
    add(list, 2, 0x10);
    add(list, 0, 0x30);
    found("blend patch between parts 3 and 0", list, 1);

    list.clear();
    for (std::uint32_t tag : {0u, 1u, 3u, 0x102u, 0x2000000u, 0xffffffffu}) add(list, tag, static_cast<unsigned char>(tag));
    none("no part 2 (0, 1, 3, 0x102, 0x2000000, ~0)", list.data(), list.size());

    // The size must be whole records: a 4-byte prefix before a whole blend patch is refused.
    list.clear();
    list.resize(sizeof(std::uint32_t));
    add(list, 2, 0x10);
    none("a blend patch after a 4-byte prefix", list.data(), list.size());
    none("a blend patch one byte short", list.data() + sizeof(std::uint32_t), record_size - 1);
    none("no records", list.data(), 0);
    none("null records, no bytes", nullptr, 0);
    none("null records, one record of bytes", nullptr, record_size);

    if (failures) {
        std::printf("startup_derived_blend: %d of %d FAILED\n", failures, cases);
        return 1;
    }
    std::printf("startup_derived_blend: PASS (%d cases)\n", cases);
    return 0;
}
