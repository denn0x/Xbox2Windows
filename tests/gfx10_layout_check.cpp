// LOCAL (textures): checks src/startup_stubs/gfx10_layout.h (the console-swizzled texture layout used to detile
// memory views) on synthetic shapes.
//
// Without addrlib (every build):
//   gfx10_layout_check <golden>            every element address of every golden shape hashes to the addrlib value
//                                          recorded in <golden>, and the layout agrees with the repo's two
//                                          independent tiled forms (Bc16Surface, bc64::Chain) where they overlap
//   gfx10_layout_check <golden> --mutate floor|tail   negative controls: a floor mip size or dropped tail
//                                          coordinates must be detected (the test then fails)
// With addrlib (XBOX2WINDOWS_ADDRLIB_DIR, Mesa 25.1.0 src/amd/addrlib, MIT; see tools/fetch_addrlib.py):
//   --addrlib                              every element address and surface size against Addr2ComputeSurfaceAddrFromCoord
//                                          / Addr2ComputeSurfaceInfo on four configurations (Navi1x and Navi21, 1 and
//                                          16 pipes), the in-tree equation table against addrlib, and BC formats given
//                                          as pixels (ADDR_FMT_BC*) against the same shape given as 4x4 blocks
//   --print-equations                      the equation table (src/startup_stubs/gfx10_layout_equations.inc)
//   --write-golden <path>                  the golden file, hashed from addrlib's addresses (not from gfx10_layout.h)
//   --write-tiled <shape> <path> --addrlib the synthetic surface of the WARP detile draw, every element at addrlib's
//                                          address (the same without --addrlib uses gfx10_layout.h: plumbing only)
#include "startup_stubs/gfx10_layout.h"
#include "startup_stubs/bc64_chain.h"
#include "startup_stubs/bc7_4k_s.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using x2w_startup::gfx10::Layout;

namespace {
struct Shape {
    const char* name;
    unsigned mode;
    bool is3d;
    unsigned elem;  // bytes per element
    unsigned w, h, d, mips;  // elements (4x4 blocks for BC), depth or array size
    unsigned bc;    // 0, or the BC number for the pixel-form addrlib check (pixel size = 4 * elements)
};

// The measured refusal shapes (runs 97/98, layout 0x105 and 0x100), then wider coverage of the four tiled modes.
const Shape shapes[] = {
    {"r16_64x64x32_array", 5, false, 2, 64, 64, 32, 1, 0},
    {"r16_64x64x32_array_7mips", 5, false, 2, 64, 64, 32, 7, 0},
    {"bc1_12x44", 5, false, 8, 3, 11, 1, 1, 1},
    {"bc3_512x468", 5, false, 16, 128, 117, 1, 1, 3},
    {"bc1_2048_12mips", 5, false, 8, 512, 512, 1, 12, 1},
    {"bc3_256_9mips", 5, false, 16, 64, 64, 1, 9, 3},
    {"rgba16f_17cubed_3d", 5, true, 8, 17, 17, 17, 1, 0},
    {"rgba16f_17cubed_3d_5mips", 5, true, 8, 17, 17, 17, 5, 0},
    {"r32f_128x72_7mips_linear", 0, false, 4, 128, 72, 1, 7, 0},
    {"rgba8_300x200_9mips_4k_s", 5, false, 4, 300, 200, 1, 9, 0},
    {"rgba8_300x200_9mips_4k_d", 6, false, 4, 300, 200, 1, 9, 0},
    {"rgba8_300x200_9mips_64k_s", 9, false, 4, 300, 200, 1, 9, 0},
    {"rgba8_300x200_9mips_64k_d", 10, false, 4, 300, 200, 1, 9, 0},
    {"r8_1000x7_10mips_64k_s", 9, false, 1, 1000, 7, 1, 10, 0},
    {"r16_77x300_9mips_4k_s", 5, false, 2, 77, 300, 1, 9, 0},
    {"rg32_129x65_8mips_64k_d", 10, false, 8, 129, 65, 1, 8, 0},
    {"rgba8_64x64x6_7mips_64k_s", 9, false, 4, 64, 64, 6, 7, 0},
    {"r8_32cubed_3d_6mips_64k_s", 9, true, 1, 32, 32, 32, 6, 0},
    {"rgba32f_40x24x9_3d_6mips_4k_s", 5, true, 16, 40, 24, 9, 6, 0},
    {"bc7_1024_9mips_64k_s", 9, false, 16, 256, 256, 1, 9, 7},
    {"bc7_1024x512x2_9mips_64k_s", 9, false, 16, 256, 128, 2, 9, 7},
    {"bc5_4096_13mips_64k_s", 9, false, 16, 1024, 1024, 1, 13, 5},
    {"bc1_13x5_4mips_4k_s", 5, false, 8, 4, 2, 1, 3, 1},
    {"r32f_77x33_7mips_linear", 0, false, 4, 77, 33, 1, 7, 0},
    {"rgba8_5x3x4_linear_3d", 0, true, 4, 5, 3, 4, 1, 0},
    // Run 102 census: layout 0x10a (SW_64KB_D) BC1 / BC7 mip chains; D modes with 8- and 16-byte elements.
    {"bc1_1024_11mips_64k_d", 10, false, 8, 256, 256, 1, 11, 1},
    {"bc7_2048x1024_12mips_64k_d", 10, false, 16, 512, 256, 1, 12, 7},
    {"bc7_300x200x3_7mips_4k_d", 6, false, 16, 75, 50, 3, 7, 7},
    {"bc1_44x12_4mips_4k_d", 6, false, 8, 11, 3, 1, 4, 1},
    // The WARP detile draw (tests/startup_memory_srv_detile_check.py; --write-tiled), with rgba16f_17cubed_3d_5mips.
    {"bc1_256_9mips_4k_s", 5, false, 8, 64, 64, 1, 9, 1},
    {"bc1_256x128x2_8mips_64k_d", 10, false, 8, 64, 32, 2, 8, 1},
    {"bc3_256_9mips_4k_s", 5, false, 16, 64, 64, 1, 9, 3},  // a tiled_bc_srv description the tiled paths refuse (mips)
    {"rgba16f_100x60_3mips_linear", 0, false, 8, 100, 60, 1, 3, 0},
};

unsigned ceil_shift(unsigned v, unsigned s) { return (v + (1u << s) - 1) >> s; }

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void add(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    }
};

