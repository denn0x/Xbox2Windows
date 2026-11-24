// Experimental (docs/startup-runtime.md): the blend state of a pipeline created through device
// slot 57, read from its state block A (#1216).
//
// State block A holds the eight CB_BLENDn_CONTROL words at +0xD0, one per render target (measured
// on #1216). They are decoded with cb_blend.h. Every target keeps all four channels here; the
// write masks (CB_TARGET_MASK & CB_SHADER_MASK) are pipeline_state_decode.h's, behind a switch.
#pragma once

#include "pipeline_state_decode.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x2w_startup {
constexpr std::size_t state_block_blend_offset = 0xD0;

// Fills out from state block A. Returns false, leaving out alone, when the block is too short or
// the decoder refuses the word of any of the eight targets; reason then names the first cause and
// refused_targets has a bit per refused target. Nothing is substituted for a refused word: drawing
// that target unblended would change its output, so the caller keeps the path it had before.
inline bool state_block_blend(const unsigned char* block, std::size_t bytes, bool alpha_factor_supported, D3D12_BLEND_DESC* out,
                              const char** reason = nullptr, unsigned* refused_targets = nullptr)
{
    const char* ignored = nullptr;
    const char*& why = reason ? *reason : ignored;
    why = nullptr;
    if (refused_targets) *refused_targets = 0;
    if (!block || bytes < state_block_blend_offset + 8 * sizeof(std::uint32_t)) {
        why = "state block A shorter than the blend words";
        return false;
    }
    // One decoder for the eight words (pipeline_state_decode.h), with every target writing RGBA.
    std::uint32_t words[8];
    std::memcpy(words, block + state_block_blend_offset, sizeof(words));
    const x2w_pipeline_state::Blend decoded = x2w_pipeline_state::decode_blend(words, 0, 0, false, alpha_factor_supported);
    if (refused_targets) *refused_targets = decoded.refused_targets;
    if (!decoded.ok) {
        why = decoded.reason;
        return false;
    }
    const D3D12_BLEND_DESC& blend = decoded.desc;
    *out = blend;
    return true;
}
}  // namespace x2w_startup
