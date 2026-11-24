// Source-built comparison of XG-style swizzle selection against AMD addrlib (#870).
//
// What is public:
//   - The XG caller sequence (microsoft/DirectXTex Auxiliary/DirectXTexXboxTile.cpp at
//     a8e5dbf42909aeeb9fcf9c2ae399ae3c06737379, MIT): a caller that has no specific mode asks XG for one
//     (XGComputeOptimalSwizzleMode(dimension, format, width, height, arraySize, sampleCount, bindFlags)) and
//     puts it in XG_TEXTURE2D_DESC::SwizzleMode. DirectXTex does not implement selection itself.
//   - AMD addrlib (Mesa src/amd/addrlib at tag mesa-25.1.0, 5c142e46f3f6e752ed745fd48912ebb8fad67145, MIT) is
//     the supported swizzle selector for gfx10.3: Addr2GetPreferredSurfaceSetting and Addr2ComputeSurfaceInfo.
//     The create inputs follow Mesa's ac_addrlib_create, and the selection inputs follow
//     gfx9_get_preferred_swizzle_mode (Mesa 25.1.0, src/amd/common/ac_surface.c, MIT). The 256 B and
//     variable-size blocks are forbidden and the resource lives in the CPU-invisible local heap.
//   - The configuration is Mesa's "navi21" surface-test configuration (src/amd/common/ac_fake_hw_db.h at
//     mesa-25.1.0): family NV (0x8F), external revision 0x28, GB_ADDR_CONFIG 0x40 with NUM_PIPES = 4 and
//     NUM_PKRS = 4, giving 0x444.
//
// These are "AddrLib observations": a match establishes the chosen PC reference behavior only. The
// Xbox-specific XG selection and its inputs still need separate evidence.
//
// Not public: the console's GB_ADDR_CONFIG, XG's own selection rules, the signature of the imported
// XGSuggestSwizzleMode, and XG enum values. The table below records what addrlib selects on the desktop
// Navi 21 configuration. It is a reference to compare an XG provider against, not the console's answer.
//
// Proposed desktop policy (test-only provider, suggest_swizzle_mode): XG-style bind flags map to addrlib
// surface flags. Every resource is a texture. A render target sets color, a depth target sets depth, an
// unordered-access target sets unordered, and a display target sets color and display. A forced-linear
// request forbids every non-linear block.
//
// Returns 0 on pass, 1 on failure, and 77 (skip) when built without addrlib (XBOX2WINDOWS_ADDRLIB_DIR unset).
// With --mutate-linear-fallback the comparison must detect all nine mismatches.
#include <cstdint>
#include <cstdio>

#ifndef XBOX2WINDOWS_HAVE_ADDRLIB
int main()
{
    std::puts("xg_swizzle_select: SKIP (built without addrlib; run tools/fetch_addrlib.py and configure with "
              "-DXBOX2WINDOWS_ADDRLIB_DIR=<mesa>/src/amd/addrlib)");
    return 77;
}
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>

#include "addrinterface.h"
#include "../src/startup_stubs/bc64_chain.h"