// Every element of every level and slice, in a fixed order, with addrlib's (ceiling) level extents.
template<class Visit> void for_each_element(const Shape& s, Visit visit)
{
    for (unsigned mip = 0; mip < s.mips; ++mip) {
        const unsigned w = ceil_shift(s.w, mip), h = ceil_shift(s.h, mip);
        // A 3D mip holds max(1, depth >> mip) slices (D3D's floor; addrlib's Valid3DMipSliceIdConstraint refuses the
        // rest), while width and height round up as the placement does.
        const unsigned depth = s.is3d ? (std::max)(1u, s.d >> mip) : s.d;
        for (unsigned z = 0; z < depth; ++z)
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) visit(mip, x, y, z);
    }
}

Layout layout_of(const Shape& s, Layout::Mutation m = Layout::none)
{
    return Layout(s.mode, s.is3d, s.elem, s.w, s.h, s.d, s.mips, m);
}

int failures;
void fail(const char* format, const char* a, std::uint64_t b = 0, std::uint64_t c = 0)
{
    if (failures++ < 12) {
        std::printf("FAIL ");
        std::printf(format, a, b, c);
        std::printf("\n");
    }
}

// The two existing, independently written tiled forms, where gfx10_layout.h covers the same surface.
void cross_check()
{
    // Bc16Surface: 16-byte 4 KiB standard, one level.
    for (const auto [w, h] : {std::pair{512u, 468u}, std::pair{64u, 64u}, std::pair{100u, 36u}}) {
        const x2w_startup::Bc16Surface bc(w, h);
        const Layout l(5, false, 16, bc.columns, bc.rows, 1, 1);
        for (unsigned y = 0; y < bc.rows; ++y)
            for (unsigned x = 0; x < bc.columns; ++x)
                if (l.address(0, x, y, 0) != bc.block_offset(x, y)) {
                    fail("Bc16Surface %s differs at %llu,%llu", "4KB_S", x, y);
                    return;
                }
    }
    // bc64::Chain: 16-byte 64 KiB standard 1024-wide chains.
    for (const auto [h, mips, slices] : {std::tuple{1024u, 6u, 1u}, std::tuple{1024u, 9u, 2u}, std::tuple{512u, 9u, 1u}}) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1024;
        d.Height = h;
        d.DepthOrArraySize = static_cast<UINT16>(slices);
        d.MipLevels = static_cast<UINT16>(mips);
        d.Format = DXGI_FORMAT_BC7_UNORM;
        d.SampleDesc.Count = 1;
        d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x109);
        const x2w_startup::bc64::Chain chain(d);
        const Layout l(9, false, 16, 256, h / 4, slices, mips);
        if (l.size != chain.bytes()) fail("bc64::Chain size %s: %llu vs %llu", "64KB_S", l.size, chain.bytes());
        for (unsigned s = 0; s < slices; ++s)
            for (unsigned mip = 0; mip < mips; ++mip)
                for (unsigned y = 0; y < chain.rows(mip); ++y)
                    for (unsigned x = 0; x < chain.columns(mip); ++x)
                        if (l.address(mip, x, y, s) != chain.address(mip, x, y, s)) {
                            fail("bc64::Chain differs at mip/x %s %llu %llu", "64KB_S", mip, x);
                            return;
                        }
    }
}

