#pragma once

#include "pipeline_state_decode.h"

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
    // The record walk is shared with the other parts (pipeline_state_decode.h).
    const std::size_t at = x2w_pipeline_state::last_part(records, bytes, x2w_pipeline_state::part_blend);
    if (at == SIZE_MAX) return false;
    offset = at;
    std::memcpy(&blend, records + offset, sizeof(blend));
    return true;
}
} // namespace x2w_derived_blend
