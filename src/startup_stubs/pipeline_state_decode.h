// Experimental (docs/startup-runtime.md): fixed-function state of a pipeline created through device
// slot 57, decoded from its state block A, plus the desktop-layout parts a slot 55 derivation patches.
//
// State block A is a packed struct of selected register values, one u32 each at fixed offsets (local
// layout notes; mesh pipelines, A+0x004 == 6, carry the same fields 0x54 bytes later from +0x058 on):
//   +0x078 CB_SHADER_MASK, +0x09c PA_CL_CLIP_CNTL, +0x0a0 PA_SU_SC_MODE_CNTL,
//   +0x0ac / +0x0b0 / +0x0b4 three floats taken as PA_SU_POLY_OFFSET_CLAMP / _FRONT_SCALE / _FRONT_OFFSET
//   (the hardware register order; value ranges fit, not measured), +0x0b8 DB_DEPTH_CONTROL,
//   +0x0bc DB_STENCIL_CONTROL, +0x0c0 / +0x0c4 DB_STENCILREFMASK / _BF, +0x0cc CB_TARGET_MASK,
//   +0x0d0 .. +0x0ec CB_BLEND0..7_CONTROL (decoded by cb_blend.h).
// Field layouts and enumerations: Mesa's src/amd/registers/gfx10.json (MIT). Facing and polygon offset
// conventions as Mesa's RADV programs them (src/amd/vulkan/radv_cmd_buffer.c: FACE = VkFrontFace, so
// FACE 0 is counter-clockwise in framebuffer space; POLY_OFFSET_FRONT_SCALE = slope factor * 16) and
// as vkd3d-proton maps D3D12 (FrontCounterClockwise -> VK_FRONT_FACE_COUNTER_CLOCKWISE).
//
// Pure functions; a form D3D12 cannot express is refused with a reason (or, where noted, approximated
// and flagged) so the caller can keep its previous state and count the cause.
#pragma once

#include "cb_blend.h"

#include <d3d12.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x2w_pipeline_state {

// ---- state block A offsets (vertex / keyed pipelines; mesh_shift added from +0x058 on) ----
constexpr std::size_t a_shader_mask = 0x078, a_clip = 0x09c, a_mode = 0x0a0, a_bias = 0x0ac, a_depth = 0x0b8,
                      a_stencil = 0x0bc, a_refmask = 0x0c0, a_refmask_bf = 0x0c4, a_target_mask = 0x0cc, a_blend = 0x0d0;
constexpr std::size_t mesh_shift = 0x54;

constexpr std::uint32_t bits(std::uint32_t v, unsigned lo, unsigned hi) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

// The register words of one state block A.
struct Words {
    std::uint32_t shader_mask = 0, clip = 0, mode = 0, depth = 0, stencil = 0, refmask = 0, refmask_bf = 0, target_mask = 0;
    std::uint32_t blend[8] = {};
    float bias_clamp = 0, bias_scale = 0, bias_offset = 0;
    bool mesh = false;
};

inline bool read_words(const unsigned char* a, std::size_t bytes, Words& w)
{
    if (!a || bytes < 8) return false;
    std::uint32_t kind;
    std::memcpy(&kind, a + 4, 4);
    w.mesh = kind == 6;
    const std::size_t s = w.mesh ? mesh_shift : 0;
    if (bytes < a_blend + s + 32) return false;
    auto u = [&](std::size_t off) { std::uint32_t v; std::memcpy(&v, a + off + s, 4); return v; };
    auto f = [&](std::size_t off) { float v; std::memcpy(&v, a + off + s, 4); return v; };
    w.shader_mask = u(a_shader_mask);
    w.clip = u(a_clip);
    w.mode = u(a_mode);
    w.bias_clamp = f(a_bias);
    w.bias_scale = f(a_bias + 4);
    w.bias_offset = f(a_bias + 8);
    w.depth = u(a_depth);
    w.stencil = u(a_stencil);
    w.refmask = u(a_refmask);
    w.refmask_bf = u(a_refmask_bf);
    w.target_mask = u(a_target_mask);
    for (unsigned i = 0; i < 8; ++i) w.blend[i] = u(a_blend + 4 * i);
    return true;
}