int golden_check(const char* path, Layout::Mutation mutation)
{
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::printf("FAIL cannot open %s\n", path);
        return 1;
    }
    char line[512];
    unsigned checked = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[128];
        unsigned long long size = 0, hash = 0;
        if (std::sscanf(line, "%127s %llu %llx", name, &size, &hash) != 3) continue;
        const Shape* s = nullptr;
        for (const auto& c : shapes)
            if (std::strcmp(c.name, name) == 0) s = &c;
        if (!s) {
            fail("golden shape %s is not in the shape list", name);
            continue;
        }
        const Layout l = layout_of(*s, mutation);
        if (!l.valid) {
            fail("%s: layout refused", name);
            continue;
        }
        if (l.size != size) fail("%s: size %llu, golden %llu", name, l.size, size);
        Fnv fnv;
        for_each_element(*s, [&](unsigned mip, unsigned x, unsigned y, unsigned z) { fnv.add(l.address(mip, x, y, z)); });
        if (fnv.h != hash) fail("%s: address hash %llx, golden %llx", name, fnv.h, hash);
        ++checked;
    }
    std::fclose(f);
    if (checked != sizeof shapes / sizeof shapes[0]) fail("golden has %s%llu of %llu shapes", "", checked, sizeof shapes / sizeof shapes[0]);
    if (mutation == Layout::none) cross_check();
    std::printf("gfx10_layout%s: %u golden shapes, %d failures\n",
                mutation == Layout::floor_mips ? " --mutate floor" : mutation == Layout::no_tail_coords ? " --mutate tail" : "",
                checked, failures);
    return failures ? 1 : 0;
}