namespace {
// Mesa ac_fake_hw_db.h "navi21": family_id AMDGPU_FAMILY_NV, chip_external_rev 0x28, gb_addr_config 0x40 with
// NUM_PIPES (bits 0-2) = 4 and NUM_PKRS (bits 8-10) = 4.
constexpr UINT_32 family_nv = 0x8F;
constexpr UINT_32 navi21_rev = 0x28;
constexpr UINT_32 navi21_gb_addr_config = 0x40 | 4u | (4u << 8);
constexpr UINT_32 arctic_island_engine = 0x0000000D;  // CIASICIDGFXENGINE_ARCTICISLAND (addrlib core/addrlib.h)
constexpr const char* addrlib_source = "Mesa 5c142e46f3f6e752ed745fd48912ebb8fad67145 (mesa-25.1.0) src/amd/addrlib (MIT)";

// ---- repo-local mirror of the XG-style request (field names from DirectXTex; values are repo-local) -------
enum Bind : std::uint32_t { srv = 1, rtv = 2, dsv = 4, uav = 8, display = 16 };
enum class Format { rgba8, r8, r16, r32, rgba32f };
struct XgRequest {
    const char* name;
    std::uint32_t width, height, mips;
    Format format;
    std::uint32_t bind;
    bool force_linear;
};

struct FormatInfo { AddrFormat addr; UINT_32 bpp; };
FormatInfo format_info(Format f)
{
    switch (f) {
    case Format::rgba8: return {ADDR_FMT_8_8_8_8, 32};
    case Format::r8: return {ADDR_FMT_8, 8};
    case Format::r16: return {ADDR_FMT_16, 16};
    case Format::r32: return {ADDR_FMT_32, 32};
    case Format::rgba32f: return {ADDR_FMT_32_32_32_32, 128};
    }
    return {ADDR_FMT_INVALID, 0};
}

const char* mode_name(AddrSwizzleMode m)
{
    switch (m) {
    case ADDR_SW_LINEAR: return "LINEAR";
    case ADDR_SW_4KB_S: return "4KB_S";
    case ADDR_SW_4KB_D: return "4KB_D";
    case ADDR_SW_4KB_S_X: return "4KB_S_X";
    case ADDR_SW_4KB_D_X: return "4KB_D_X";
    case ADDR_SW_4KB_R_X: return "4KB_R_X";
    case ADDR_SW_4KB_Z_X: return "4KB_Z_X";
    case ADDR_SW_64KB_S: return "64KB_S";
    case ADDR_SW_64KB_D: return "64KB_D";
    case ADDR_SW_64KB_S_T: return "64KB_S_T";
    case ADDR_SW_64KB_D_T: return "64KB_D_T";
    case ADDR_SW_64KB_S_X: return "64KB_S_X";
    case ADDR_SW_64KB_D_X: return "64KB_D_X";
    case ADDR_SW_64KB_R_X: return "64KB_R_X";
    case ADDR_SW_64KB_Z_X: return "64KB_Z_X";
    default: return "other";
    }
}

VOID* ADDR_API alloc_sys_mem(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE ADDR_API free_sys_mem(const ADDR_FREESYSMEM_INPUT* in)
{
    std::free(in->pVirtAddr);
    return ADDR_OK;
}

ADDR_HANDLE create_navi21()
{
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof in;
    out.size = sizeof out;
    in.chipEngine = arctic_island_engine;
    in.chipFamily = family_nv;
    in.chipRevision = navi21_rev;
    in.callbacks.allocSysMem = alloc_sys_mem;
    in.callbacks.freeSysMem = free_sys_mem;
    in.regValue.gbAddrConfig = navi21_gb_addr_config;
    return AddrCreate(&in, &out) == ADDR_OK ? out.hLib : nullptr;
}

ADDR2_SURFACE_FLAGS surface_flags(std::uint32_t bind)
{
    ADDR2_SURFACE_FLAGS f{};
    f.texture = 1;
    if (bind & (rtv | display)) f.color = 1;
    if (bind & display) f.display = 1;
    if (bind & dsv) f.depth = 1;
    if (bind & uav) f.unordered = 1;
    return f;
}

struct Selection {
    ADDR_E_RETURNCODE rc = ADDR_ERROR;
    AddrSwizzleMode mode = ADDR_SW_MAX_TYPE;
    UINT_32 base_align = 0;
    UINT_64 surf_size = 0;
};

ADDR_E_RETURNCODE surface_info(ADDR_HANDLE lib, const XgRequest& r, AddrSwizzleMode mode,
                              ADDR2_COMPUTE_SURFACE_INFO_OUTPUT& out, ADDR2_MIP_INFO* levels = nullptr)
{
    const FormatInfo fi = format_info(r.format);
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
    in.size = sizeof in;
    in.flags = surface_flags(r.bind);
    in.swizzleMode = mode;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.format = fi.addr;
    in.bpp = fi.bpp;
    in.width = r.width;
    in.height = r.height;
    in.numSlices = 1;
    in.numMipLevels = r.mips;
    in.numSamples = 1;
    in.numFrags = 1;
    out.size = sizeof out;
    out.pMipInfo = levels;
    return Addr2ComputeSurfaceInfo(lib, &in, &out);
}

// Test-only XG-style provider: the proposed desktop mapping of an XG request onto addrlib's selector, followed
// by Addr2ComputeSurfaceInfo for the chosen mode (base alignment and size show the block the mode implies).
Selection suggest_swizzle_mode(ADDR_HANDLE lib, const XgRequest& r, bool forbid_linear = false)
{
    const FormatInfo fi = format_info(r.format);
    ADDR2_GET_PREFERRED_SURF_SETTING_INPUT in{};
    ADDR2_GET_PREFERRED_SURF_SETTING_OUTPUT out{};
    in.size = sizeof in;
    out.size = sizeof out;
    in.flags = surface_flags(r.bind);
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.format = fi.addr;
    in.resourceLoction = ADDR_RSRC_LOC_INVIS;
    in.forbiddenBlock.micro = 1;
    in.forbiddenBlock.var = 1;
    if (r.force_linear) {
        in.forbiddenBlock.macroThin4KB = in.forbiddenBlock.macroThick4KB = 1;
        in.forbiddenBlock.macroThin64KB = in.forbiddenBlock.macroThick64KB = 1;
    }
    if (forbid_linear) in.forbiddenBlock.linear = 1;
    in.bpp = fi.bpp;
    in.width = r.width;
    in.height = r.height;
    in.numSlices = 1;
    in.numMipLevels = r.mips;
    in.numSamples = 1;
    in.numFrags = 1;
    Selection s;
    s.rc = Addr2GetPreferredSurfaceSetting(lib, &in, &out);
    if (s.rc != ADDR_OK) return s;
    s.mode = out.swizzleMode;

    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT so{};
    s.rc = surface_info(lib, r, s.mode, so);
    s.base_align = so.baseAlign;
    s.surf_size = so.surfSize;
    return s;
}

unsigned failures = 0;
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

using Provider = Selection (*)(ADDR_HANDLE, const XgRequest&);
Selection reference_provider(ADDR_HANDLE lib, const XgRequest& r) { return suggest_swizzle_mode(lib, r); }
// A provider that silently falls back to linear: it asks addrlib with every non-linear block forbidden.
Selection linear_fallback_provider(ADDR_HANDLE lib, const XgRequest& r)
{
    XgRequest linear = r;
    linear.force_linear = true;
    return suggest_swizzle_mode(lib, linear);
}

// The reference table: addrlib's selection for each request on the Navi 21 configuration above, recorded
// from this build (AddrLib observations, not console values).
struct Row {
    XgRequest request;
    AddrSwizzleMode mode;
    UINT_32 base_align;
};
constexpr Row rgba8_mip_reference{
    {"128x128 RGBA8 8 mips, shader resource", 128, 128, 8, Format::rgba8, srv, false}, ADDR_SW_64KB_R_X, 65536};
// clang-format off
const Row rows[] = {
    rgba8_mip_reference,
    {{"1024x1024 RGBA8 11 mips, shader resource",         1024, 1024, 11, Format::rgba8,  srv,           false}, ADDR_SW_64KB_R_X, 65536},
    {{"64x64 R8 1 mip, shader resource",                  64,   64,   1, Format::r8,      srv,           false}, ADDR_SW_4KB_D_X, 4096},
    {{"16x16 RGBA8 1 mip, shader resource",               16,   16,   1, Format::rgba8,   srv,           false}, ADDR_SW_4KB_D_X, 4096},
    {{"4096x1 R32 1 mip, shader resource",                4096, 1,    1, Format::r32,     srv,           false}, ADDR_SW_LINEAR, 256},
    {{"1920x1080 RGBA8 1 mip, render target",             1920, 1080, 1, Format::rgba8,   srv | rtv,     false}, ADDR_SW_64KB_R_X, 65536},
    {{"1920x1080 RGBA8 1 mip, display render target",     1920, 1080, 1, Format::rgba8,   rtv | display, false}, ADDR_SW_64KB_R_X, 65536},
    {{"1920x1080 R32 1 mip, depth target",                1920, 1080, 1, Format::r32,     srv | dsv,     false}, ADDR_SW_64KB_Z_X, 65536},
    {{"512x512 RGBA32F 10 mips, unordered access",        512,  512,  10, Format::rgba32f, srv | uav,    false}, ADDR_SW_64KB_R_X, 65536},
    {{"2048x2048 R16 1 mip, unordered access",            2048, 2048, 1, Format::r16,     srv | uav,     false}, ADDR_SW_64KB_R_X, 65536},
};
// clang-format on

bool matches(const Selection& s, const Row& row)
{
    return s.rc == ADDR_OK && s.mode == row.mode && s.base_align == row.base_align;
}

// The one comparison every provider goes through: the number of reference rows it does not reproduce.
unsigned table_mismatches(ADDR_HANDLE lib, Provider provider)
{
    unsigned n = 0;
    for (const Row& row : rows) n += !matches(provider(lib, row.request), row);
    return n;
}

// DirectXTex #498 supplies the call shape. The descriptor and explicit encoding below
// are fixture-local and carry the chosen PC mode from the selector to the texture computer.
constexpr UINT_32 unknown_layout = 0;
constexpr UINT_32 explicit_layout = 0x100;
constexpr std::uint32_t untouched = 0xffffffffu;
constexpr std::size_t factory_mips = 8;
constexpr std::size_t factory_texels = 21845;

struct FactoryDesc {
    UINT_32 width, height, mips, layout;
};
static_assert(sizeof(FactoryDesc) == 4 * sizeof(UINT_32));

XgRequest factory_request(const FactoryDesc& desc)
{
    return {"swizzle factory", desc.width, desc.height, desc.mips, Format::rgba8, srv, false};
}

HRESULT suggest_factory_layout(ADDR_HANDLE lib, const FactoryDesc* desc, UINT_32 selector, UINT_32* output)
{
    if (!desc || !output || selector != 0 || desc->width == 0 || desc->height == 0 ||
        desc->mips != factory_mips || desc->layout != unknown_layout) return E_INVALIDARG;
    const Selection selection = suggest_swizzle_mode(lib, factory_request(*desc));
    if (selection.rc != ADDR_OK) return E_FAIL;
    *output = explicit_layout | unsigned(selection.mode);
    return S_OK;
}

struct TextureComputer {
    UINT_64 size = 0;
    UINT_32 alignment = 0;
    AddrSwizzleMode mode = ADDR_SW_MAX_TYPE;
    std::array<ADDR2_MIP_INFO, factory_mips> levels{};
    std::vector<std::size_t> word_offsets;
};

bool create_texture_computer(ADDR_HANDLE lib, const FactoryDesc& desc, TextureComputer& computer)
{
    if (desc.width == 0 || desc.height == 0 || desc.mips != factory_mips ||
        desc.layout < explicit_layout || desc.layout >= explicit_layout + ADDR_SW_MAX_TYPE) return false;
    TextureComputer next;
    next.mode = static_cast<AddrSwizzleMode>(desc.layout - explicit_layout);
    const XgRequest request = factory_request(desc);
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT surface{};
    if (surface_info(lib, request, next.mode, surface, next.levels.data()) != ADDR_OK ||
        surface.surfSize == 0 || surface.surfSize % sizeof(std::uint32_t) != 0) return false;
    next.size = surface.surfSize;
    next.alignment = surface.baseAlign;

    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    in.size = sizeof in;
    in.flags = surface_flags(request.bind);
    in.swizzleMode = next.mode;
    in.resourceType = ADDR_RSRC_TEX_2D;
    in.bpp = format_info(request.format).bpp;
    in.unalignedWidth = desc.width;
    in.unalignedHeight = desc.height;
    in.numSlices = 1;
    in.numMipLevels = desc.mips;
    in.numSamples = 1;
    in.numFrags = 1;
    next.word_offsets.reserve(factory_texels);
    for (UINT_32 mip = 0; mip < desc.mips; ++mip) {
        in.mipId = mip;
        for (in.y = 0; in.y < std::max(1u, desc.height >> mip); ++in.y) {
            for (in.x = 0; in.x < std::max(1u, desc.width >> mip); ++in.x) {
                ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};
                out.size = sizeof out;
                if (Addr2ComputeSurfaceAddrFromCoord(lib, &in, &out) != ADDR_OK || out.bitPosition != 0 ||
                    out.addr >= next.size || out.addr % sizeof(std::uint32_t) != 0) return false;
                next.word_offsets.push_back(static_cast<std::size_t>(out.addr / sizeof(std::uint32_t)));
            }
        }
    }
    computer = std::move(next);
    return true;
}

bool same_layout(const TextureComputer& a, const TextureComputer& b)
{
    if (std::tie(a.size, a.alignment, a.mode, a.word_offsets) !=
        std::tie(b.size, b.alignment, b.mode, b.word_offsets)) return false;
    for (std::size_t mip = 0; mip < factory_mips; ++mip) {
        const auto& x = a.levels[mip];
        const auto& y = b.levels[mip];
        if (std::tie(x.pitch, x.height, x.offset, x.macroBlockOffset, x.mipTailOffset) !=
            std::tie(y.pitch, y.height, y.offset, y.macroBlockOffset, y.mipTailOffset)) return false;
    }
    return true;
}

std::uint32_t source_texel(std::size_t index) { return 0xab120000u + static_cast<std::uint32_t>(index); }

bool write_texels(const TextureComputer& computer, std::vector<std::uint32_t>& surface)
{
    surface.assign(static_cast<std::size_t>(computer.size / sizeof(std::uint32_t)) + 2, untouched);
    if (computer.word_offsets.size() != factory_texels) return false;
    for (std::size_t i = 0; i < computer.word_offsets.size(); ++i) {
        auto& word = surface[computer.word_offsets[i] + 1];
        if (word != untouched) return false;
        word = source_texel(i);
    }
    return surface.front() == untouched && surface.back() == untouched;
}

std::size_t count_mismatches(const TextureComputer& reader, const std::vector<std::uint32_t>& surface)
{
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < reader.word_offsets.size(); ++i) {
        const auto offset = reader.word_offsets[i];
        mismatches += offset >= surface.size() - 2 || surface[offset + 1] != source_texel(i);
    }
    return mismatches;
}

