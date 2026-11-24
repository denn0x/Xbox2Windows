#include "startup_stubs/placement_publish.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace {
long fail_after = -1;
bool injected;
}

void* operator new(std::size_t size)
{
    if (fail_after == 0) {
        fail_after = -1;
        injected = true;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    using Key = unsigned;
    struct Record { std::uint64_t bytes; };
    for (long n = 0; n < 32; ++n) {
        SRWLOCK lock = SRWLOCK_INIT;
        std::unordered_map<Key, Record> records{{1, {4}}};
        std::multimap<std::uint64_t, Key> by_start{{0x1000, 1}};
        std::uint64_t max_width = 4;
        const auto publish = [&](Key owner) {
            return x2w_startup::publish_placement(lock, [&]() -> auto& { return records; }, by_start,
                max_width, owner, 0x2000, Record{64});
        };

        fail_after = n;
        injected = false;
        const HRESULT hr = publish(2);
        fail_after = -1;
        if (!TryAcquireSRWLockExclusive(&lock)) return 1;
        ReleaseSRWLockExclusive(&lock);
        if (injected) {
            if (hr != E_OUTOFMEMORY || records.size() != 1 || by_start.size() != 1 || max_width != 4 ||
                records.at(1).bytes != 4 || by_start.begin()->second != 1) return 2;
            if (publish(2) != S_OK) return 3;
        } else if (hr != S_OK) return 4;
        if (records.size() != 2 || records.at(2).bytes != 64 || by_start.count(0x2000) != 1 || max_width != 64) return 5;
        if (publish(2) != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || by_start.size() != 2) return 6;
        if (!injected) {
            const auto reject_invalid = [&](Key bad_owner, std::uint64_t bad_start, std::uint64_t bad_bytes) {
                bool records_accessed = false;
                const HRESULT bad_hr = x2w_startup::publish_placement(
                    lock, [&]() -> auto& { records_accessed = true; return records; }, by_start, max_width,
                    bad_owner, bad_start, Record{bad_bytes});
                if (bad_hr != E_INVALIDARG || records_accessed) return false;
                if (!TryAcquireSRWLockExclusive(&lock)) return false;
                ReleaseSRWLockExclusive(&lock);
                return records.size() == 2 && by_start.size() == 2 && max_width == 64;
            };
            constexpr std::uint64_t u64_max = (std::numeric_limits<std::uint64_t>::max)();
            if (!reject_invalid(3, 0, 128)) return 9;
            if (!reject_invalid(1, 0, 128)) return 10;
            if (!reject_invalid(4, 0x3000, 0)) return 11;
            if (!reject_invalid(5, u64_max - 15, 16)) return 12;
            if (!reject_invalid(5, u64_max - 15, 128)) return 13;
            if (!reject_invalid(5, u64_max, 1)) return 14;
            const HRESULT edge_hr = x2w_startup::publish_placement(
                lock, [&]() -> auto& { return records; }, by_start, max_width,
                Key{6}, u64_max - 15, Record{15});
            if (edge_hr != S_OK || records.size() != 3 || records.at(6).bytes != 15 ||
                by_start.size() != 3 || by_start.count(u64_max - 15) != 1 || max_width != 64) return 15;
            std::printf("placement registration: %ld allocation failures rolled back and retried; invalid ranges rejected\n", n);
            return n >= 2 ? 0 : 7;
        }
    }
    return 8;
}
