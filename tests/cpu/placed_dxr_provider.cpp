// Fixture-only imports. The two records contain guest addresses; the fixed
// triangle and ray observations use the same production resolver as #1326.
#include "../dxr_fixture.h"
#include "../dxr_placed_build.h"
#include "../xmem_test_device.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>

namespace {
using namespace dxr_fixture;
using Sizes = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO;
struct Request { UINT64 dest, scratch; };
struct Input {
    UINT64 base[5]; // BLAS, TLAS, BLAS scratch, TLAS scratch, one-byte-short TLAS scratch
    UINT64 mode;
    Request requests[2]{};
};
static_assert(sizeof(Request) == 16 && offsetof(Input, mode) == 40 && offsetof(Input, requests) == 48);

struct Scene {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    PlacedBuildScene placement;
    std::array<Sizes, 2> sizes{};
    Input input{};
    BuildCounts actual{}, reference{};
    std::ofstream log{"calls.txt"};
};
std::unique_ptr<Scene> scene;

void prepare_sizes(Scene& s) {
    Microsoft::WRL::ComPtr<ID3D12Device5> dxr;
    check(s.device.As(&dxr), "Query DXR device");
    D3D12_RAYTRACING_GEOMETRY_DESC triangle{};
    triangle.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    triangle.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    triangle.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    triangle.Triangles.VertexCount = 3;
    triangle.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.NumDescs = 1;
    input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.pGeometryDescs = &triangle;
    dxr->GetRaytracingAccelerationStructurePrebuildInfo(&input, &s.sizes[0]);
    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    input.InstanceDescs = 0;
    dxr->GetRaytracingAccelerationStructurePrebuildInfo(&input, &s.sizes[1]);
    for (const auto& size : s.sizes) {
        if (!size.ResultDataMaxSizeInBytes || !size.ScratchDataSizeInBytes)
            throw std::runtime_error("No nonzero result/scratch size for the fixture geometry");
    }
}

void record_adapter(Scene& s, bool warp) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto luid = s.device->GetAdapterLuid();
    check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "Resolve actual DXR device LUID");
    DXGI_ADAPTER_DESC1 desc{};
    LARGE_INTEGER driver{};
    check(adapter->GetDesc1(&desc), "Read DXR adapter");
    check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "Read DXR driver");
    if (((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) != warp)
        throw std::runtime_error("Selected DXR device does not match requested backend");
    char name[512]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr))
        throw std::runtime_error("Could not encode adapter name");
    s.log << "device " << name << " luid=" << luid.HighPart << ":" << luid.LowPart
          << " driver=" << HIWORD(driver.HighPart) << "." << LOWORD(driver.HighPart) << "."
          << HIWORD(driver.LowPart) << "." << LOWORD(driver.LowPart) << " software=" << warp << "\n";
}

void write_words(const char* path, const std::vector<UINT32>& words) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(UINT32));
}
}

extern "C" __declspec(dllexport) UINT64 dxr_inputs() try {
    scene = std::make_unique<Scene>();
    auto& s = *scene;
    const char* adapter = std::getenv("X2W_DXR_ADAPTER");
    const bool warp = adapter && std::strcmp(adapter, "warp") == 0;
    bool unmatched = false;
    s.device = create_device(warp, unmatched);
    if (unmatched) return 0;
    if (!s.device || !supported(s.device.Get(), Backend::pipeline)) return 77;
    record_adapter(s, warp);
    prepare_sizes(s);
    s.placement.attach(s.device.Get());
    constexpr auto as = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto& b = s.input.base;
    for (UINT i = 0; i < 2; ++i) {
        b[i] = s.placement.place(i, 512 + s.sizes[i].ResultDataMaxSizeInBytes, as);
        b[i+2] = s.placement.place(i+2, 768 + s.sizes[i].ScratchDataSizeInBytes, uav);
    }
    b[4] = s.placement.place(4, 768 + s.sizes[1].ScratchDataSizeInBytes - 1, uav);
    const char* mode = std::getenv("X2W_DXR_CASE");
    if (mode && std::strcmp(mode, "short") == 0) s.input.mode = 1;
    if (mode && std::strcmp(mode, "unmapped") == 0) s.input.mode = 2;
    return reinterpret_cast<UINT64>(&s.input);
} catch (const std::exception& e) {
    std::fprintf(stderr, "prepare: %s\n", e.what());
    return 0;
}

extern "C" __declspec(dllexport) UINT64 dxr_batch(const Request* requests, UINT count) try {
    if (!scene || !requests || count != 2) return 99;
    auto& s = *scene;
    ResolvedBatch batch{};
    for (UINT i = 0; i < count; ++i) {
        const auto& request = requests[i];
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC output{};
        const HRESULT hr = x2w_startup::resolve_placed_build(s.placement.guest(), request.dest, request.scratch,
                                                              s.sizes[i], output);
        const UINT scratch_slot = i == 1 && s.input.mode == 1 ? 4 : i + 2;
        s.log << "request " << i << " dest_offset=" << (request.dest - s.input.base[i]) << " scratch=";
        if (request.scratch == 0x1000) s.log << "unmapped";
        else s.log << scratch_slot << "+" << (request.scratch - s.input.base[scratch_slot]);
        s.log << "\n";
        if (FAILED(hr)) {
            if (hr != E_INVALIDARG || output.DestAccelerationStructureData || output.ScratchAccelerationStructureData)
                throw std::runtime_error("Rejected descriptor was partially published or returned unexpected HRESULT");
            s.log << "first_rejected=" << i << " error=E_INVALIDARG records=" << s.actual.records
                  << " submissions=" << s.actual.submissions << "\n";
            return i+1;
        }
        batch[i] = {output.DestAccelerationStructureData, output.ScratchAccelerationStructureData, s.sizes[i]};
    }
    const auto placed = trace_placed_triangle(s.device.Get(), &batch, nullptr, &s.actual);
    const auto direct = trace_placed_triangle(s.device.Get(), nullptr, nullptr, &s.reference);
    write_words("placed.bin", placed);
    write_words("direct.bin", direct);
    s.log << "accepted=2 placed_records=" << s.actual.records << " placed_submissions=" << s.actual.submissions
          << " direct_records=" << s.reference.records << " direct_submissions=" << s.reference.submissions << "\n";
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "batch: %s\n", e.what());
    return 99;
}

extern "C" __declspec(dllexport) UINT64 dxr_release() try {
    if (!scene) return 0;
    const unsigned count = scene->placement.release();
    scene->log << "released=" << count << "\n";
    scene.reset();
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "release: %s\n", e.what());
    return 1;
}
