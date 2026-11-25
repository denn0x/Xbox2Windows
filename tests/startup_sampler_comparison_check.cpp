// Checks the static samplers src/startup_stubs/translated_sampler_bindings.h builds from manifest lines,
// without a device: a smpc line is a comparison sampler (D3D12 comparison filter and function from the
// SQ_IMG_SAMP words), a smp line stays an ordinary one, and the manifest order is the register order.
#include "startup_stubs/translated_sampler_bindings.h"

#include <cstdio>

namespace {
int failures;

void expect(bool ok, const char* what, int a = 0, int b = 0)
{
    if (!ok) {
        std::printf("FAIL %s (%d, %d)\n", what, a, b);
        ++failures;
    }
}

// SQ_TEX_DEPTH_COMPARE 0..7 in hardware order, written as the D3D12 enumerators.
const D3D12_COMPARISON_FUNC funcs[8] = {
    D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
    D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL,
    D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
}  // namespace

int main()
{
    for (unsigned func = 0; func < 8; ++func)
        for (unsigned linear = 0; linear < 2; ++linear) {
            // CLAMP_LAST_TEXEL in X and Y, TRUNC_COORD with point filtering, the XY filter in word 2 bits 20..23.
            const unsigned w0 = 2u | 2u << 3 | func << 12 | (linear ? 0u : 1u << 27);
            const unsigned w2 = linear ? 1u << 20 | 1u << 22 : 0u;
            char line[96];
            std::snprintf(line, sizeof(line), "smpc 0 0x%x 0x0 0x%x 0x0", w0, w2);
            x2w_startup::TranslatedSamplers samplers;
            expect(samplers.read_manifest_line(line) && samplers.statics.size() == 1, "smpc line parsed", func, linear);
            const auto decoded = x2w_startup::decode_statics(samplers);
            if (decoded.size() != 1) continue;
            const auto& s = decoded[0];
            // D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT is 0x80, ..._MIN_MAG_LINEAR_MIP_POINT is 0x94.
            expect(s.Filter == (linear ? 0x94 : 0x80), "comparison filter", func, linear);
            expect(s.ComparisonFunc == funcs[func], "comparison function", func, linear);
            expect(s.AddressU == D3D12_TEXTURE_ADDRESS_MODE_CLAMP && s.AddressV == D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                   "clamp addressing", func, linear);
        }

    // An ordinary smp line is not a comparison sampler, whatever bits 14:12 hold, and keeps its register.
    x2w_startup::TranslatedSamplers mixed;
    expect(mixed.read_manifest_line("smp 0 0x7012 0x0 0x0 0x0") &&
           mixed.read_manifest_line("smpc 1 0x1012 0x0 0x0 0x0"), "mixed lines parsed");
    const auto both = x2w_startup::decode_statics(mixed);
    expect(both.size() == 2, "two samplers", int(both.size()));
    if (both.size() == 2) {
        expect(both[0].ShaderRegister == 0 && both[0].Filter == 0x0 && both[0].ComparisonFunc == D3D12_COMPARISON_FUNC_ALWAYS,
               "ordinary sampler at register 0", both[0].Filter, both[0].ComparisonFunc);
        expect(both[1].ShaderRegister == 1 && both[1].Filter == 0x80 && both[1].ComparisonFunc == D3D12_COMPARISON_FUNC_LESS,
               "comparison sampler at register 1", both[1].Filter, both[1].ComparisonFunc);
    }
    // LOCAL (textures): the LOD and border fields (X2W_SAMPLER_LOD), passed explicitly. Word 2 0x18503f80 is the run-97
    // manifest word with LOD_BIAS 0x3f80 = -128/256; 0x2000 is the most negative 14-bit value (-32, clamped to -16).
    {
        using x2w_startup::decode_sampler;
        const auto biased = decode_sampler({0, 0xfffu << 12, 0x18503f80, 0}, 0, false, true);
        expect(biased.MipLODBias == -0.5f && biased.MinLOD == 0.0f && biased.MaxLOD == D3D12_FLOAT32_MAX, "bias -0.5");
        expect(decode_sampler({0, 0, 0x100, 0}, 0, false, true).MipLODBias == 1.0f, "bias +1");
        expect(decode_sampler({0, 0, 0x2000, 0}, 0, false, true).MipLODBias == D3D12_MIP_LOD_BIAS_MIN, "bias clamped low");
        expect(decode_sampler({0, 0, 0x1fff, 0}, 0, false, true).MipLODBias == D3D12_MIP_LOD_BIAS_MAX, "bias clamped high");
        const auto limited = decode_sampler({0, 0x180u | 0x280u << 12, 0, 1u << 30}, 0, false, true);
        expect(limited.MinLOD == 1.5f && limited.MaxLOD == 2.5f, "min 1.5 max 2.5");
        expect(limited.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, "border opaque black");
        expect(decode_sampler({0, 0, 0, 2u << 30}, 0, false, true).BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
               "border opaque white");
        expect(decode_sampler({0, 0, 0, 3u << 30}, 0, false, true).BorderColor == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
               "border register -> transparent black");
        expect(decode_sampler({0, 0x300u | 0x100u << 12, 0, 0}, 0, false, true).MaxLOD == 3.0f, "max below min raised to min");
        // Negative control: without the fields the old values stay (no bias, no LOD limit, transparent black).
        const auto plain = decode_sampler({0, 0x180u | 0x280u << 12, 0x18503f80, 2u << 30}, 0, false, false);
        expect(plain.MipLODBias == 0.0f && plain.MinLOD == 0.0f && plain.MaxLOD == D3D12_FLOAT32_MAX &&
               plain.BorderColor == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, "fields dropped when off");
        const auto dynamic = x2w_startup::dynamic_sampler({0, 0, 0, 2u << 30}, true);
        expect(dynamic.BorderColor[0] == 1.0f && dynamic.BorderColor[3] == 1.0f, "dynamic border white");
        const auto fallback = x2w_startup::border_sampler();
        expect(fallback.MaxLOD == D3D12_FLOAT32_MAX, "fallback sampler unlimited");
    }
    std::printf(failures ? "startup_sampler_comparison failed\n" : "startup_sampler_comparison ok\n");
    return failures ? 1 : 0;
}