// ---- depth / stencil: DB_DEPTH_CONTROL, DB_STENCIL_CONTROL, DB_STENCILREFMASK(_BF) ----
struct DepthStencil {
    bool ok = false;
    const char* reason = nullptr;
    D3D12_DEPTH_STENCIL_DESC desc{};
    bool depth_bounds = false;      // DEPTH_BOUNDS_ENABLE: not applied (values are dynamic); flagged only
    bool masks_differ = false;      // back-face read/write masks differ from the front: front used, flagged
};

// CompareFrag (FRAG_NEVER .. FRAG_ALWAYS) has the order of D3D12_COMPARISON_FUNC (NEVER = 1 .. ALWAYS = 8).
constexpr D3D12_COMPARISON_FUNC compare(std::uint32_t frag) { return static_cast<D3D12_COMPARISON_FUNC>((frag & 7) + 1); }

// StencilOp -> D3D12_STENCIL_OP; 0 when D3D12 has no equivalent. opval: STENCILOPVAL, the amount the
// ADD / SUB ops apply (D3D12's INCR / DECR forms add one).
inline D3D12_STENCIL_OP stencil_op(std::uint32_t op, std::uint32_t opval)
{
    switch (op) {
    case 0: return D3D12_STENCIL_OP_KEEP;
    case 1: return D3D12_STENCIL_OP_ZERO;
    case 3: return D3D12_STENCIL_OP_REPLACE;               // STENCIL_REPLACE_TEST: the test (reference) value
    case 5: return opval == 1 ? D3D12_STENCIL_OP_INCR_SAT : static_cast<D3D12_STENCIL_OP>(0);  // ADD_CLAMP
    case 6: return opval == 1 ? D3D12_STENCIL_OP_DECR_SAT : static_cast<D3D12_STENCIL_OP>(0);  // SUB_CLAMP
    case 7: return D3D12_STENCIL_OP_INVERT;
    case 8: return opval == 1 ? D3D12_STENCIL_OP_INCR : static_cast<D3D12_STENCIL_OP>(0);      // ADD_WRAP
    case 9: return opval == 1 ? D3D12_STENCIL_OP_DECR : static_cast<D3D12_STENCIL_OP>(0);      // SUB_WRAP
    default: return static_cast<D3D12_STENCIL_OP>(0);      // ONES, REPLACE_OP, AND .. XNOR
    }
}

inline DepthStencil decode_depth_stencil(std::uint32_t depth_control, std::uint32_t stencil_control, std::uint32_t refmask,
                                         std::uint32_t refmask_bf)
{
    DepthStencil out;
    D3D12_DEPTH_STENCIL_DESC& d = out.desc;
    if (bits(depth_control, 30, 31)) {
        out.reason = "colour writes tied to the depth result (DB_DEPTH_CONTROL bits 30-31)";
        return out;
    }
    const bool z = bits(depth_control, 1, 1) != 0;
    d.DepthEnable = z;
    d.DepthWriteMask = z && bits(depth_control, 2, 2) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = compare(bits(depth_control, 4, 6));
    out.depth_bounds = bits(depth_control, 3, 3) != 0;
    d.StencilEnable = bits(depth_control, 0, 0) != 0;
    const bool back_own = bits(depth_control, 7, 7) != 0;  // BACKFACE_ENABLE: else back faces use the front state
    d.StencilReadMask = static_cast<UINT8>(bits(refmask, 8, 15));
    d.StencilWriteMask = static_cast<UINT8>(bits(refmask, 16, 23));
    const std::uint32_t opval = bits(refmask, 24, 31), opval_bf = back_own ? bits(refmask_bf, 24, 31) : opval;
    auto face = [&](D3D12_DEPTH_STENCILOP_DESC& f, unsigned lo, std::uint32_t func, std::uint32_t val) {
        f.StencilFailOp = stencil_op(bits(stencil_control, lo, lo + 3), val);
        f.StencilPassOp = stencil_op(bits(stencil_control, lo + 4, lo + 7), val);
        f.StencilDepthFailOp = stencil_op(bits(stencil_control, lo + 8, lo + 11), val);
        f.StencilFunc = compare(func);
        return f.StencilFailOp && f.StencilPassOp && f.StencilDepthFailOp;
    };
    const bool front_ok = face(d.FrontFace, 0, bits(depth_control, 8, 10), opval);
    const bool back_ok = back_own ? face(d.BackFace, 12, bits(depth_control, 20, 22), opval_bf) : (d.BackFace = d.FrontFace, front_ok);
    if (d.StencilEnable && (!front_ok || !back_ok)) {
        out.reason = "stencil op without a D3D12 form (ONES, REPLACE_OP, logic op, or add / subtract by other than one)";
        return out;
    }
    if (!d.StencilEnable) {  // ignored by D3D12, but keep the description canonical (one PSO per real state)
        d.StencilReadMask = d.StencilWriteMask = 0xFF;
        d.FrontFace = d.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    } else if (back_own && bits(refmask_bf, 8, 23) != bits(refmask, 8, 23)) {
        out.masks_differ = true;
    }
    if (!z) d.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;  // canonical: the function of a disabled test is ignored
    out.ok = true;
    return out;
}