enum class FactoryResult { pass, missing_output, fail };

template <typename Suggest>
FactoryResult check_factory(ADDR_HANDLE lib, Suggest suggest)
{
    const Row& reference = rgba8_mip_reference;
    const FactoryDesc initial{reference.request.width, reference.request.height, reference.request.mips, unknown_layout};
    FactoryDesc aliased = initial;
    FactoryDesc separate = initial;
    UINT_32 output = untouched;
    const HRESULT alias_status = suggest(&aliased, 0, &aliased.layout);
    const HRESULT separate_status = suggest(&separate, 0, &output);
    if (alias_status != S_OK || separate_status != alias_status ||
        std::memcmp(&separate, &initial, sizeof initial) != 0) return FactoryResult::fail;
    if (aliased.layout == unknown_layout || output == untouched) return FactoryResult::missing_output;
    separate.layout = output;
    FactoryDesc expected = initial;
    expected.layout = explicit_layout | unsigned(reference.mode);
    if (std::memcmp(&aliased, &expected, sizeof expected) != 0 ||
        std::memcmp(&separate, &expected, sizeof expected) != 0) return FactoryResult::fail;

    TextureComputer alias_computer, separate_computer;
    if (!create_texture_computer(lib, aliased, alias_computer) ||
        !create_texture_computer(lib, separate, separate_computer) ||
        !same_layout(alias_computer, separate_computer) || alias_computer.alignment != reference.base_align) {
        return FactoryResult::fail;
    }

    FactoryDesc linear = initial;
    linear.layout = explicit_layout | unsigned(ADDR_SW_LINEAR);
    TextureComputer linear_computer;
    std::vector<std::uint32_t> tiled_surface, linear_surface;
    if (!create_texture_computer(lib, linear, linear_computer) || linear_computer.alignment != 256 ||
        !write_texels(alias_computer, tiled_surface) || !write_texels(linear_computer, linear_surface)) {
        return FactoryResult::fail;
    }
    const auto tiled_errors = count_mismatches(separate_computer, tiled_surface);
    const auto linear_errors = count_mismatches(linear_computer, linear_surface);
    const auto stale_errors = count_mismatches(linear_computer, tiled_surface);
    std::printf("  layout 0x%x (%s), %zu mips, %zu texels, tiled %llu bytes align %u, linear %llu bytes align %u\n",
                output, mode_name(alias_computer.mode), factory_mips, factory_texels,
                static_cast<unsigned long long>(alias_computer.size), alias_computer.alignment,
                static_cast<unsigned long long>(linear_computer.size), linear_computer.alignment);
    std::printf("  tiled mismatches %zu, linear mismatches %zu, stale linear interpretation %zu\n",
                tiled_errors, linear_errors, stale_errors);
    if (tiled_errors != 0 || linear_errors != 0 || stale_errors != factory_texels) return FactoryResult::fail;

    FactoryDesc invalid = initial;
    invalid.width = 0;
    const FactoryDesc invalid_before = invalid;
    if (suggest(&invalid, 0, &invalid.layout) != E_INVALIDARG ||
        std::memcmp(&invalid, &invalid_before, sizeof invalid) != 0) return FactoryResult::fail;
    output = untouched;
    TextureComputer rejected;
    if (suggest(&invalid, 0, &output) != E_INVALIDARG ||
        output != untouched || std::memcmp(&invalid, &invalid_before, sizeof invalid) != 0 ||
        create_texture_computer(lib, invalid, rejected) || create_texture_computer(lib, initial, rejected)) {
        return FactoryResult::fail;
    }
    std::puts("  zero width: E_INVALIDARG for both outputs; descriptor and separate sentinel preserved");
    return FactoryResult::pass;
}

