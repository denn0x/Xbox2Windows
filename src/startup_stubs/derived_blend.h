#pragma once

#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x2w_derived_blend {
constexpr std::size_t record_size = sizeof(std::uint32_t) + sizeof(D3D12_BLEND_DESC);
static_assert(record_size == 0x14c);

// Each record starts with its state-part tag. A later blend patch replaces an earlier one.
inline bool last_blend(const unsigned char* records, std::size_t bytes,
                       D3D12_BLEND_DESC& blend, std::size_t& offset)
{
    if (!records || bytes % record_size) return false;
    for (std::size_t end = bytes; end; end -= record_size) {
        const std::size_t at = end - record_size;
        std::uint32_t tag;
        std::memcpy(&tag, records + at, sizeof(tag));
        if (tag != 2) continue;
        offset = at + sizeof(tag);
        std::memcpy(&blend, records + offset, sizeof(blend));
        return true;
    }
    return false;
}
} // namespace x2w_derived_blend