// ---- rasterizer: PA_SU_SC_MODE_CNTL, PA_CL_CLIP_CNTL, polygon offset ----
struct Raster {
    bool ok = false;
    const char* reason = nullptr;
    D3D12_RASTERIZER_DESC desc{};
    bool clip_split = false;        // only one of ZCLIP_NEAR/FAR_DISABLE: D3D12 has one switch; clipping kept
    bool bias_rounded = false;      // FRONT_OFFSET not an integer: rounded to DepthBias
};

// flip_winding: run-time switch for the one convention that was not observed on a frame (FACE 0 is
// taken as FrontCounterClockwise = TRUE; flip inverts that).
inline Raster decode_rasterizer(std::uint32_t mode, std::uint32_t clip, float bias_clamp, float bias_scale, float bias_offset,
                                bool flip_winding)
{
    Raster out;
    D3D12_RASTERIZER_DESC& r = out.desc;
    if (bits(mode, 3, 4)) {  // POLY_MODE dual: per-face point / line / triangle drawing
        const std::uint32_t front = bits(mode, 5, 7), back = bits(mode, 8, 10);
        if (front != back || front == 0) {
            out.reason = "polygon mode with points or with different front and back modes";
            return out;
        }
        r.FillMode = front == 1 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    } else {
        r.FillMode = D3D12_FILL_MODE_SOLID;
    }
    const bool cull_front = bits(mode, 0, 0) != 0, cull_back = bits(mode, 1, 1) != 0;
    if (cull_front && cull_back) {
        out.reason = "both faces culled";
        return out;
    }
    r.CullMode = cull_front ? D3D12_CULL_MODE_FRONT : cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    r.FrontCounterClockwise = (bits(mode, 2, 2) == 0) != flip_winding;
    const bool offset_front = bits(mode, 11, 11) != 0, offset_back = bits(mode, 12, 12) != 0;
    if (offset_front != offset_back) {
        out.reason = "polygon offset on one face only";
        return out;
    }
    if (offset_front) {
        if (!std::isfinite(bias_clamp) || !std::isfinite(bias_scale) || !std::isfinite(bias_offset) || std::fabs(bias_offset) > 1e6f) {
            out.reason = "polygon offset values not finite";
            return out;
        }
        const float rounded = std::nearbyint(bias_offset);
        out.bias_rounded = rounded != bias_offset;
        r.DepthBias = static_cast<INT>(rounded);
        r.DepthBiasClamp = bias_clamp;
        r.SlopeScaledDepthBias = bias_scale / 16.0f;
    }
    const bool near_off = bits(clip, 26, 26) != 0, far_off = bits(clip, 27, 27) != 0;
    out.clip_split = near_off != far_off;
    r.DepthClipEnable = !(near_off && far_off);
    r.MultisampleEnable = FALSE;
    r.AntialiasedLineEnable = FALSE;
    r.ForcedSampleCount = 0;
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    out.ok = true;
    return out;
}

// ---- blend: CB_BLENDn_CONTROL (cb_blend.h) with CB_TARGET_MASK & CB_SHADER_MASK per target ----
struct Blend {
    bool ok = false;
    const char* reason = nullptr;
    unsigned refused_targets = 0;
    D3D12_BLEND_DESC desc{};
    bool dual_source = false;       // an enabled target uses a SRC1 factor: the pixel shader must export a second colour
    unsigned masked_targets = 0;    // bit per target whose write mask is narrower than RGBA
};

inline bool uses_src1(D3D12_BLEND f)
{
    return f == D3D12_BLEND_SRC1_COLOR || f == D3D12_BLEND_INV_SRC1_COLOR || f == D3D12_BLEND_SRC1_ALPHA || f == D3D12_BLEND_INV_SRC1_ALPHA;
}

