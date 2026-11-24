#include "cpu/shim_gpu_device.h"
#include "desktop_gpu.h"
#include "startup_stubs/slice_footprints.h"

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {
constexpr UINT width = 17, height = 3;

unsigned char pixel(UINT frame, UINT row, UINT column)
{
    return static_cast<unsigned char>(17 * frame + 7 * row + column);
}

// Three CPU-backed textures rotate between successive frames. Reusing one after three
// presents must publish its new bytes even without an XG write notification.
void run(ID3D12Device* device, bool stale)
{
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        DXGI_FORMAT_R8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    const x2w_startup::SliceFootprints plan(device, desc, 1);
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3> textures;
    for (auto& texture : textures)
        texture = resource(device, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto upload = make_buffer(device, plan.size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = make_buffer(device, plan.size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RANGE none{};
    UINT wrong = 0;
    for (UINT frame = 0; frame < 6; ++frame) {
        ID3D12Resource* texture = textures[frame % textures.size()].Get();
        ComputeCommands commands(device, nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto* list = commands.list.Get();
        const UINT pitch = plan.placed[0].Footprint.RowPitch;
        if (frame < textures.size() || (!stale && x2w_startup::recheck_cpu_texture(desc, plan.size, 3))) {
            void* mapped = nullptr;
            check(upload->Map(0, &none, &mapped), "Map R8 source");
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                    static_cast<unsigned char*>(mapped)[pitch * y + x] = pixel(frame, y, x);
            upload->Unmap(0, nullptr);
            transition(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            plan.upload(list, texture, upload.Get(), 1, 1, 0, 0, 0);
            transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        transition(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION target{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        target.PlacedFootprint = plan.placed[0];
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        commands.submit_and_wait(device);
        void* observed = nullptr;
        check(readback->Map(0, nullptr, &observed), "Map R8 result");
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const auto actual = static_cast<unsigned char*>(observed)[pitch * y + x];
                const UINT expected_frame = stale ? frame % textures.size() : frame;
                if (actual != pixel(expected_frame, y, x)) throw std::runtime_error("Unexpected R8 pixel");
                wrong += actual != pixel(frame, y, x);
            }
        readback->Unmap(0, &none);
    }
    if (wrong != (stale ? 3 * width * height : 0)) throw std::runtime_error("Unexpected stale pixel count");
    std::printf("stale=%d, six frames, %u of %u pixels differ from the current CPU image\n", stale, wrong, 6 * width * height);

    auto other = desc;
    other.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (x2w_startup::recheck_cpu_texture(desc, plan.size, 0) ||
        !x2w_startup::recheck_cpu_texture(desc, plan.size, 1) ||
        x2w_startup::recheck_cpu_texture(desc, (UINT64{32} << 20) + 1, 1) ||
        x2w_startup::recheck_cpu_texture(other, plan.size, 14) ||
        !x2w_startup::recheck_cpu_texture(other, plan.size, 15))
        throw std::runtime_error("Unexpected CPU texture refresh interval");
}

// A stand-in command list: a COM object starts with a pointer to its method table, and slot 16 is
// CopyTextureRegion in the declaration order of ID3D12GraphicsCommandListVtbl in d3d12.h. Every other
// slot ends the run, so a call to another method fails.
constexpr std::size_t list_slots = 64, copy_slot = 16;

template <std::size_t N>
void unexpected_slot()
{
    std::printf("FAIL unexpected call to command list slot %zu\n", N);
    std::fflush(stdout);
    std::_Exit(1);
}

template <std::size_t... N>
std::array<void*, list_slots> trap_table(std::index_sequence<N...>)
{
    return {reinterpret_cast<void*>(&unexpected_slot<N>)...};
}

struct CopyCall {
    int calls;
    UINT subresource;
    UINT64 offset;
    ID3D12Resource *destination, *source;
};
CopyCall last_copy;

void STDMETHODCALLTYPE fake_copy(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION* to, UINT, UINT, UINT,
                                 const D3D12_TEXTURE_COPY_LOCATION* from, const D3D12_BOX*)
{
    last_copy = {last_copy.calls + 1, to->SubresourceIndex, from->PlacedFootprint.Offset, to->pResource, from->pResource};
}

struct FakeList {
    const void* const* table;
};

int guard_failures;

void guard_expect(bool ok, const char* name)
{
    if (ok) return;
    std::printf("FAIL %s\n", name);
    ++guard_failures;
}

D3D12_RESOURCE_DESC image(DXGI_FORMAT format, UINT64 width, UINT height)
{
    return {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
}

// What D3D12CalcSubresource defines: mip + array slice * mips + plane * mips * array size.
UINT calc_subresource(UINT mip, UINT slice, UINT plane, UINT mips, UINT slices)
{
    return mip + slice * mips + plane * mips * slices;
}

// SliceFootprints::bytes() and upload() need no GPU: a plan built without a device is filled by hand.
// Invalid requests must give 0 bytes and no copy instead of reading past the arrays, wrapping around
// 2^64 or dividing by zero.
void check_footprint_guards()
{
    using x2w_startup::SliceFootprints;
    SliceFootprints plane(nullptr, image(DXGI_FORMAT_R8_UNORM, 17, 3), 1);
    plane.placed[0].Footprint = {DXGI_FORMAT_R8_UNORM, 17, 3, 1, 256};
    plane.rows[0] = 3;
    plane.row_bytes[0] = 17;
    guard_expect(plane.bytes(0) == 256 * 2 + 17, "bytes: one plane ends after the last row, not the last pitch");
    guard_expect(plane.bytes(1) == 0, "bytes: index past count but in the arrays, never filled");
    guard_expect(plane.bytes(SliceFootprints::max_mips * 2) == 0, "bytes: index just past the arrays");
    guard_expect(plane.bytes(1000) == 0 && plane.bytes(UINT_MAX) == 0, "bytes: far index");
    guard_expect(plane.size == UINT64_MAX, "plan without a device has no size");

    SliceFootprints yuv(nullptr, image(DXGI_FORMAT_NV12, 8, 4), 1);
    yuv.placed[0].Footprint = {DXGI_FORMAT_R8_UNORM, 8, 4, 1, 256};
    yuv.rows[0] = 4;
    yuv.row_bytes[0] = 8;
    yuv.placed[1].Footprint = {DXGI_FORMAT_R8G8_UNORM, 4, 2, 1, 256};
    yuv.rows[1] = 2;
    yuv.row_bytes[1] = 8;
    guard_expect(yuv.bytes(0) == 256 * 3 + 8 && yuv.bytes(1) == 256 + 8, "bytes: NV12 luma then chroma");
    guard_expect(yuv.bytes(2) == 0, "bytes: NV12 index past both planes");

    SliceFootprints odd(nullptr, image(DXGI_FORMAT_R8_UNORM, 1, 1), 1);
    odd.placed[0].Footprint = {DXGI_FORMAT_R8_UNORM, 1, 0, 1, 256};
    odd.row_bytes[0] = 17;
    guard_expect(odd.bytes(0) == 0, "bytes: no rows");
    odd.rows[0] = 3;
    odd.placed[0].Footprint.Depth = 0;
    guard_expect(odd.bytes(0) == 0, "bytes: no depth");
    odd.placed[0].Footprint.Depth = 1;
    odd.placed[0].Footprint.RowPitch = 0;
    guard_expect(odd.bytes(0) == 17, "bytes: zero pitch spans one row");
    odd.placed[0].Footprint.RowPitch = 256;
    odd.placed[0].Footprint.Depth = 4;
    guard_expect(odd.bytes(0) == 256 * 11 + 17, "bytes: depth slices of rows");
    odd.rows[0] = UINT_MAX;
    odd.placed[0].Footprint.Depth = UINT_MAX;
    guard_expect(odd.bytes(0) == 0, "bytes: reject an overflowing span");
    const SliceFootprints unset(nullptr, image(DXGI_FORMAT_R8_UNORM, 1, 1), 1);
    guard_expect(unset.bytes(0) == 0, "bytes: nothing filled in");

    auto table = trap_table(std::make_index_sequence<list_slots>{});
    table[copy_slot] = reinterpret_cast<void*>(&fake_copy);
    FakeList list_object{table.data()};
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&list_object);
    auto* texture = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{0x10});
    auto* buffer = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{0x20});

    // Valid requests: NV12 planes, then the second slice of a three-mip array and its last mip.
    yuv.size = 0x1000;
    for (UINT i = 0; i < 2; ++i) {
        last_copy = {};
        yuv.upload(list, texture, buffer, 1, 1, 0, i, 0x300 + i);
        guard_expect(last_copy.calls == 1 && last_copy.subresource == calc_subresource(0, 0, i, 1, 1) &&
                         last_copy.offset == 0x300 + i && last_copy.destination == texture && last_copy.source == buffer,
                     "upload: NV12 plane copies into its own subresource");
    }
    SliceFootprints chain(nullptr, image(DXGI_FORMAT_R8_UNORM, 4, 4), 3);
    chain.size = 0x1000;
    last_copy = {};
    chain.upload(list, texture, buffer, 3, 2, 1, 2, 0x40);
    guard_expect(last_copy.calls == 1 && last_copy.subresource == calc_subresource(2, 1, 0, 3, 2) && last_copy.offset == 0x40,
                 "upload: slice 1 mip 2 of a three-mip, two-slice texture");

    // Refused requests copy nothing, and do not reach the list.
    struct Refused {
        const char* name;
        ID3D12GraphicsCommandList* list;
        ID3D12Resource *texture, *buffer;
        UINT mips, slices, slice, i;
    };
    const Refused refused[] = {
        {"no list", nullptr, texture, buffer, 3, 2, 1, 2},
        {"no texture", list, nullptr, buffer, 3, 2, 1, 2},
        {"no upload buffer", list, texture, nullptr, 3, 2, 1, 2},
        {"zero mips", list, texture, buffer, 0, 2, 1, 2},
        {"16 mips", list, texture, buffer, 16, 2, 1, 2},
        {"zero slices", list, texture, buffer, 3, 0, 0, 2},
        {"slice past the slices", list, texture, buffer, 3, 2, 2, 2},
        {"overflowing subresource", list, texture, buffer, 3, UINT_MAX, UINT_MAX - 1, 2},
        {"subresource past the plan", list, texture, buffer, 3, 2, 1, 3},
        {"subresource far past the plan", list, texture, buffer, 3, 2, 1, UINT_MAX},
    };
    for (const auto& item : refused) {
        last_copy = {};
        chain.upload(item.list, item.texture, item.buffer, item.mips, item.slices, item.slice, item.i, 0x40);
        char name[64];
        std::snprintf(name, sizeof(name), "upload: %s copies nothing", item.name);
        guard_expect(last_copy.calls == 0, name);
    }
    const SliceFootprints without_footprints(nullptr, image(DXGI_FORMAT_R8_UNORM, 4, 4), 3);
    last_copy = {};
    without_footprints.upload(list, texture, buffer, 3, 2, 1, 2, 0x40);
    guard_expect(last_copy.calls == 0, "upload: a plan without footprints copies nothing");

    if (guard_failures) throw std::runtime_error("Slice footprint guards failed");
    std::printf("slice footprint guards: PASS\n");
}
}  // namespace

int main()
{
    try {
        check_footprint_guards();
        auto device = shim_create_device();
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create observation factory");
        check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "Identify test device");
        DXGI_ADAPTER_DESC1 description{};
        check(adapter->GetDesc1(&description), "Read test adapter");
        std::printf("adapter=%ls\n", description.Description);
        run(device.Get(), false);
        run(device.Get(), true);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (...) { return 1; }
    return 0;
}
