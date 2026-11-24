// publish_placement (src/startup_stubs/placement_publish.h) without a device. The record is read and
// written while the caller's lock is held exclusively, because binding lookup reads the same maps
// under that lock in shared mode. max_width stays the widest record published in the group, because
// lookup starts its search at address - max_width + 1.
#include "startup_stubs/placement_publish.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {
SRWLOCK lock = SRWLOCK_INIT;
int probes;
int not_exclusive; // probes that could take the lock in shared mode

void probe()
{
    ++probes;
    if (TryAcquireSRWLockShared(&lock)) {
        ReleaseSRWLockShared(&lock);
        ++not_exclusive;
    }
}

// The registry copies the record into its node, so the copy shows whether the lock is held then.
struct Record {
    std::uint64_t bytes;
    explicit Record(std::uint64_t value) : bytes(value) {}
    Record(const Record& other) : bytes(other.bytes) { probe(); }
    Record& operator=(const Record&) = default;
};

int fail(int code, const char* what)
{
    std::fprintf(stderr, "placement publish: %s\n", what);
    return code;
}
} // namespace

int main(int argc, char** argv)
{
    // The control expects the narrower second record to shrink the window and must fail.
    const bool shrink = argc == 3 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "shrink") == 0;
    if (argc != 1 && !shrink) return fail(64, "usage: startup_placement_publish_check [--mutate shrink]");

    using Key = unsigned;
    std::unordered_map<Key, Record> records;
    std::multimap<std::uint64_t, Key> by_start;
    std::uint64_t max_width = 0;

    // Widths of the large group (above 64 KiB). Expected max_width after each one:
    // 1 MiB -> 0x100000; 128 KiB keeps 0x100000; 4 MiB -> 0x400000; 256 KiB keeps 0x400000.
    // With 0x20000 after the second record, a lookup of the last byte of the first record
    // (0x100FFFFF) would start at 0x100E0000 and miss the record at 0x10000000.
    struct Step { Key owner; std::uint64_t start, bytes, expected_width; };
    const Step steps[] = {
        {1, 0x10000000, 0x100000, 0x100000},
        {2, 0x20000000, 0x20000, shrink ? 0x20000u : 0x100000u},
        {3, 0x30000000, 0x400000, 0x400000},
        {4, 0x40000000, 0x40000, 0x400000},
    };
    int published = 0;
    for (const Step& step : steps) {
        const Record record(step.bytes);
        probes = not_exclusive = 0;
        const HRESULT hr = x2w_startup::publish_placement(lock, [&]() -> auto& { probe(); return records; },
            by_start, max_width, step.owner, step.start, record);
        ++published;
        if (hr != S_OK) return fail(1, "a new owner was not published");
        // One probe when the records are fetched and one when the new record is copied in.
        if (probes != 2) return fail(2, "the records were not fetched once and the record copied once");
        if (not_exclusive) return fail(3, "the record was published without the lock held exclusively");
        if (!TryAcquireSRWLockExclusive(&lock)) return fail(4, "the lock is still held after the call");
        ReleaseSRWLockExclusive(&lock);
        if (max_width != step.expected_width) {
            std::fprintf(stderr, "after owner %u: max_width 0x%llx, expected 0x%llx\n", step.owner,
                         static_cast<unsigned long long>(max_width),
                         static_cast<unsigned long long>(step.expected_width));
            return fail(5, "max_width is not the widest record published");
        }
        if (records.size() != static_cast<std::size_t>(published) || records.at(step.owner).bytes != step.bytes)
            return fail(6, "the record was not stored under its owner");
        if (by_start.size() != static_cast<std::size_t>(published) || by_start.count(step.start) != 1 ||
            by_start.find(step.start)->second != step.owner)
            return fail(7, "the index does not map the start to its owner");
    }
    std::printf("placement publish: 4 records under the exclusive lock, max_width 0x100000 then 0x400000\n");
    return 0;
}