const Shape* shape_named(const char* name)
{
    for (const auto& s : shapes)
        if (std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

std::uint16_t half_of(unsigned n)  // an integer below 2048 as binary16 (exact)
{
    if (!n) return 0;
    unsigned e = 0;
    while ((2u << e) <= n) ++e;
    return static_cast<std::uint16_t>(((e + 15) << 10) | ((n << (10 - e)) & 0x3ff));
}

// The synthetic element pattern of the WARP detile draw; tests/startup_memory_srv_detile_check.py checks the same
// values in its pixel shader. BC1 shapes: a solid block (color0 = color1 = id, indices 0) with id = x | y << 6 |
// (mip + 8 * slice) << 12. BC3 shapes: the same color block after a solid alpha block (alpha0 = alpha1 =
// 16 * level + 5, indices 0). Other 8-byte shapes: R16G16B16A16_FLOAT (x, y, z, mip + 1).
bool pattern(const Shape& s, unsigned mip, unsigned x, unsigned y, unsigned z, unsigned char* out)
{
    if (s.bc == 3 && s.elem == 16) {
        const unsigned level = mip + 8 * z;
        if (x >= 64 || y >= 64 || level >= 16) return false;
        const unsigned id = x | y << 6 | level << 12;
        const auto alpha = static_cast<unsigned char>(16 * level + 5);
        const unsigned char block[16] = {alpha, alpha, 0, 0, 0, 0, 0, 0, static_cast<unsigned char>(id), static_cast<unsigned char>(id >> 8),
                                         static_cast<unsigned char>(id), static_cast<unsigned char>(id >> 8), 0, 0, 0, 0};
        std::memcpy(out, block, 16);
        return true;
    }
    if (s.elem != 8) return false;
    if (s.bc == 1) {
        const unsigned level = mip + 8 * z;
        if (x >= 64 || y >= 64 || level >= 16) return false;
        const unsigned id = x | y << 6 | level << 12;
        const unsigned char block[8] = {static_cast<unsigned char>(id), static_cast<unsigned char>(id >> 8),
                                        static_cast<unsigned char>(id), static_cast<unsigned char>(id >> 8), 0, 0, 0, 0};
        std::memcpy(out, block, 8);
        return true;
    }
    if (s.bc || x >= 2048 || y >= 2048 || z >= 2048) return false;
    const std::uint16_t h[4] = {half_of(x), half_of(y), half_of(z), half_of(mip + 1)};
    std::memcpy(out, h, 8);
    return true;
}

// The surface bytes of a shape with every element's pattern at the address `address` gives (addrlib's for the
// WARP oracle, gfx10_layout.h's for the plumbing variant); gaps stay zero.
template<class Address> int write_tiled(const Shape& s, const char* path, std::uint64_t size, Address address)
{
    if (!size || size > (64ull << 20)) {
        std::printf("FAIL %s: surface size %llu\n", s.name, static_cast<unsigned long long>(size));
        return 1;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    bool ok = true;
    unsigned elements = 0;
    for_each_element(s, [&](unsigned mip, unsigned x, unsigned y, unsigned z) {
        std::uint64_t a = 0;
        if (!ok || !address(mip, x, y, z, a) || a + s.elem > size || !pattern(s, mip, x, y, z, &bytes[static_cast<std::size_t>(a)]))
            ok = false;
        ++elements;
    });
    FILE* f = ok ? std::fopen(path, "wb") : nullptr;
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::printf("FAIL %s: cannot write the tiled surface to %s\n", s.name, path);
        if (f) std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("wrote %s: %s, %u elements, %llu bytes\n", path, s.name, elements, static_cast<unsigned long long>(size));
    return 0;
}

int write_tiled_layout(const char* name, const char* path)
{
    const Shape* s = shape_named(name);
    if (!s) return 2;
    const Layout l = layout_of(*s);
    if (!l.valid) return 1;
    return write_tiled(*s, path, l.size, [&](unsigned mip, unsigned x, unsigned y, unsigned z, std::uint64_t& a) {
        a = l.address(mip, x, y, z);
        return true;
    });
}
}  // namespace

#ifdef XBOX2WINDOWS_HAVE_ADDRLIB
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "addrinterface.h"

namespace {
VOID* ADDR_API alloc_sys_mem(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE ADDR_API free_sys_mem(const ADDR_FREESYSMEM_INPUT* in)
{
    std::free(in->pVirtAddr);
    return ADDR_OK;
}

struct Config {
    const char* name;
    UINT_32 revision, gb_addr_config;
    bool rb_plus;  // Navi21: addrlib's RB+ pattern tables
};
// Family NV (0x8F). Navi10 (revision 0x03, no RB+ tables) and Navi21 (0x28, RB+ tables); GB_ADDR_CONFIG NUM_PIPES
// (bits 0-2, log2) 0 or 4 with NUM_PKRS (bits 8-10) 0 or 4 and the 0x40 of Mesa's navi21 test configuration.
const Config configs[] = {
    {"navi10 1 pipe", 0x03, 0x40, false},
    {"navi10 16 pipes", 0x03, 0x40 | 4u | (4u << 8), false},
    {"navi21 1 pipe", 0x28, 0x40, true},
    {"navi21 16 pipes", 0x28, 0x40 | 4u | (4u << 8), true},
};

ADDR_HANDLE create(const Config& c)
{
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof in;
    out.size = sizeof out;
    in.chipEngine = 0x0000000D;  // CIASICIDGFXENGINE_ARCTICISLAND
    in.chipFamily = 0x8F;
    in.chipRevision = c.revision;
    in.callbacks.allocSysMem = alloc_sys_mem;
    in.callbacks.freeSysMem = free_sys_mem;
    in.regValue.gbAddrConfig = c.gb_addr_config;
    return AddrCreate(&in, &out) == ADDR_OK ? out.hLib : nullptr;
}

AddrFormat element_format(unsigned elem)
{
    switch (elem) {
    case 1: return ADDR_FMT_8;
    case 2: return ADDR_FMT_16;
    case 4: return ADDR_FMT_32;
    case 8: return ADDR_FMT_32_32;
    default: return ADDR_FMT_32_32_32_32;
    }
}
AddrFormat bc_format(unsigned bc)
{
    switch (bc) {
    case 1: return ADDR_FMT_BC1;
    case 3: return ADDR_FMT_BC3;
    case 5: return ADDR_FMT_BC5;
    default: return ADDR_FMT_BC7;
    }
}

ADDR_E_RETURNCODE info(ADDR_HANDLE lib, const Shape& s, ADDR2_COMPUTE_SURFACE_INFO_OUTPUT& out, ADDR2_MIP_INFO* mips,
                       bool pixels = false)
{
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
    in.size = sizeof in;
    in.flags.texture = 1;
    in.swizzleMode = static_cast<AddrSwizzleMode>(s.mode);
    in.resourceType = s.is3d ? ADDR_RSRC_TEX_3D : ADDR_RSRC_TEX_2D;
    in.format = pixels ? bc_format(s.bc) : element_format(s.elem);
    in.bpp = pixels ? 0 : s.elem * 8;
    in.width = pixels ? s.w * 4 : s.w;
    in.height = pixels ? s.h * 4 : s.h;
    in.numSlices = s.d;
    in.numMipLevels = s.mips;
    in.numSamples = 1;
    in.numFrags = 1;
    out = {};
    out.size = sizeof out;
    out.pMipInfo = mips;
    return Addr2ComputeSurfaceInfo(lib, &in, &out);
}

bool addr(ADDR_HANDLE lib, const Shape& s, unsigned mip, unsigned x, unsigned y, unsigned z, std::uint64_t& address)
{
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT out{};
    in.size = sizeof in;
    out.size = sizeof out;
    in.x = x;
    in.y = y;
    in.slice = z;
    in.mipId = mip;
    in.unalignedWidth = s.w;
    in.unalignedHeight = s.h;
    in.numSlices = s.d;
    in.numMipLevels = s.mips;
    in.numSamples = 1;
    in.numFrags = 1;
    in.swizzleMode = static_cast<AddrSwizzleMode>(s.mode);
    in.resourceType = s.is3d ? ADDR_RSRC_TEX_3D : ADDR_RSRC_TEX_2D;
    in.bpp = s.elem * 8;
    in.flags.texture = 1;
    if (Addr2ComputeSurfaceAddrFromCoord(lib, &in, &out) != ADDR_OK || out.bitPosition != 0) return false;
    address = out.addr;
    return true;
}

// The in-block equation of (thick, mode, element bytes), probed bit by bit on a one-block surface: in a non-XOR
// mode each coordinate bit moves exactly one address bit.
x2w_startup::gfx10::Equation probe(ADDR_HANDLE lib, bool thick, unsigned mode, unsigned e)
{
    x2w_startup::gfx10::Equation eq{};
    Shape s{"probe", mode, thick, 1u << e, 1, 1, 1, 1, 0};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT o{};
    if (info(lib, s, o, nullptr) != ADDR_OK) return eq;
    s.w = o.blockWidth;
    s.h = o.blockHeight;
    s.d = thick ? o.blockSlices : 1;
    const unsigned block_log2 = (mode == 5 || mode == 6) ? 12 : 16;
    std::uint8_t src[16];
    std::memset(src, 0xff, sizeof src);
    for (unsigned k = 0; k < e; ++k) src[k] = static_cast<std::uint8_t>(0xc0 | k);
    const auto place = [&](unsigned channel, unsigned extent, unsigned x, unsigned y, unsigned z, unsigned k) {
        std::uint64_t a = 0;
        if (!addr(lib, s, 0, x, y, z, a) || !a || (a & (a - 1)) || a >= (1ull << block_log2)) return false;
        unsigned bit = 0;
        while ((1ull << bit) != a) ++bit;
        if (src[bit] != 0xff) return false;
        src[bit] = static_cast<std::uint8_t>(channel | k);
        (void)extent;
        return true;
    };
    for (unsigned k = 0; (1u << k) < s.w; ++k)
        if (!place(0x00, s.w, 1u << k, 0, 0, k)) return eq;
    for (unsigned k = 0; (1u << k) < s.h; ++k)
        if (!place(0x40, s.h, 0, 1u << k, 0, k)) return eq;
    if (thick)
        for (unsigned k = 0; (1u << k) < s.d; ++k)
            if (!place(0x80, s.d, 0, 0, 1u << k, k)) return eq;
    for (unsigned i = 0; i < block_log2; ++i)
        if (src[i] == 0xff) return eq;
    eq.bits = static_cast<std::uint8_t>(block_log2);
    std::memcpy(eq.src, src, sizeof src);
    return eq;
}

const unsigned table_modes[4] = {5, 6, 9, 10};

int print_equations()
{
    ADDR_HANDLE lib = create(configs[0]);
    if (!lib) return 1;
    std::printf("// GENERATED by tests/gfx10_layout_check.cpp --print-equations from AMD addrlib (Mesa 5c142e46f3f6e752ed745fd48912ebb8fad67145,\n"
                "// mesa-25.1.0, src/amd/addrlib, MIT; see gfx10_layout.h): [thick][4KB_S 4KB_D 64KB_S 64KB_D][log2 bytes].\n");
    for (int thick = 0; thick < 2; ++thick) {
        std::printf("{\n");
        for (unsigned mode : table_modes) {
            std::printf("    {\n");
            for (unsigned e = 0; e < 5; ++e) {
                const auto eq = probe(lib, thick != 0, mode, e);
                std::printf("        {%u, {", eq.bits);
                for (unsigned i = 0; i < 16; ++i) std::printf("%s0x%02x", i ? ", " : "", eq.bits ? eq.src[i] : 0);
                std::printf("}},\n");
            }
            std::printf("    },\n");
        }
        std::printf("},\n");
    }
    AddrDestroy(lib);
    return 0;
}

int write_golden(const char* path)
{
    ADDR_HANDLE lib = create(configs[0]);
    FILE* f = lib ? std::fopen(path, "w") : nullptr;
    if (!f) return 1;
    std::fprintf(f, "# GENERATED by tests/gfx10_layout_check.cpp --write-golden from AMD addrlib (Mesa 5c142e46f3f6e752ed745fd48912ebb8fad67145,\n"
                    "# mesa-25.1.0, src/amd/addrlib, MIT), configuration %s: shape, surface bytes, FNV-1a 64 of every element\n"
                    "# address (level, slice or depth, row, column order, addrlib's level extents). Synthetic shapes only.\n",
                 configs[0].name);
    for (const auto& s : shapes) {
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT o{};
        ADDR2_MIP_INFO mips[16]{};
        if (info(lib, s, o, mips) != ADDR_OK) {
            std::printf("FAIL addrlib refuses %s\n", s.name);
            return 1;
        }
        Fnv fnv;
        bool ok = true;
        for_each_element(s, [&](unsigned mip, unsigned x, unsigned y, unsigned z) {
            std::uint64_t a = 0;
            ok = ok && addr(lib, s, mip, x, y, z, a);
            fnv.add(a);
        });
        if (!ok) {
            std::printf("FAIL addrlib address failed for %s\n", s.name);
            return 1;
        }
        std::fprintf(f, "%s %llu %016llx\n", s.name, static_cast<unsigned long long>(o.surfSize), static_cast<unsigned long long>(fnv.h));
    }
    std::fclose(f);
    AddrDestroy(lib);
    std::printf("wrote %s (%zu shapes)\n", path, sizeof shapes / sizeof shapes[0]);
    return 0;
}

int write_tiled_addrlib(const char* name, const char* path)
{
    const Shape* s = shape_named(name);
    ADDR_HANDLE lib = s ? create(configs[0]) : nullptr;
    if (!lib) return 2;
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT o{};
    ADDR2_MIP_INFO mips[16]{};
    int rc = 1;
    if (info(lib, *s, o, mips) == ADDR_OK)
        rc = write_tiled(*s, path, o.surfSize, [&](unsigned mip, unsigned x, unsigned y, unsigned z, std::uint64_t& a) {
            return addr(lib, *s, mip, x, y, z, a);
        });
    AddrDestroy(lib);
    return rc;
}

int debug_shape(const char* name)
{
    ADDR_HANDLE lib = create(configs[0]);
    for (const auto& s : shapes) {
        if (std::strcmp(s.name, name) != 0) continue;
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT o{};
        ADDR2_MIP_INFO mips[16]{};
        const auto rc = info(lib, s, o, mips);
        const Layout l = layout_of(s);
        std::printf("%s: addrlib rc %d size %llu slice %llu block %ux%ux%u first tail %u; layout size %llu slice %llu block %ux%ux%u\n",
                    s.name, rc, o.surfSize, o.sliceSize, o.blockWidth, o.blockHeight, o.blockSlices, o.firstMipIdInTail, l.size,
                    l.slice_size, l.bw, l.bh, l.bd);
        for (unsigned m = 0; m < s.mips; ++m) {
            std::uint64_t a = 0;
            const bool ok = addr(lib, s, m, 0, 0, 0, a);
            std::printf("  mip %u: addrlib pitch %u height %u depth %u offset %llu macro %llu tail %llu (%u,%u,%u) addr(0,0,0) %s%llu; "
                        "layout pitch %u height %u macro %llu tail %d (%u,%u) addr %llu\n",
                        m, mips[m].pitch, mips[m].height, mips[m].depth, mips[m].offset, mips[m].macroBlockOffset, static_cast<unsigned long long>(mips[m].mipTailOffset),
                        mips[m].mipTailCoordX, mips[m].mipTailCoordY, mips[m].mipTailCoordZ, ok ? "" : "FAILED ", a, l.level[m].pitch,
                        l.level[m].height, l.level[m].offset, l.level[m].in_tail, l.level[m].tail_x, l.level[m].tail_y, l.address(m, 0, 0, 0));
        }
    }
    AddrDestroy(lib);
    return 0;
}

// The D modes (4KB_D, 64KB_D) with 4-byte elements: addrlib's RB+ equation differs from the Navi1x one, so the
// in-tree table (Navi1x) is compared with Navi1x only, and on Navi21 the difference itself is asserted. The runtime
// does not detile D modes with 4-byte elements (d3d12_memory_srv.inl detile_layout).
bool chip_dependent(unsigned mode, unsigned elem) { return (mode == 6 || mode == 10) && elem == 4; }

int addrlib_check()
{
    unsigned compared = 0, chip_dependent_seen = 0;
    for (const auto& c : configs) {
        ADDR_HANDLE lib = create(c);
        if (!lib) {
            fail("AddrCreate %s", c.name);
            continue;
        }
        for (int thick = 0; thick < 2; ++thick)
            for (unsigned mi = 0; mi < 4; ++mi)
                for (unsigned e = 0; e < 5; ++e) {
                    const auto eq = probe(lib, thick != 0, table_modes[mi], e);
                    const auto& in_tree = x2w_startup::gfx10::equations[thick][mi][e];
                    const bool differs = eq.bits != in_tree.bits || (eq.bits && std::memcmp(eq.src, in_tree.src, sizeof eq.src) != 0);
                    if (c.rb_plus && !thick && chip_dependent(table_modes[mi], 1u << e)) {
                        if (!differs) fail("%s: the D-mode equation e %llu was expected to differ on RB+", c.name, e);
                        ++chip_dependent_seen;
                        continue;
                    }
                    if (differs)
                        fail("%s: equation thick %llu mode index/e %llu differs from the in-tree table", c.name, thick, mi * 10 + e);
                }
        for (const auto& s : shapes) {
            ADDR2_COMPUTE_SURFACE_INFO_OUTPUT o{};
            ADDR2_MIP_INFO mips[16]{};
            if (info(lib, s, o, mips) != ADDR_OK) {
                fail("addrlib (%s) refuses shape %llu", c.name, static_cast<std::uint64_t>(&s - shapes));
                continue;
            }
            const Layout l = layout_of(s);
            if (!l.valid) {
                fail("%s: layout refuses it", s.name);
                continue;
            }
            if (c.rb_plus && !s.is3d && chip_dependent(s.mode, s.elem)) continue;  // Navi1x-only (see chip_dependent)
            if (l.size != o.surfSize || l.slice_size != o.sliceSize)
                fail("%s: size %llu vs addrlib %llu", s.name, l.size, o.surfSize);
            bool reported = false;
            for_each_element(s, [&](unsigned mip, unsigned x, unsigned y, unsigned z) {
                std::uint64_t a = 0;
                const bool ok = addr(lib, s, mip, x, y, z, a);
                if (!ok || a != l.address(mip, x, y, z)) {
                    if (!reported) {
                        fail("%s: first address mismatch", s.name);
                        std::printf("     (%s) at mip %u x %u y %u z %u: addrlib %s%llu, layout %llu\n", c.name, mip, x, y, z, ok ? "" : "FAILED ",
                                    static_cast<unsigned long long>(a), static_cast<unsigned long long>(l.address(mip, x, y, z)));
                    }
                    reported = true;
                }
                ++compared;
            });
            if (s.bc) {
                // The same shape given as BC pixels: identical sizes and level placement.
                ADDR2_COMPUTE_SURFACE_INFO_OUTPUT p{};
                ADDR2_MIP_INFO pm[16]{};
                if (info(lib, s, p, pm, true) != ADDR_OK) {
                    fail("addrlib (%s) refuses the BC pixel form of shape %llu", c.name, static_cast<std::uint64_t>(&s - shapes));
                    continue;
                }
                if (p.surfSize != o.surfSize || p.sliceSize != o.sliceSize) fail("%s: BC pixel form size %llu vs %llu", s.name, p.surfSize, o.surfSize);
                for (unsigned m = 0; m < s.mips; ++m)
                    if (pm[m].offset != mips[m].offset || pm[m].macroBlockOffset != mips[m].macroBlockOffset ||
                        pm[m].mipTailOffset != mips[m].mipTailOffset)
                        fail("%s: BC pixel form level %llu offset %llu differs", s.name, m, pm[m].offset);
            }
        }
        AddrDestroy(lib);
    }
    std::printf("gfx10_layout --addrlib: %zu configurations, %zu shapes, %u element addresses compared, %u chip-dependent D-mode "
                "equations confirmed different on RB+, %d failures\n",
                sizeof configs / sizeof configs[0], sizeof shapes / sizeof shapes[0], compared, chip_dependent_seen, failures);
    return failures ? 1 : 0;
}
}  // namespace
#endif

int main(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "--write-tiled") == 0) return write_tiled_layout(argv[2], argv[3]);
    if (argc >= 2 && std::strncmp(argv[1], "--", 2) == 0) {
#ifdef XBOX2WINDOWS_HAVE_ADDRLIB
        if (argc == 5 && std::strcmp(argv[1], "--write-tiled") == 0 && std::strcmp(argv[4], "--addrlib") == 0)
            return write_tiled_addrlib(argv[2], argv[3]);
        if (std::strcmp(argv[1], "--addrlib") == 0) return addrlib_check();
        if (std::strcmp(argv[1], "--print-equations") == 0) return print_equations();
        if (std::strcmp(argv[1], "--write-golden") == 0 && argc == 3) return write_golden(argv[2]);
        if (std::strcmp(argv[1], "--debug") == 0 && argc == 3) return debug_shape(argv[2]);
        std::puts("usage: gfx10_layout_check <golden> [--mutate floor|tail] | --addrlib | --print-equations | --write-golden <path>");
        return 2;
#else
        std::puts("gfx10_layout: SKIP (built without addrlib; configure with -DXBOX2WINDOWS_ADDRLIB_DIR=<mesa>/src/amd/addrlib)");
        return 77;
#endif
    }
    if (argc == 2) return golden_check(argv[1], Layout::none);
    if (argc == 4 && std::strcmp(argv[2], "--mutate") == 0)
        return golden_check(argv[1], std::strcmp(argv[3], "floor") == 0 ? Layout::floor_mips : Layout::no_tail_coords);
    std::puts("usage: gfx10_layout_check <golden> [--mutate floor|tail] | --addrlib | --print-equations | --write-golden <path>");
    return 2;
}