int run_factory(bool mutate)
{
    ADDR_HANDLE lib = create_navi21();
    if (!lib) return 1;
    const auto selector = [lib](const FactoryDesc* desc, UINT_32 flags, UINT_32* output) {
        return suggest_factory_layout(lib, desc, flags, output);
    };
    const auto no_write = [](const FactoryDesc*, UINT_32, UINT_32*) -> HRESULT { return S_OK; };
    const FactoryResult result = mutate ? check_factory(lib, no_write) : check_factory(lib, selector);
    AddrDestroy(lib);
    const bool ok = result == (mutate ? FactoryResult::missing_output : FactoryResult::pass);
    if (mutate && ok) std::puts("  detected S_OK with missing layout output");
    std::printf("xg_swizzle_factory: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

bool block_addresses_match(ADDR_HANDLE lib, const ADDR2_COMPUTE_SURFACE_INFO_INPUT& surface)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Height = surface.height;
    desc.MipLevels = static_cast<UINT16>(surface.numMipLevels);
    desc.DepthOrArraySize = static_cast<UINT16>(surface.numSlices);
    const x2w_startup::bc64::Chain chain(desc);
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    in.size = sizeof(in);
    in.flags = surface.flags;
    in.resourceType = surface.resourceType;
    in.swizzleMode = surface.swizzleMode;
    in.bpp = 128;
    in.unalignedWidth = surface.width / 4;
    in.unalignedHeight = surface.height / 4;
    in.numMipLevels = surface.numMipLevels;
    in.numSlices = surface.numSlices;
    in.numSamples = in.numFrags = 1;
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};
    out.size = sizeof(out);
    for (in.slice = 0; in.slice < in.numSlices; ++in.slice) {
        for (in.mipId = 0; in.mipId < in.numMipLevels; ++in.mipId) {
            const unsigned columns = ((surface.width >> in.mipId) + 3) / 4;
            const unsigned rows = ((surface.height >> in.mipId) + 3) / 4;
            for (in.y = 0; in.y < rows; ++in.y) {
                for (in.x = 0; in.x < columns; ++in.x) {
                    const auto expected = chain.address(in.mipId, in.x, in.y, in.slice);
                    if (Addr2ComputeSurfaceAddrFromCoord(lib, &in, &out) != ADDR_OK ||
                        out.bitPosition != 0 || out.addr != expected) {
                        std::printf("BC block mismatch at slice %u mip %u (%u,%u): AddrLib %llu, XG %u\n",
                                    in.slice, in.mipId, in.x, in.y, static_cast<unsigned long long>(out.addr), expected);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

int bc64_tiling_reference()
{
    ADDR_HANDLE lib = create_navi21();
    if (!lib) return 1;
    for (auto format : {ADDR_FMT_BC5, ADDR_FMT_BC7}) {
        for (UINT_32 height : {512u, 1024u}) {
            for (UINT_32 slices : {1u, 2u}) {
                ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
                ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out{};
                std::array<ADDR2_MIP_INFO, 9> mips{};
                in.size = sizeof(in);
                in.flags.texture = 1;
                in.format = format;
                in.swizzleMode = ADDR_SW_64KB_S;
                in.resourceType = ADDR_RSRC_TEX_2D;
                in.width = 1024;
                in.height = height;
                in.numMipLevels = static_cast<UINT_32>(mips.size());
                in.numSlices = slices;
                in.numSamples = in.numFrags = 1;
                out.size = sizeof(out);
                out.pMipInfo = mips.data();
                constexpr unsigned tile = 65536;
                const unsigned slice_tiles = height == 512 ? 12 : 22;
                const std::array<unsigned, 3> starts{height == 512 ? 4u : 6u, 2u, 1u};
                bool matches = Addr2ComputeSurfaceInfo(lib, &in, &out) == ADDR_OK &&
                    out.sliceSize == slice_tiles * tile && out.surfSize == out.sliceSize * slices &&
                    out.firstMipIdInTail == starts.size() && out.blockWidth == 64 && out.blockHeight == 64;
                for (unsigned i = 0; i < mips.size(); ++i) {
                    const unsigned block_offset = i < starts.size() ? starts[i] * tile : 0;
                    matches &= mips[i].macroBlockOffset == block_offset;
                }
                std::printf("BC surface format=%u, 1024x%u, %u slice(s): %llu bytes per slice, first tail %u\n",
                            static_cast<unsigned>(format), height, slices,
                            static_cast<unsigned long long>(out.sliceSize), out.firstMipIdInTail);
                expect(matches, "64 KiB standard block starts, packed tile and complete array size");
                expect(block_addresses_match(lib, in), "all visible BC blocks match AddrLib");
            }
        }
    }
    AddrDestroy(lib);
    return failures ? 1 : 0;
}

int run(bool mutate_linear_fallback)
{
    std::printf("AddrLib observations from %s\n", addrlib_source);
    std::printf("  AddrCreate: chipEngine 0x%X chipFamily 0x%X chipRevision 0x%X gbAddrConfig 0x%X createFlags 0\n",
                arctic_island_engine, family_nv, navi21_rev, navi21_gb_addr_config);
    std::puts("  selection inputs: ADDR_RSRC_TEX_2D, ADDR_RSRC_LOC_INVIS, forbiddenBlock micro+var, 1 slice, 1 sample");
    ADDR_HANDLE lib = create_navi21();
    expect(lib != nullptr, "AddrCreate for family NV rev 0x28, GB_ADDR_CONFIG 0x444 (Mesa navi21 test config)");
    if (!lib) return 1;

    // 1. The provider under test against the reference table, through the shared comparison.
    const Provider under_test = mutate_linear_fallback ? linear_fallback_provider : reference_provider;
    if (mutate_linear_fallback) std::puts("  mutation: the provider under test is the linear-fallback provider");
    unsigned tiled_rows = 0;
    std::vector<AddrSwizzleMode> chosen;
    for (const Row& row : rows) {
        const Selection s = under_test(lib, row.request);
        chosen.push_back(s.mode);
        const FormatInfo fi = format_info(row.request.format);
        std::printf("  %-50s fmt 0x%X bpp %u flags 0x%X mips %u -> status %d %s (%u) baseAlign %u surfSize %llu\n",
                    row.request.name, unsigned(fi.addr), fi.bpp, surface_flags(row.request.bind).value, row.request.mips,
                    int(s.rc), mode_name(s.mode), unsigned(s.mode), s.base_align, (unsigned long long)s.surf_size);
        char line[200];
        std::snprintf(line, sizeof line, "%s selects %s with %u-byte base alignment", row.request.name,
                      mode_name(row.mode), row.base_align);
        const bool expected_match = !mutate_linear_fallback || row.mode == ADDR_SW_LINEAR;
        if (mutate_linear_fallback && !expected_match) {
            std::snprintf(line, sizeof line, "linear fallback mismatches %s", row.request.name);
        }
        expect(matches(s, row) == expected_match, line);
        tiled_rows += row.mode != ADDR_SW_LINEAR;
    }
    const unsigned expected_mismatches = mutate_linear_fallback ? 9 : 0;
    expect(table_mismatches(lib, under_test) == expected_mismatches, "the comparison finds the expected mismatch count");

    // 2. Linear-fallback control: a provider that sends each request to addrlib with only the linear block allowed
    //    goes through the same comparison and must be caught. It agrees only on the reference's own LINEAR row;
    //    the 8 tiled colour rows get LINEAR and the depth target is refused.
    const unsigned fallback_mismatches = table_mismatches(lib, linear_fallback_provider);
    std::printf("  tiled reference rows %u, linear-fallback provider mismatches %u of %zu\n", tiled_rows,
                fallback_mismatches, sizeof rows / sizeof rows[0]);
    expect(tiled_rows == 9 && fallback_mismatches == tiled_rows,
           "linear-fallback control: the comparison rejects the addrlib-backed linear-fallback provider on all 9 tiled rows");

    // 3. Forced-linear control: with only the linear block allowed, every colour request selects LINEAR and the
    //    depth target, which has no linear mode, is refused rather than given a tiled mode.
    bool all_linear = true;
    for (const Row& row : rows) {
        XgRequest r = row.request;
        r.force_linear = true;
        const Selection s = suggest_swizzle_mode(lib, r);
        std::printf("  forced linear: %-50s -> status %d %s\n", row.request.name, int(s.rc), mode_name(s.mode));
        if (r.bind & dsv)
            all_linear &= s.rc == ADDR_INVALIDPARAMS;
        else
            all_linear &= s.rc == ADDR_OK && s.mode == ADDR_SW_LINEAR;
    }
    expect(all_linear,
           "forced-linear control: 9 colour requests select LINEAR, the depth target is refused (ADDR_INVALIDPARAMS)");

    // 4. Tiled-request control: forbidding the linear block never yields LINEAR, including for the
    //    one-row texture.
    bool none_linear = true;
    for (const Row& row : rows) {
        const Selection s = suggest_swizzle_mode(lib, row.request, true);
        none_linear &= s.rc == ADDR_OK && s.mode != ADDR_SW_LINEAR;
        if (s.mode == ADDR_SW_LINEAR) std::printf("  %s still LINEAR with linear forbidden\n", row.request.name);
    }
    expect(none_linear, "tiled-request control: no request selects LINEAR once the linear block is forbidden");

    // 5. Tight-pitch control: an explicit linear pitch below the required pitch is refused.
    {
        auto linear_pitch = [&](UINT_32 pitch, UINT_32* out_pitch) {
            ADDR2_COMPUTE_SURFACE_INFO_INPUT si{};
            ADDR2_COMPUTE_SURFACE_INFO_OUTPUT so{};
            si.size = sizeof si;
            so.size = sizeof so;
            si.flags.texture = 1;
            si.swizzleMode = ADDR_SW_LINEAR;
            si.resourceType = ADDR_RSRC_TEX_2D;
            si.format = ADDR_FMT_8_8_8_8;
            si.bpp = 32;
            si.width = 100;
            si.height = 64;
            si.numSlices = 1;
            si.numMipLevels = 1;
            si.numSamples = 1;
            si.numFrags = 1;
            si.pitchInElement = pitch;
            const ADDR_E_RETURNCODE rc = Addr2ComputeSurfaceInfo(lib, &si, &so);
            if (out_pitch) *out_pitch = so.pitch;
            return rc;
        };
        UINT_32 required = 0, accepted = 0;
        const ADDR_E_RETURNCODE base = linear_pitch(0, &required);
        std::printf("  100x64 RGBA8 linear: required pitch %u elements\n", required);
        expect(base == ADDR_OK && required >= 100, "linear 100x64 RGBA8 reports a pitch of at least 100 elements");
        expect(linear_pitch(required, &accepted) == ADDR_OK && accepted == required,
               "an explicit pitch equal to the required pitch is accepted");
        expect(linear_pitch(required + 1, nullptr) == ADDR_INVALIDPARAMS, "a misaligned explicit pitch is refused");
        expect(linear_pitch(64, nullptr) == ADDR_INVALIDPARAMS,
               "tight-pitch control: an explicit pitch of 64 elements for a 100-wide texture is refused");
    }

    // 6. Mismatched-input control: the reference does not give one answer for every request.
    bool varied = false;
    for (std::size_t i = 1; i < chosen.size(); ++i) varied |= chosen[i] != chosen[0];
    expect(varied, "mismatched-input control: the table's requests do not all select the same mode");

    AddrDestroy(lib);
    return failures ? 1 : 0;
}
}  // namespace

// The mutation passes only if the comparison detects the expected fallback mismatches.
int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--bc64-tiling") == 0) return bc64_tiling_reference();
    if (argc == 2 && std::strcmp(argv[1], "--factory") == 0) return run_factory(false);
    if (argc == 2 && std::strcmp(argv[1], "--mutate-factory-no-write") == 0) return run_factory(true);
    const bool mutate = argc == 2 && std::strcmp(argv[1], "--mutate-linear-fallback") == 0;
    if (argc > 2 || (argc == 2 && !mutate)) {
        std::puts("usage: xg_swizzle_select_check [--mutate-linear-fallback|--factory|--mutate-factory-no-write|--bc64-tiling]");
        return 2;
    }
    const int result = run(mutate);
    std::printf("xg_swizzle_select: %s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}
#endif