// apply_masks false reproduces state_block_blend.h (every target writes RGBA).
inline Blend decode_blend(const std::uint32_t control[8], std::uint32_t target_mask, std::uint32_t shader_mask, bool apply_masks,
                          bool alpha_factor_supported)
{
    Blend out;
    out.desc.IndependentBlendEnable = TRUE;
    for (unsigned t = 0; t < 8; ++t) {
        const std::uint32_t mask = apply_masks ? ((target_mask & shader_mask) >> (4 * t)) & 0xF : 0xF;
        const x2w_cb_blend::Decoded d = x2w_cb_blend::decode(control[t], mask, alpha_factor_supported);
        if (!d.ok) {
            if (!out.refused_targets) out.reason = d.reason;
            out.refused_targets |= 1u << t;
            continue;
        }
        out.desc.RenderTarget[t] = d.desc;
        if (mask != 0xF) out.masked_targets |= 1u << t;
        const auto& r = d.desc;
        if (r.BlendEnable && (uses_src1(r.SrcBlend) || uses_src1(r.DestBlend) || uses_src1(r.SrcBlendAlpha) || uses_src1(r.DestBlendAlpha)))
            out.dual_source = true;
    }
    out.ok = !out.refused_targets;
    return out;
}

// ---- slot 55 derived records: {u32 part; desktop desc} of record_size bytes; the last of a part wins ----
constexpr std::size_t record_size = sizeof(std::uint32_t) + sizeof(D3D12_BLEND_DESC);
static_assert(record_size == 0x14c, "derived record size");
enum Part : std::uint32_t { part_depth_stencil = 0, part_rasterizer = 1, part_blend = 2 };

// Returns the byte offset of the last record's desc with the given part, or SIZE_MAX.
inline std::size_t last_part(const unsigned char* records, std::size_t bytes, std::uint32_t part)
{
    if (!records || !bytes || bytes % record_size) return SIZE_MAX;
    for (std::size_t end = bytes; end; end -= record_size) {
        std::uint32_t tag;
        std::memcpy(&tag, records + end - record_size, sizeof(tag));
        if (tag == part) return end - record_size + sizeof(tag);
    }
    return SIZE_MAX;
}

inline bool valid_stencil_face(const D3D12_DEPTH_STENCILOP_DESC& f)
{
    auto op = [](D3D12_STENCIL_OP o) { return o >= D3D12_STENCIL_OP_KEEP && o <= D3D12_STENCIL_OP_DECR; };
    return op(f.StencilFailOp) && op(f.StencilDepthFailOp) && op(f.StencilPassOp) && f.StencilFunc >= D3D12_COMPARISON_FUNC_NEVER &&
           f.StencilFunc <= D3D12_COMPARISON_FUNC_ALWAYS;
}

// A derived depth-stencil part, read field by field (the title leaves garbage in padding); false when a
// field is outside its enumeration.
inline bool derived_depth_stencil(const unsigned char* records, std::size_t bytes, D3D12_DEPTH_STENCIL_DESC& out)
{
    const std::size_t at = last_part(records, bytes, part_depth_stencil);
    if (at == SIZE_MAX) return false;
    D3D12_DEPTH_STENCIL_DESC d;
    std::memcpy(&d, records + at, sizeof(d));
    if (static_cast<std::uint32_t>(d.DepthWriteMask) > 1 || d.DepthFunc < D3D12_COMPARISON_FUNC_NEVER || d.DepthFunc > D3D12_COMPARISON_FUNC_ALWAYS ||
        !valid_stencil_face(d.FrontFace) || !valid_stencil_face(d.BackFace))
        return false;
    D3D12_DEPTH_STENCIL_DESC clean{};  // field by field: the padding after StencilWriteMask holds garbage
    clean.DepthEnable = d.DepthEnable != 0;
    clean.DepthWriteMask = d.DepthWriteMask;
    clean.DepthFunc = d.DepthFunc;
    clean.StencilEnable = d.StencilEnable != 0;
    clean.StencilReadMask = d.StencilReadMask;
    clean.StencilWriteMask = d.StencilWriteMask;
    clean.FrontFace = d.FrontFace;
    clean.BackFace = d.BackFace;
    out = clean;
    return true;
}

