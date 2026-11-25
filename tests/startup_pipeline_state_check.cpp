// Checks src/startup_stubs/pipeline_state_decode.h without a device: hand-built register words (field
// layouts of Mesa's gfx10.json) decoded to D3D12 depth-stencil, rasterizer and blend descriptions,
// the refusals, synthetic state blocks in both layouts, slot 55 records, the depth-target adjustments
// and the switches. No title data.
//
// --mutate winding: expects FACE 0 to mean clockwise front faces (the check must then fail).
// --mutate mesh-shift: expects mesh-layout fields at the vertex-layout offsets (must fail).
#include "startup_stubs/pipeline_state_decode.h"
#include "startup_stubs/state_block_blend.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace x2w_pipeline_state;
int failures, cases;
bool mutate_winding, mutate_shift;

void check(bool ok, const char* name)
{
    ++cases;
    if (!ok) {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

// DB_DEPTH_CONTROL from its fields.
constexpr std::uint32_t depth_control(bool stencil, bool z, bool z_write, bool bounds, unsigned zfunc, bool backface, unsigned sfunc,
                                      unsigned sfunc_bf)
{
    return std::uint32_t{stencil} | std::uint32_t{z} << 1 | std::uint32_t{z_write} << 2 | std::uint32_t{bounds} << 3 | (zfunc & 7) << 4 |
           std::uint32_t{backface} << 7 | (sfunc & 7) << 8 | (sfunc_bf & 7) << 20;
}
constexpr std::uint32_t stencil_control(unsigned fail, unsigned zpass, unsigned zfail, unsigned fail_bf, unsigned zpass_bf, unsigned zfail_bf)
{
    return fail | zpass << 4 | zfail << 8 | fail_bf << 12 | zpass_bf << 16 | zfail_bf << 20;
}
constexpr std::uint32_t refmask(unsigned test, unsigned mask, unsigned write, unsigned opval) { return test | mask << 8 | write << 16 | opval << 24; }
constexpr std::uint32_t mode_cntl(bool cull_front, bool cull_back, bool face_cw, unsigned poly_mode, unsigned front_type, unsigned back_type,
                                  bool offset_front, bool offset_back)
{
    return std::uint32_t{cull_front} | std::uint32_t{cull_back} << 1 | std::uint32_t{face_cw} << 2 | poly_mode << 3 | front_type << 5 |
           back_type << 8 | std::uint32_t{offset_front} << 11 | std::uint32_t{offset_back} << 12 | 1u << 21;  // MULTI_PRIM_IB_ENA
}

void depth_stencil_cases()
{
    // Depth off: the function the word carries is ignored (canonical ALWAYS), stencil off.
    auto d = decode_depth_stencil(depth_control(false, false, false, false, 1, true, 7, 7), 0, refmask(0, 0xff, 0, 1), refmask(0, 0xff, 0, 1));
    check(d.ok && !d.desc.DepthEnable && d.desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO && d.desc.DepthFunc == D3D12_COMPARISON_FUNC_ALWAYS &&
              !d.desc.StencilEnable, "depth off");
    // Reversed depth: GEQUAL read-only.
    d = decode_depth_stencil(depth_control(false, true, false, false, 6, true, 7, 7), 0, refmask(0, 0xff, 0, 1), refmask(0, 0xff, 0, 1));
    check(d.ok && d.desc.DepthEnable && d.desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO && d.desc.DepthFunc == D3D12_COMPARISON_FUNC_GREATER_EQUAL,
          "GEQUAL read");
    // Every compare function in order.
    const D3D12_COMPARISON_FUNC funcs[8] = {D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
                                            D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL,
                                            D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
    for (unsigned f = 0; f < 8; ++f) {
        d = decode_depth_stencil(depth_control(true, true, true, false, f, true, f, 7 - f), 0, refmask(0, 0xff, 0xff, 1), refmask(0, 0xff, 0xff, 1));
        check(d.ok && d.desc.DepthFunc == funcs[f] && d.desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL && d.desc.FrontFace.StencilFunc == funcs[f] &&
                  d.desc.BackFace.StencilFunc == funcs[7 - f], "compare function order");
    }
    // Stencil: replace on depth pass, both faces, masks.
    d = decode_depth_stencil(depth_control(true, true, true, false, 6, true, 7, 7), stencil_control(0, 3, 0, 0, 3, 0), refmask(0, 0xff, 0xff, 1),
                             refmask(0, 0xff, 0xff, 1));
    check(d.ok && d.desc.StencilEnable && d.desc.StencilReadMask == 0xff && d.desc.StencilWriteMask == 0xff &&
              d.desc.FrontFace.StencilPassOp == D3D12_STENCIL_OP_REPLACE && d.desc.BackFace.StencilPassOp == D3D12_STENCIL_OP_REPLACE &&
              d.desc.FrontFace.StencilFailOp == D3D12_STENCIL_OP_KEEP && d.desc.FrontFace.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP,
          "stencil replace");
    // Every op with a D3D12 form, front fail / back depth-fail; the add / subtract ops by one.
    const struct { unsigned op; D3D12_STENCIL_OP d3d; } ops[] = {{0, D3D12_STENCIL_OP_KEEP},     {1, D3D12_STENCIL_OP_ZERO},
                                                                 {3, D3D12_STENCIL_OP_REPLACE},  {5, D3D12_STENCIL_OP_INCR_SAT},
                                                                 {6, D3D12_STENCIL_OP_DECR_SAT}, {7, D3D12_STENCIL_OP_INVERT},
                                                                 {8, D3D12_STENCIL_OP_INCR},     {9, D3D12_STENCIL_OP_DECR}};
    for (const auto& o : ops) {
        d = decode_depth_stencil(depth_control(true, false, false, false, 7, true, 2, 2), stencil_control(o.op, 0, 0, 0, 0, o.op),
                                 refmask(0, 0x28, 0, 1), refmask(0, 0x28, 0, 1));
        check(d.ok && d.desc.FrontFace.StencilFailOp == o.d3d && d.desc.BackFace.StencilDepthFailOp == o.d3d && d.desc.StencilReadMask == 0x28 &&
                  d.desc.StencilWriteMask == 0, "stencil op mapping");
    }
    // No back-face state: back faces use the front state (the _BF words are ignored).
    d = decode_depth_stencil(depth_control(true, false, false, false, 7, false, 3, 0), stencil_control(0, 8, 0, 9, 9, 9), refmask(0, 0x80, 0x80, 1),
                             refmask(0, 0, 0, 7));
    check(d.ok && d.desc.BackFace.StencilFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL && d.desc.BackFace.StencilPassOp == D3D12_STENCIL_OP_INCR &&
              !d.masks_differ, "back faces follow the front");
    // Negative: ops without a form, add by two, colour writes on depth fail.
    check(!decode_depth_stencil(depth_control(true, false, false, false, 7, true, 7, 7), stencil_control(2, 0, 0, 0, 0, 0), refmask(0, 0xff, 0xff, 1),
                                refmask(0, 0xff, 0xff, 1)).ok, "ONES refused");
    check(!decode_depth_stencil(depth_control(true, false, false, false, 7, true, 7, 7), stencil_control(0, 4, 0, 0, 4, 0), refmask(0, 0xff, 0xff, 1),
                                refmask(0, 0xff, 0xff, 1)).ok, "REPLACE_OP refused");
    check(!decode_depth_stencil(depth_control(true, false, false, false, 7, true, 7, 7), stencil_control(0, 8, 0, 0, 8, 0), refmask(0, 0xff, 0xff, 2),
                                refmask(0, 0xff, 0xff, 2)).ok, "add by two refused");
    check(!decode_depth_stencil(depth_control(true, false, false, false, 7, true, 7, 7), stencil_control(0, 0, 0, 0, 12, 0), refmask(0, 0xff, 0xff, 1),
                                refmask(0, 0xff, 0xff, 1)).ok, "back-face XOR refused");
    check(!decode_depth_stencil(depth_control(false, true, true, false, 6, false, 7, 7) | 1u << 30, 0, 0, 0).ok, "colour writes on depth fail refused");
    // An unusable op on a disabled stencil does not refuse the depth state.
    check(decode_depth_stencil(depth_control(false, true, true, false, 1, true, 7, 7), stencil_control(2, 2, 2, 2, 2, 2), 0, 0).ok,
          "ops of a disabled stencil ignored");
    // Flags.
    d = decode_depth_stencil(depth_control(false, true, false, true, 3, false, 7, 7), 0, 0, 0);
    check(d.ok && d.depth_bounds, "depth bounds flagged");
    d = decode_depth_stencil(depth_control(true, false, false, false, 7, true, 7, 7), 0, refmask(0, 0xff, 0xff, 1), refmask(0, 0x0f, 0xff, 1));
    check(d.ok && d.masks_differ && d.desc.StencilReadMask == 0xff, "back masks differ flagged, front kept");
}

void rasterizer_cases()
{
    const bool ccw_for_face0 = !mutate_winding;
    auto r = decode_rasterizer(mode_cntl(false, false, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, false);
    check(r.ok && r.desc.CullMode == D3D12_CULL_MODE_NONE && (r.desc.FrontCounterClockwise != 0) == ccw_for_face0 && r.desc.DepthClipEnable &&
              r.desc.FillMode == D3D12_FILL_MODE_SOLID && r.desc.DepthBias == 0 && r.desc.SlopeScaledDepthBias == 0, "no cull, FACE 0");
    r = decode_rasterizer(mode_cntl(false, true, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, false);
    check(r.ok && r.desc.CullMode == D3D12_CULL_MODE_BACK, "cull back");
    r = decode_rasterizer(mode_cntl(true, false, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, false);
    check(r.ok && r.desc.CullMode == D3D12_CULL_MODE_FRONT, "cull front");
    r = decode_rasterizer(mode_cntl(false, true, true, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, false);
    check(r.ok && (r.desc.FrontCounterClockwise != 0) == !ccw_for_face0, "FACE 1");
    r = decode_rasterizer(mode_cntl(false, true, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, true);
    check(r.ok && (r.desc.FrontCounterClockwise != 0) == !ccw_for_face0, "flip switch");
    // Polygon offset: clamp, scale (slope * 16), offset in the hardware register order.
    r = decode_rasterizer(mode_cntl(false, true, false, 0, 2, 2, true, true), 0x00080000, 0.003f, 29.0f, 1.0f, false);
    check(r.ok && r.desc.DepthBias == 1 && r.desc.DepthBiasClamp == 0.003f && r.desc.SlopeScaledDepthBias == 29.0f / 16.0f && !r.bias_rounded,
          "polygon offset");
    r = decode_rasterizer(mode_cntl(false, true, false, 0, 2, 2, false, false), 0x00080000, 0.003f, 29.0f, 1.0f, false);
    check(r.ok && r.desc.DepthBias == 0 && r.desc.SlopeScaledDepthBias == 0 && r.desc.DepthBiasClamp == 0, "offset disabled ignores values");
    r = decode_rasterizer(mode_cntl(false, false, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 2.5f, false);
    check(r.ok && r.bias_rounded, "fractional offset flagged");
    // Depth clip.
    r = decode_rasterizer(mode_cntl(false, false, false, 0, 2, 2, true, true), 0x0c080000, 0, 0, 0, false);
    check(r.ok && !r.desc.DepthClipEnable && !r.clip_split, "zclip disabled");
    r = decode_rasterizer(mode_cntl(false, false, false, 0, 2, 2, true, true), 0x04080000, 0, 0, 0, false);
    check(r.ok && r.desc.DepthClipEnable && r.clip_split, "one zclip plane flagged, clipping kept");
    // Polygon modes.
    r = decode_rasterizer(mode_cntl(false, false, false, 1, 1, 1, true, true), 0x00080000, 0, 0, 0, false);
    check(r.ok && r.desc.FillMode == D3D12_FILL_MODE_WIREFRAME, "line mode");
    check(!decode_rasterizer(mode_cntl(false, false, false, 1, 0, 0, true, true), 0x00080000, 0, 0, 0, false).ok, "point mode refused");
    check(!decode_rasterizer(mode_cntl(false, false, false, 1, 1, 2, true, true), 0x00080000, 0, 0, 0, false).ok, "mixed modes refused");
    check(!decode_rasterizer(mode_cntl(true, true, false, 0, 2, 2, true, true), 0x00080000, 0, 0, 0, false).ok, "both faces culled refused");
    check(!decode_rasterizer(mode_cntl(false, false, false, 0, 2, 2, true, false), 0x00080000, 0, 0, 0, false).ok, "one-face offset refused");
}

void blend_cases()
{
    using x2w_cb_blend::control_word;
    const std::uint32_t straight = control_word(4, 0, 5, 1, 0, 5, true, true), off = 0x80000000;
    const std::uint32_t dual = control_word(1, 0, 15, 1, 0, 17, true, true);
    std::uint32_t words[8] = {straight, off, off, off, off, off, off, off};
    // Masks: CB_TARGET_MASK & CB_SHADER_MASK per target nibble.
    auto b = decode_blend(words, 0x77777777, 0xff, true, false);
    check(b.ok && b.desc.RenderTarget[0].RenderTargetWriteMask == 7 && b.desc.RenderTarget[1].RenderTargetWriteMask == 7 &&
              b.desc.RenderTarget[2].RenderTargetWriteMask == 0 && b.desc.RenderTarget[0].BlendEnable && !b.desc.RenderTarget[1].BlendEnable &&
              b.masked_targets == 0xff && !b.dual_source, "write masks");
    b = decode_blend(words, 0xffffffff, 0xf, true, false);
    check(b.ok && b.desc.RenderTarget[0].RenderTargetWriteMask == 0xf && b.desc.RenderTarget[1].RenderTargetWriteMask == 0 && b.masked_targets == 0xfe,
          "shader mask limits the targets");
    b = decode_blend(words, 0, 0xf, false, false);
    check(b.ok && b.desc.RenderTarget[3].RenderTargetWriteMask == 0xf && !b.masked_targets, "masks not applied");
    for (auto& w : words) w = dual;
    b = decode_blend(words, 0xf, 0xff, true, false);
    check(b.ok && b.dual_source && b.desc.RenderTarget[0].DestBlend == D3D12_BLEND_SRC1_COLOR && b.desc.RenderTarget[0].DestBlendAlpha == D3D12_BLEND_SRC1_ALPHA,
          "dual source");
    words[3] = control_word(11, 0, 1, 1, 0, 0, false, true);  // BOTH_SRC_ALPHA
    b = decode_blend(words, 0xffffffff, 0xffffffff, true, false);
    check(!b.ok && b.refused_targets == 0x8 && b.reason, "refused target");
    // The unmasked form is what state_block_blend.h answered before: compare on a synthetic block.
    std::vector<unsigned char> block(388, 0xA5);
    const std::uint32_t mix[8] = {straight, off, control_word(1, 0, 1, 1, 0, 1, false, true), control_word(1, 3, 1, 1, 2, 1, true, true), off, off, straight, off};
    std::memcpy(block.data() + x2w_startup::state_block_blend_offset, mix, sizeof(mix));
    D3D12_BLEND_DESC old{};
    const bool old_ok = x2w_startup::state_block_blend(block.data(), block.size(), false, &old);
    bool same = old_ok && old.IndependentBlendEnable && !old.AlphaToCoverageEnable;
    for (unsigned t = 0; t < 8 && same; ++t) {
        const auto ref = x2w_cb_blend::decode(mix[t], 0xF, false);
        same = ref.ok && std::memcmp(&ref.desc, &old.RenderTarget[t], sizeof(ref.desc)) == 0;
    }
    check(same, "state_block_blend equals the per-target decoder");
}

void block_cases()
{
    // Vertex layout (+0x004 = 4) and mesh layout (+0x004 = 6, fields 0x54 later from +0x058).
    for (int mesh = 0; mesh < 2; ++mesh) {
        std::vector<unsigned char> a(mesh ? 0x1d0 : 0x184, 0x5a);
        const std::uint32_t kind = mesh ? 6 : 4;
        std::memcpy(a.data() + 4, &kind, 4);
        const std::size_t s = mesh && !mutate_shift ? mesh_shift : 0;
        auto put = [&](std::size_t off, std::uint32_t v) { std::memcpy(a.data() + off + s, &v, 4); };
        const float bias[3] = {0.5f, 32.0f, 3.0f};
        put(a_shader_mask, 0xff);
        put(a_clip, 0x0c080000);
        put(a_mode, 0x00201a42);
        std::memcpy(a.data() + a_bias + s, bias, sizeof(bias));
        put(a_depth, 0x007007e7);
        put(a_stencil, 0x00030030);
        put(a_refmask, 0x0100ff00);
        put(a_refmask_bf, 0x0100fe00);
        put(a_target_mask, 0x77);
        for (unsigned t = 0; t < 8; ++t) put(a_blend + 4 * t, 0x80000000u + t);
        Words w;
        const bool ok = read_words(a.data(), a.size(), w);
        check(ok && w.mesh == (mesh != 0) && w.shader_mask == 0xff && w.clip == 0x0c080000 && w.mode == 0x00201a42 && w.depth == 0x007007e7 &&
                  w.stencil == 0x00030030 && w.refmask == 0x0100ff00 && w.refmask_bf == 0x0100fe00 && w.target_mask == 0x77 &&
                  w.blend[7] == 0x80000007 && w.bias_clamp == 0.5f && w.bias_scale == 32.0f && w.bias_offset == 3.0f,
              mesh ? "mesh layout words" : "vertex layout words");
        Words short_words;
        check(!read_words(a.data(), (mesh ? mesh_shift : 0) + a_blend + 31, short_words), "short block refused");
    }
}

void add_record(std::vector<unsigned char>& list, std::uint32_t part, const void* desc, std::size_t bytes, unsigned char fill)
{
    const std::size_t at = list.size();
    list.resize(at + record_size, fill);
    std::memcpy(list.data() + at, &part, 4);
    std::memcpy(list.data() + at + 4, desc, bytes);
}

void derived_cases()
{
    D3D12_DEPTH_STENCIL_DESC a{}, b{};
    a.DepthEnable = TRUE;
    a.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    a.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    a.FrontFace = a.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    b = a;
    b.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    b.StencilEnable = 7;  // a non-canonical TRUE
    b.StencilReadMask = 0xff;
    b.StencilWriteMask = 0x10;
    b.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    unsigned char with_pad[sizeof(D3D12_DEPTH_STENCIL_DESC)];
    std::memcpy(with_pad, &b, sizeof(b));
    with_pad[offsetof(D3D12_DEPTH_STENCIL_DESC, StencilWriteMask) + 1] = 0xcc;  // garbage in the padding
    D3D12_RASTERIZER_DESC r{D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK, 9, 0, 0, 0, TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    D3D12_BLEND_DESC blend{};
    std::vector<unsigned char> list;
    add_record(list, part_depth_stencil, &a, sizeof(a), 0xee);
    add_record(list, part_blend, &blend, sizeof(blend), 0xee);
    add_record(list, part_depth_stencil, with_pad, sizeof(with_pad), 0xee);
    add_record(list, part_rasterizer, &r, sizeof(r), 0xee);
    D3D12_DEPTH_STENCIL_DESC got{};
    D3D12_RASTERIZER_DESC got_r{};
    check(derived_depth_stencil(list.data(), list.size(), got) && got.DepthFunc == D3D12_COMPARISON_FUNC_GREATER_EQUAL && got.StencilEnable == TRUE &&
              got.StencilWriteMask == 0x10 && got.FrontFace.StencilPassOp == D3D12_STENCIL_OP_REPLACE, "last depth-stencil part");
    D3D12_DEPTH_STENCIL_DESC canonical{};
    std::memcpy(&canonical, &got, sizeof(got));
    D3D12_DEPTH_STENCIL_DESC rebuilt{};
    rebuilt.DepthEnable = TRUE;
    rebuilt.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    rebuilt.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    rebuilt.StencilEnable = TRUE;
    rebuilt.StencilReadMask = 0xff;
    rebuilt.StencilWriteMask = 0x10;
    rebuilt.FrontFace = b.FrontFace;
    rebuilt.BackFace = b.BackFace;
    check(std::memcmp(&canonical, &rebuilt, sizeof(rebuilt)) == 0, "padding cleaned (state hash stable)");
    check(derived_rasterizer(list.data(), list.size(), got_r) && got_r.CullMode == D3D12_CULL_MODE_BACK && got_r.FrontCounterClockwise == TRUE,
          "rasterizer part");
    check(last_part(list.data(), list.size(), part_blend) == record_size + 4, "blend part offset");
    check(last_part(list.data(), list.size() - 1, part_blend) == SIZE_MAX, "partial list refused");
    // Out-of-range fields refuse the part.
    std::vector<unsigned char> bad;
    D3D12_DEPTH_STENCIL_DESC c = a;
    c.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(9);
    add_record(bad, part_depth_stencil, &c, sizeof(c), 0);
    D3D12_RASTERIZER_DESC rr = r;
    rr.CullMode = static_cast<D3D12_CULL_MODE>(4);
    add_record(bad, part_rasterizer, &rr, sizeof(rr), 0);
    check(!derived_depth_stencil(bad.data(), bad.size(), got), "bad depth func refused");
    check(!derived_rasterizer(bad.data(), bad.size(), got_r), "bad cull mode refused");
    std::vector<unsigned char> none;
    add_record(none, part_blend, &blend, sizeof(blend), 0);
    check(!derived_depth_stencil(none.data(), none.size(), got) && !derived_rasterizer(none.data(), none.size(), got_r), "no part");
}

void target_cases()
{
    auto ds = decode_depth_stencil(depth_control(true, true, true, false, 6, true, 7, 7), stencil_control(0, 3, 0, 0, 3, 0), refmask(0, 0xff, 0xff, 1),
                                   refmask(0, 0xff, 0xff, 1)).desc;
    auto d = ds;
    check(adjust_for_target(d, false, false, false) == adjust_no_target && !d.DepthEnable && !d.StencilEnable, "no depth target");
    d = ds;
    check(adjust_for_target(d, true, false, false) == adjust_no_stencil && d.DepthEnable && !d.StencilEnable, "no stencil plane");
    d = ds;
    check(adjust_for_target(d, true, true, true) == adjust_read_only && d.DepthEnable && d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO &&
              d.StencilWriteMask == 0, "read-only target");
    d = ds;
    check(adjust_for_target(d, true, true, false) == 0 && std::memcmp(&d, &ds, sizeof(d)) == 0, "matching target unchanged");
}

void switch_cases()
{
    auto s = parse_switches(nullptr, nullptr, nullptr, nullptr, nullptr);
    check(!s.depth && !s.cull && !s.flip && !s.blend, "defaults off");
    s = parse_switches(nullptr, "1", "1", "1", "1");
    check(s.depth && s.cull && s.flip && s.blend, "all on");
    s = parse_switches("0", "1", "1", "1", "1");
    check(!s.depth && !s.cull && !s.flip && !s.blend, "X2W_DRAW_STATE=0 wins");
    s = parse_switches("1", nullptr, nullptr, nullptr, "1");
    check(!s.cull && !s.flip, "flip needs cull");
    s = parse_switches(nullptr, "10", "yes", "", nullptr);
    check(!s.depth && !s.cull && !s.blend, "only \"1\" turns a group on");
    check(!s.depth_clear, "depth clears default off");
    s = parse_switches(nullptr, nullptr, nullptr, nullptr, nullptr, "1");
    check(s.depth_clear && !s.depth, "depth clears on their own switch");
    s = parse_switches("0", nullptr, nullptr, nullptr, nullptr, "1");
    check(!s.depth_clear, "X2W_DRAW_STATE=0 turns depth clears off");
    check(!s.uncleared, "uncleared depth tests default off");
    s = parse_switches(nullptr, "1", nullptr, nullptr, nullptr, nullptr, "1");
    check(s.depth && s.uncleared, "uncleared depth tests on with depth");
    s = parse_switches(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "1");
    check(!s.uncleared, "uncleared depth tests need the depth group");
    s = parse_switches("0", "1", nullptr, nullptr, nullptr, nullptr, "1");
    check(!s.uncleared, "X2W_DRAW_STATE=0 turns uncleared depth tests off");
    check(!s.depthclip, "depth clip default off");
    s = parse_switches(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "1");
    check(s.depthclip && !s.cull && !s.depth && !s.blend, "depth clip on its own switch");
    s = parse_switches("0", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "1");
    check(!s.depthclip, "X2W_DRAW_STATE=0 turns depth clip off");
    s = parse_switches(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "yes");
    check(!s.depthclip, "only \"1\" turns depth clip on");
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--mutate") == 0) {
        mutate_winding = std::strcmp(argv[2], "winding") == 0;
        mutate_shift = std::strcmp(argv[2], "mesh-shift") == 0;
        if (!mutate_winding && !mutate_shift) {
            std::printf("unknown mutation\n");
            return 2;
        }
    } else if (argc > 1) {
        std::printf("usage: startup_pipeline_state_check [--mutate winding|mesh-shift]\n");
        return 2;
    }
    depth_stencil_cases();
    rasterizer_cases();
    blend_cases();
    block_cases();
    derived_cases();
    target_cases();
    switch_cases();
    std::printf("%d of %d checks failed\n", failures, cases);
    return failures ? 1 : 0;
}
