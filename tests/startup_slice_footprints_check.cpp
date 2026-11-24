// Checks src/startup_stubs/slice_footprints.h without a GPU: which descriptions the XG stand-in
// accepts, when a CPU-backed texture is read again, what the constructor asks GetCopyableFootprints
// for, and the bytes one placed subresource spans.
#include "startup_stubs/slice_footprints.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
using x2w_startup::SliceFootprints;
int failures, cases;
bool padded_control;

void expect(bool ok, const char* name)
{
    ++cases;
    if (!ok) {
        std::printf("FAIL %s\n", name);
        ++failures;
    }
}

D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, UINT64 width, UINT height, UINT16 layers = 1, UINT16 mips = 1)
{
    return {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, layers, mips, format, {1, 0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
}

// A stand-in device. A COM object starts with a pointer to its method table, and slot 38 is
// GetCopyableFootprints in the declaration order of ID3D12DeviceVtbl in d3d12.h. Every other slot
// ends the run, so a call to another method fails.
constexpr std::size_t slots = 64, footprints_slot = 38;

template <std::size_t N>
void unexpected_slot()
{
    std::printf("FAIL unexpected call to method slot %zu\n", N);
    std::fflush(stdout);
    std::_Exit(1);
}

template <std::size_t... N>
std::array<void*, slots> trap_table(std::index_sequence<N...>)
{
    return {reinterpret_cast<void*>(&unexpected_slot<N>)...};
}

struct FootprintCall {
    int calls;
    D3D12_RESOURCE_DESC desc;
    UINT first, count;
    UINT64 base;
    const void *layouts, *rows, *row_bytes, *total;
};
FootprintCall footprint_call;

void STDMETHODCALLTYPE fake_footprints(ID3D12Device*, const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
                                       D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* row_bytes, UINT64* total)
{
    footprint_call = {footprint_call.calls + 1, *desc, first, count, base, layouts, rows, row_bytes, total};
    for (UINT i = 0; i < count; ++i) {
        layouts[i] = {0x1000u * i, {desc->Format, 100 + i, 50 + i, 1, 256}};
        rows[i] = 50 + i;
        row_bytes[i] = 100 + i;
    }
    *total = 0x12345;
}

struct FakeObject {
    const void* const* table;
};

bool same(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout &&
           a.Flags == b.Flags;
}

// Construct a plan through the fake device: either one request for every plane of every mip, from
// subresource 0 at offset 0 into the plan's own arrays, or no request and no size.
void constructed(const char* name, ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, UINT mips, UINT planes, bool requested)
{
    footprint_call = {};
    const SliceFootprints plan(device, desc, mips);
    ++cases;
    const UINT count = mips * planes;
    const bool ok = plan.planes == planes && plan.count == count &&
                    (requested ? footprint_call.calls == 1 && footprint_call.first == 0 && footprint_call.count == count &&
                                     footprint_call.base == 0 && same(footprint_call.desc, desc) &&
                                     footprint_call.layouts == plan.placed && footprint_call.rows == plan.rows &&
                                     footprint_call.row_bytes == plan.row_bytes && footprint_call.total == &plan.size &&
                                     plan.size == 0x12345 && plan.placed[count - 1].Offset == 0x1000u * (count - 1) &&
                                     plan.rows[count - 1] == 50 + count - 1
                               : footprint_call.calls == 0 && plan.size == UINT64_MAX);
    if (!ok) {
        std::printf("FAIL %s: planes %u count %u size 0x%llx, device asked %d times for %u from %u at %llu (expected %u, %u, %s)\n",
                    name, plan.planes, plan.count, static_cast<unsigned long long>(plan.size), footprint_call.calls,
                    footprint_call.count, footprint_call.first, static_cast<unsigned long long>(footprint_call.base), planes,
                    count, requested ? "asked once" : "not asked");
        ++failures;
    }
}

// The bytes from the first byte of a placed subresource to the end of its last row, set up
// without a device: RowPitch * (Depth * rows - 1) + row_bytes. The control expects a full row pitch
// for the last row as well.
void spans(const char* name, UINT index, UINT pitch, UINT depth, UINT rows, UINT64 row_bytes, UINT64 last_row_end,
           UINT64 last_pitch_end)
{
    const UINT64 expected = padded_control ? last_pitch_end : last_row_end;
    SliceFootprints plan(nullptr, texture(DXGI_FORMAT_R8_UNORM, 1, 1), 1);
    plan.placed[index].Footprint = {DXGI_FORMAT_R8_UNORM, 1, rows, depth, pitch};
    plan.rows[index] = rows;
    plan.row_bytes[index] = row_bytes;
    ++cases;
    if (plan.bytes(index) != expected) {
        std::printf("FAIL %s: %llu bytes, expected %llu\n", name, static_cast<unsigned long long>(plan.bytes(index)),
                    static_cast<unsigned long long>(expected));
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // --mutate padded-last-row: the expectations of a bytes() that counts a full row pitch for the
    // last row (a negative control: the check must then fail).
    padded_control = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "padded-last-row") == 0;
    if (argc > 1 && !padded_control) {
        std::printf("unknown mutation\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // supports(): any format other than NV12, and NV12 only as one even-sized 2D image.
    D3D12_RESOURCE_DESC volume = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 7, 5, 3, 3);
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume.SampleDesc.Count = 4;
    expect(SliceFootprints::supports(volume), "supports an odd 3D RGBA8 volume with mips and samples");
    expect(SliceFootprints::supports(texture(DXGI_FORMAT_R8_UNORM, 17, 3, 6, 5)), "supports an odd R8 array with mips");
    expect(SliceFootprints::supports(texture(DXGI_FORMAT_NV12, 8, 4)), "supports NV12 8x4");
    expect(SliceFootprints::supports(texture(DXGI_FORMAT_NV12, 2, 2)), "supports NV12 2x2");
    expect(SliceFootprints::supports(texture(DXGI_FORMAT_NV12, 3840, 2160)), "supports NV12 3840x2160");
    const D3D12_RESOURCE_DESC nv12 = texture(DXGI_FORMAT_NV12, 8, 4);
    struct Named {
        const char* name;
        D3D12_RESOURCE_DESC desc;
    } refused[] = {{"NV12 width 7", nv12}, {"NV12 height 3", nv12}, {"NV12 width 0", nv12}, {"NV12 height 0", nv12},
                   {"NV12 2 mips", nv12}, {"NV12 0 mips", nv12}, {"NV12 2 layers", nv12}, {"NV12 0 layers", nv12},
                   {"NV12 2 samples", nv12}, {"NV12 3D", nv12}, {"NV12 1D", nv12}, {"NV12 buffer", nv12}};
    refused[0].desc.Width = 7;
    refused[1].desc.Height = 3;
    refused[2].desc.Width = 0;
    refused[3].desc.Height = 0;
    refused[4].desc.MipLevels = 2;
    refused[5].desc.MipLevels = 0;
    refused[6].desc.DepthOrArraySize = 2;
    refused[7].desc.DepthOrArraySize = 0;
    refused[8].desc.SampleDesc.Count = 2;
    refused[9].desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    refused[10].desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    refused[11].desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    for (const auto& item : refused) expect(!SliceFootprints::supports(item.desc), item.name);

    // recheck_cpu_texture(): a single R8 or A8 plane after one frame, anything else after 15, never past 32 MiB.
    constexpr UINT64 cap = 32ull << 20;
    const D3D12_RESOURCE_DESC r8 = texture(DXGI_FORMAT_R8_UNORM, 1920, 1080);
    const D3D12_RESOURCE_DESC a8 = texture(DXGI_FORMAT_A8_UNORM, 2048, 2048);
    const D3D12_RESOURCE_DESC rgba = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080);
    expect(!x2w_startup::recheck_cpu_texture(r8, 4096, 0), "R8 plane: not in the same frame");
    expect(x2w_startup::recheck_cpu_texture(r8, 4096, 1), "R8 plane: after one frame");
    expect(x2w_startup::recheck_cpu_texture(r8, cap, 1), "R8 plane: exactly 32 MiB");
    expect(!x2w_startup::recheck_cpu_texture(r8, cap + 1, 1), "R8 plane: one byte past 32 MiB");
    expect(!x2w_startup::recheck_cpu_texture(r8, cap + 1, 1000), "R8 plane: past 32 MiB, 1000 frames");
    expect(!x2w_startup::recheck_cpu_texture(a8, 4096, 0), "A8 atlas: not in the same frame");
    expect(x2w_startup::recheck_cpu_texture(a8, 4096, 1), "A8 atlas: after one frame");
    expect(x2w_startup::recheck_cpu_texture(a8, cap, 1), "A8 atlas: exactly 32 MiB");
    expect(!x2w_startup::recheck_cpu_texture(a8, cap + 1, 1), "A8 atlas: one byte past 32 MiB");
    const Named others[] = {{"R8 with 2 mips", texture(DXGI_FORMAT_R8_UNORM, 1920, 1080, 1, 2)},
                              {"R8 array of 2", texture(DXGI_FORMAT_R8_UNORM, 1920, 1080, 2, 1)},
                              {"A8 with 2 mips", texture(DXGI_FORMAT_A8_UNORM, 2048, 2048, 1, 2)},
                              {"A8 array of 2", texture(DXGI_FORMAT_A8_UNORM, 2048, 2048, 2, 1)},
                              {"R8G8", texture(DXGI_FORMAT_R8G8_UNORM, 960, 540)}, {"RGBA8", rgba}};
    for (const auto& other : others)
        for (const UINT frames : {1u, 14u, 15u}) {
            char name[64];
            std::snprintf(name, sizeof(name), "%s: %s after %u frames", other.name, frames == 15 ? "read" : "not read", frames);
            expect(x2w_startup::recheck_cpu_texture(other.desc, 4096, frames) == (frames == 15), name);
        }
    expect(x2w_startup::recheck_cpu_texture(rgba, cap, 15), "RGBA8: exactly 32 MiB");
    expect(!x2w_startup::recheck_cpu_texture(rgba, cap + 1, 15), "RGBA8: one byte past 32 MiB");

    // The constructor asks for every plane of every mip, from subresource 0 at offset 0, into its
    // own arrays, and asks nothing for a description or mip count it cannot hold.
    const auto device_table = [] {
        auto table = trap_table(std::make_index_sequence<slots>{});
        table[footprints_slot] = reinterpret_cast<void*>(&fake_footprints);
        return table;
    }();
    FakeObject device_object{device_table.data()};
    auto* device = reinterpret_cast<ID3D12Device*>(&device_object);
    constructed("NV12 8x4", device, nv12, 1, 2, true);
    constructed("R8 17x3", device, texture(DXGI_FORMAT_R8_UNORM, 17, 3), 1, 1, true);
    constructed("RGBA8 16384x16384, 15 mips", device, texture(DXGI_FORMAT_R8G8B8A8_UNORM, 16384, 16384, 1, 15), 15, 1, true);
    constructed("RGBA8 array of 6, 3 mips", device, texture(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 6, 3), 3, 1, true);
    constructed("RGBA8, 16 mips", device, texture(DXGI_FORMAT_R8G8B8A8_UNORM, 32768, 32768, 1, 16), 16, 1, false);
    constructed("RGBA8, 0 mips", device, texture(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64), 0, 1, false);
    constructed("NV12 width 7", device, refused[0].desc, 1, 2, false);
    constructed("NV12 2 layers", device, refused[6].desc, 1, 2, false);
    // XG builds a plan before it checks for a device; a plan that calls through null ends the run here.
    std::printf("plan without a device\n");
    std::fflush(stdout);
    constructed("NV12 8x4 without a device", nullptr, nv12, 1, 2, false);

    // bytes(): the last row ends after its row bytes, not after a full row pitch.
    spans("R8 17x3, pitch 256", 0, 256, 1, 3, 17, 529, 768);
    spans("one row of 64 bytes, pitch 256", 0, 256, 1, 1, 64, 64, 256);
    spans("NV12 chroma (index 1), 4x2 R8G8, pitch 256", 1, 256, 1, 2, 8, 264, 512);
    spans("last index (29), 3 rows of 40 bytes, pitch 512", SliceFootprints::max_mips * 2 - 1, 512, 1, 3, 40, 1064, 1536);
    spans("4 slices of 2 rows of 320 bytes, pitch 512", 0, 512, 4, 2, 320, 3904, 4096);
    spans("16384 rows of 256 KiB (4 GiB)", 0, 262144, 1, 16384, 262144, 0x100000000ull, 0x100000000ull);

    if (failures) {
        std::printf("startup_slice_footprints: %d of %d FAILED\n", failures, cases);
        return 1;
    }
    std::printf("startup_slice_footprints: PASS (%d cases)\n", cases);
    return 0;
}
