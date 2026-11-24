// Colour-blend registers -> desktop blend state for one render target (#1216).
//
// CB_BLENDn_CONTROL and the target's CB_TARGET_MASK nibble, with the field layout and enums of
// Mesa's src/amd/registers/gfx10.json (identical in gfx103.json for these registers):
//   COLOR_SRCBLEND [4:0], COLOR_COMB_FCN [7:5], COLOR_DESTBLEND [12:8],
//   ALPHA_SRCBLEND [20:16], ALPHA_COMB_FCN [23:21], ALPHA_DESTBLEND [28:24],
//   SEPARATE_ALPHA_BLEND [29], ENABLE [30], DISABLE_ROP3 [31].
//
// Pure translation; where the registers sit in a captured pipeline state block is a separate step.
// A form D3D12 cannot express is refused with a reason rather than approximated, so the caller can
// keep an opaque pipeline and count the cause.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace x2w_cb_blend {

struct Decoded {
    bool ok = false;
    const char* reason = nullptr;              // why the form was refused (ok == false)
    D3D12_RENDER_TARGET_BLEND_DESC desc{};     // valid only when ok
};

namespace detail {

constexpr std::uint32_t field(std::uint32_t value, unsigned low, unsigned high)
{
    return (value >> low) & ((1u << (high - low + 1)) - 1);
}

// BlendOp enum -> D3D12_BLEND; 0 when D3D12 has no equivalent.
inline D3D12_BLEND factor(std::uint32_t op, bool alpha_factor_supported)
{
    switch (op) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 2: return D3D12_BLEND_SRC_COLOR;
    case 3: return D3D12_BLEND_INV_SRC_COLOR;
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 6: return D3D12_BLEND_DEST_ALPHA;
    case 7: return D3D12_BLEND_INV_DEST_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_SRC_ALPHA_SAT;
    // 11 BOTH_SRC_ALPHA, 12 BOTH_INV_SRC_ALPHA: legacy paired factors, no D3D12 form.
    case 13: return D3D12_BLEND_BLEND_FACTOR;
    case 14: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return D3D12_BLEND_SRC1_COLOR;
    case 16: return D3D12_BLEND_INV_SRC1_COLOR;
    case 17: return D3D12_BLEND_SRC1_ALPHA;
    case 18: return D3D12_BLEND_INV_SRC1_ALPHA;
    case 19: return alpha_factor_supported ? D3D12_BLEND_ALPHA_FACTOR : static_cast<D3D12_BLEND>(0);
    case 20: return alpha_factor_supported ? D3D12_BLEND_INV_ALPHA_FACTOR : static_cast<D3D12_BLEND>(0);
    default: return static_cast<D3D12_BLEND>(0);
    }
}

// On the alpha channel a colour factor reads that factor's alpha component. D3D12 accepts only the
// alpha forms in SrcBlendAlpha / DestBlendAlpha, so name them that way.
inline D3D12_BLEND alpha_form(D3D12_BLEND f)
{
    switch (f) {
    case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
    case D3D12_BLEND_SRC1_COLOR: return D3D12_BLEND_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_ALPHA;
    default: return f;
    }
}

// CombFunc enum -> D3D12_BLEND_OP; 0 when unknown.
inline D3D12_BLEND_OP combine(std::uint32_t fcn)
{
    switch (fcn) {
    case 0: return D3D12_BLEND_OP_ADD;           // COMB_DST_PLUS_SRC
    case 1: return D3D12_BLEND_OP_SUBTRACT;      // COMB_SRC_MINUS_DST
    case 2: return D3D12_BLEND_OP_MIN;           // COMB_MIN_DST_SRC
    case 3: return D3D12_BLEND_OP_MAX;           // COMB_MAX_DST_SRC
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;  // COMB_DST_MINUS_SRC
    default: return static_cast<D3D12_BLEND_OP>(0);
    }
}

inline bool min_max(D3D12_BLEND_OP op) { return op == D3D12_BLEND_OP_MIN || op == D3D12_BLEND_OP_MAX; }

}  // namespace detail

