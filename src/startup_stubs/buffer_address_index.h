#pragma once
// LOCAL (textures): an address-ordered index of [base, base + width) ranges for containment queries.
//
// existing_memory_buffer (d3d12_memory_srv.inl) used to walk every resource record for each slot-70 buffer call that
// no placed buffer covers. In run 113 that was 121161 wrappers for each of 150511 "no buffer" calls. This index
// answers "which ranges contain [address, address + bytes)" by scanning only the ranges that start close enough
// below the address to reach it.
//
// How the scan is bounded: ranges sit in width classes (up to 64 KiB, 1 MiB, 16 MiB, 256 MiB, larger). Each class
// keeps the largest width it ever held. A query visits in each class only the starts in
// [address - max_width + 1, address]. The width bound never shrinks on erase, which is safe, merely wider. Keys are
// unique: inserting a key that is present moves it.
//
// Not thread-safe. The caller holds its own lock, note_lock in the stand-in: exclusive to change, shared to query.
// tests/buffer_address_index_check.cpp compares every query with a brute-force walk over random overlapping
// ranges, inserts, moves and erases. Its --mutate control (window = the exact start only) must fail.
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>

namespace x2w_startup {

template<class Key> class AddressIndex {
public:
    enum class Mutation { none, exact_start };  // exact_start: the test's negative control (ignores the window)
    explicit AddressIndex(Mutation m = Mutation::none) : mutation_(m) {}

    void insert(Key key, std::uint64_t base, std::uint64_t width)
    {
        erase(key);
        if (!width || width > ~std::uint64_t{} - base) return;
        Group& g = groups_[class_of(width)];
        g.by_start.emplace(base, Entry{key, width});
        if (width > g.max_width) g.max_width = width;
        where_[key] = {base, width};
    }
    bool erase(Key key)
    {
        const auto found = where_.find(key);
        if (found == where_.end()) return false;
        const auto [base, width] = found->second;
        Group& g = groups_[class_of(width)];
        const auto [first, last] = g.by_start.equal_range(base);
        for (auto it = first; it != last; ++it)
            if (it->second.key == key) {
                g.by_start.erase(it);
                break;
            }
        where_.erase(found);
        return true;
    }
    // visit(key, base, width) for every range that contains [address, address + bytes) (bytes >= 1); visit returns
    // false to stop. Order: by class, then by start.
    template<class Visit> void containing(std::uint64_t address, std::uint64_t bytes, Visit&& visit) const
    {
        if (!bytes || bytes > ~std::uint64_t{} - address) return;
        for (const Group& g : groups_) {
            if (g.by_start.empty()) continue;
            const std::uint64_t earliest = mutation_ == Mutation::exact_start ? address
                : address >= g.max_width ? address - g.max_width + 1 : 0;
            for (auto it = g.by_start.lower_bound(earliest); it != g.by_start.end() && it->first <= address; ++it) {
                const std::uint64_t base = it->first, width = it->second.width;
                if (address - base >= width || bytes > width - (address - base)) continue;
                if (!visit(it->second.key, base, width)) return;
            }
        }
    }
    std::size_t size() const { return where_.size(); }

private:
    struct Entry {
        Key key;
        std::uint64_t width;
    };
    struct Group {
        std::multimap<std::uint64_t, Entry> by_start;
        std::uint64_t max_width = 0;
    };
    static std::size_t class_of(std::uint64_t width)
    {
        return width <= (std::uint64_t{1} << 16) ? 0 : width <= (std::uint64_t{1} << 20) ? 1 : width <= (std::uint64_t{1} << 24) ? 2
             : width <= (std::uint64_t{1} << 28) ? 3 : 4;
    }
    std::array<Group, 5> groups_;
    std::unordered_map<Key, std::pair<std::uint64_t, std::uint64_t>> where_;
    Mutation mutation_;
};

}  // namespace x2w_startup