inline bool derived_rasterizer(const unsigned char* records, std::size_t bytes, D3D12_RASTERIZER_DESC& out)
{
    const std::size_t at = last_part(records, bytes, part_rasterizer);
    if (at == SIZE_MAX) return false;
    D3D12_RASTERIZER_DESC r;
    std::memcpy(&r, records + at, sizeof(r));
    if (r.FillMode < D3D12_FILL_MODE_WIREFRAME || r.FillMode > D3D12_FILL_MODE_SOLID || r.CullMode < D3D12_CULL_MODE_NONE ||
        r.CullMode > D3D12_CULL_MODE_BACK || !std::isfinite(r.DepthBiasClamp) || !std::isfinite(r.SlopeScaledDepthBias) ||
        r.ForcedSampleCount > 16 || static_cast<std::uint32_t>(r.ConservativeRaster) > 1)
        return false;
    r.FrontCounterClockwise = r.FrontCounterClockwise != 0;
    r.DepthClipEnable = r.DepthClipEnable != 0;
    r.MultisampleEnable = r.MultisampleEnable != 0;
    r.AntialiasedLineEnable = r.AntialiasedLineEnable != 0;
    out = r;
    return true;
}

// ---- a decoded depth-stencil state against the depth target the draw has bound ----
enum TargetAdjust : unsigned {
    adjust_no_target = 1,       // depth or stencil test with no depth target bound: both turned off
    adjust_no_stencil = 2,      // stencil test on a format without a stencil plane: stencil turned off
    adjust_read_only = 4,       // read-only depth target: depth and stencil writes turned off
};

inline unsigned adjust_for_target(D3D12_DEPTH_STENCIL_DESC& d, bool has_target, bool stencil_plane, bool read_only)
{
    unsigned done = 0;
    if (!has_target) {
        if (d.DepthEnable || d.StencilEnable) done |= adjust_no_target;
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.StencilEnable = FALSE;
        return done;
    }
    if (d.StencilEnable && !stencil_plane) {
        d.StencilEnable = FALSE;
        done |= adjust_no_stencil;
    }
    if (read_only && ((d.DepthEnable && d.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO) || (d.StencilEnable && d.StencilWriteMask))) {
        d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.StencilWriteMask = 0;
        done |= adjust_read_only;
    }
    return done;
}

// ---- run-time switches ----
// Every group defaults off, which keeps the state the draw path had before: the blend words of
// state block A (state_block_blend.h) with every target RGBA, no depth, no culling.
// X2W_DRAW_STATE_DEPTH=1: depth / stencil from A and slot 55 depth-stencil parts, stencil reference forwarded.
// X2W_DRAW_STATE_CULL=1: rasterizer (cull, front face, polygon offset, depth clip) from A and slot 55
//   rasterizer parts; X2W_DRAW_STATE_CULL_FLIP=1 inverts the front-face convention.
// X2W_DRAW_STATE_BLEND=1: CB_TARGET_MASK & CB_SHADER_MASK as write masks, dual-source blend on target 0.
// X2W_DRAW_STATE_DEPTH_CLEAR=1: list slot 51 (ClearDepthStencilView) forwarded instead of dropped.
// X2W_DRAW_STATE=0 turns all of them off whatever the others say.
struct Switches {
    bool depth = false, cull = false, flip = false, blend = false, depth_clear = false;
};

inline bool is_one(const char* v) { return v && v[0] == '1' && v[1] == 0; }
inline bool is_zero(const char* v) { return v && v[0] == '0' && v[1] == 0; }

inline Switches parse_switches(const char* all, const char* depth, const char* cull, const char* blend, const char* flip,
                               const char* depth_clear = nullptr)
{
    Switches s;
    if (is_zero(all)) return s;
    s.depth = is_one(depth);
    s.cull = is_one(cull);
    s.flip = s.cull && is_one(flip);
    s.blend = is_one(blend);
    s.depth_clear = is_one(depth_clear);
    return s;
}

#ifdef _WIN32
inline Switches switches_from_env()
{
    constexpr int n = 6;
    char v[n][8] = {};
    const char* names[n] = {"X2W_DRAW_STATE",       "X2W_DRAW_STATE_DEPTH",     "X2W_DRAW_STATE_CULL",
                            "X2W_DRAW_STATE_BLEND", "X2W_DRAW_STATE_CULL_FLIP", "X2W_DRAW_STATE_DEPTH_CLEAR"};
    const char* p[n] = {};
    for (int i = 0; i < n; ++i)
        if (GetEnvironmentVariableA(names[i], v[i], sizeof(v[i])) && v[i][0]) p[i] = v[i];
    return parse_switches(p[0], p[1], p[2], p[3], p[4], p[5]);
}
#endif

}  // namespace x2w_pipeline_state