// control: CB_BLENDn_CONTROL. target_mask: the render target's CB_TARGET_MASK nibble, taken as
// R=1, G=2, B=4, A=8 (component order of an unswapped colour format). alpha_factor_supported:
// D3D12_FEATURE_DATA_D3D12_OPTIONS13::AlphaBlendFactorSupported, needed for CONSTANT_ALPHA.
inline Decoded decode(std::uint32_t control, std::uint32_t target_mask, bool alpha_factor_supported)
{
    using namespace detail;
    Decoded out;
    if (target_mask > 0xF) {
        out.reason = "target mask wider than four components";
        return out;
    }
    D3D12_RENDER_TARGET_BLEND_DESC& d = out.desc;
    d.SrcBlend = d.SrcBlendAlpha = D3D12_BLEND_ONE;
    d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_ZERO;
    d.BlendOp = d.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.LogicOp = D3D12_LOGIC_OP_NOOP;
    d.RenderTargetWriteMask = static_cast<UINT8>(target_mask);
    if (!field(control, 30, 30)) {  // ENABLE clear: the target is written unblended
        out.ok = true;
        return out;
    }
    const bool separate = field(control, 29, 29) != 0;
    const std::uint32_t color_src = field(control, 0, 4), color_fcn = field(control, 5, 7), color_dst = field(control, 8, 12);
    const std::uint32_t alpha_src = separate ? field(control, 16, 20) : color_src;
    const std::uint32_t alpha_fcn = separate ? field(control, 21, 23) : color_fcn;
    const std::uint32_t alpha_dst = separate ? field(control, 24, 28) : color_dst;

    const D3D12_BLEND src = factor(color_src, alpha_factor_supported), dst = factor(color_dst, alpha_factor_supported);
    const D3D12_BLEND src_a = factor(alpha_src, alpha_factor_supported), dst_a = factor(alpha_dst, alpha_factor_supported);
    if (!src || !dst || !src_a || !dst_a) {
        const std::uint32_t bad = !src ? color_src : !dst ? color_dst : !src_a ? alpha_src : alpha_dst;
        out.reason = bad == 11 || bad == 12 ? "BOTH_SRC_ALPHA / BOTH_INV_SRC_ALPHA factor"
                   : bad == 19 || bad == 20 ? "CONSTANT_ALPHA factor without alpha blend factor support"
                                            : "unknown blend factor";
        return out;
    }
    const D3D12_BLEND_OP op = combine(color_fcn), op_a = combine(alpha_fcn);
    if (!op || !op_a) {
        out.reason = "unknown combine function";
        return out;
    }
    // D3D12 ignores the factors of MIN and MAX. The hardware is taken to apply them (RADV forces both
    // to ONE for min/max), so only the ONE / ONE form has the same meaning on both sides.
    if ((min_max(op) && (src != D3D12_BLEND_ONE || dst != D3D12_BLEND_ONE)) ||
        (min_max(op_a) && (src_a != D3D12_BLEND_ONE || dst_a != D3D12_BLEND_ONE))) {
        out.reason = "MIN / MAX with factors other than ONE";
        return out;
    }
    d.BlendEnable = TRUE;
    d.SrcBlend = src;
    d.DestBlend = dst;
    d.BlendOp = op;
    d.SrcBlendAlpha = alpha_form(src_a);
    d.DestBlendAlpha = alpha_form(dst_a);
    d.BlendOpAlpha = op_a;
    out.ok = true;
    return out;
}

// Builds a CB_BLENDn_CONTROL word from its fields (tests and diagnostics).
constexpr std::uint32_t control_word(std::uint32_t color_src, std::uint32_t color_fcn, std::uint32_t color_dst,
                                     std::uint32_t alpha_src, std::uint32_t alpha_fcn, std::uint32_t alpha_dst,
                                     bool separate_alpha, bool enable)
{
    return (color_src & 0x1F) | (color_fcn & 7) << 5 | (color_dst & 0x1F) << 8 | (alpha_src & 0x1F) << 16 |
           (alpha_fcn & 7) << 21 | (alpha_dst & 0x1F) << 24 | std::uint32_t{separate_alpha} << 29 |
           std::uint32_t{enable} << 30;
}

}  // namespace x2w_cb_blend
