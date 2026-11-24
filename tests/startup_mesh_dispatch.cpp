// DispatchMesh ExecuteIndirect (list slot 61) through a translated mesh pipeline (d3d12_mesh.inl).
//
//   startup_mesh_dispatch <warp|hardware> <gpu|derived|zero|huge|off|missing|depth|words16|words32|wordsraw|
//                                          fmaprecise|fmabad|notargets|rtvuint|smpheap|smpheapswap>
// smpheap / smpheapswap (`smpheap 2048 1 fallback 2047`, PixelSampler): a title sampler heap of 128 raw S# entries
// bound with SetDescriptorHeaps; the pixel stage samples entry 65 over a 2x1 texture (srv 1, placed on title memory:
// texel 0 red, texel 1 blue) at u = 0.375. smpheap: 65 point, 64 linear -> red ff0000ff; smpheapswap (control): 65
// linear, 64 point -> 0.75 red + 0.25 blue (ff4000bf +-3). Both: the "x2w-draw sampler heap:" line has no unbound
// and no unreadable entry.
// fma line: derived has none ("fma unstated"), fmaprecise `fma precise` (drawn), fmabad `fma fused` (refused:
// translation failed), the other modes `fma exact` (needs DoublePrecisionFloatShaderOps). notargets: no
// OMSetRenderTargets; rtvuint: an R32_UINT target: both refused once with "call refused (...)", nothing drawn.
//   (X2W_D3D12_RENDER=1 X2W_D3D12_PERMISSIVE=1 X2W_D3D12_LIST_NOOP=1 X2W_D3D12_DEBUG=1; X2W_MESH=1 except mode off;
//    the test sets X2W_SHADER_CACHE and X2W_TRAP_LOG itself)
//
// A fake slot-57 mesh record: [0] a title root signature, [5]/[6] fake pixel code (null in mode depth), [9]-[12] null,
// [13]/[14] fake mesh code. The test computes the mesh key exactly as the rule says (pipeline_key(a or empty, b = [13],
// root)) and writes <key>.txt (manifest with `stage mesh`), <key>.ms.cso and <key>.ps.cso (dxc-built DXIL of
// startup_mesh_dispatch.hlsl) into the shader cache (mode missing: nothing).
// One list through the console slots: a container compute writer stores the DispatchMesh record {X, 1, 1} in a
// not-aliased placed title buffer (the CPU sentinel there is {1, 1, 1}); then render target (32x32 RGBA8, cleared
// to 0) or depth target, viewport/scissor, view heap with a hardware buffer descriptor (6 vertices in title memory)
// in table parameter 1, root constant 2.0 = colour, the mesh pipeline, ExecuteIndirect(mesh signature, 1, args, 0).
// Topology is never set (0): a draw would be refused there, a mesh call must not be.
//
// Expected: gpu / derived (slot-55 pipeline over the record): X = 2, both triangles in the colour, alpha 1 (rect
// entry value 0); zero: X = 0, nothing drawn; huge: X = 70000 zeroed by indirect's guard, nothing drawn; off
// (X2W_MESH unset): nothing drawn, no x2w-mesh line; missing: nothing drawn, "no translation" for the key; depth:
// the depth-only pipeline (no pixel stage) is created and issued (depth value printed; today's default state has
// depth off, so it is not checked). words16 / words32 / wordsraw (`heap 1` + `heapwords 1`, MeshHeap): no srv line;
// a title buffer view (16_16_FLOAT / 32_32_FLOAT / raw) in heap slot 2, the slot in root constant 0.0; the shader
// reads the view id from the heap, the FORMAT from vdesc word 3 and decodes the positions by it: words16 drawn with
// green 0x1d, words32 with green 0x40, wordsraw (FORMAT 32_FLOAT) nothing; with a view increment other than 32
// (WARP) the call must be skipped ("descriptor increment is not 32", no vdesc buffer) and the mode reports a
// capability skip after checking that. Every mode: no debug-layer error or
// corruption message, device not removed.
// Exit 0 pass, 77 capability skip (no adapter, no existing-heap support, MeshShaderTier 0, depth on WARP), other
// values name the step.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "startup_mesh_heap_ms.h"
#include "startup_mesh_ms.h"
#include "startup_mesh_ps.h"
#include "startup_mesh_smp_ps.h"
#include "startup_mesh_writer.h"

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0, Word e = 0, Word f = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f);
}
bool ok(Word hr) { return SUCCEEDED(static_cast<HRESULT>(hr)); }
std::uint64_t fnv(const void* data, std::size_t bytes, std::uint64_t hash = 1469598103934665603ull)
{
    for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ static_cast<const unsigned char*>(data)[i]) * 1099511628211ull;
    return hash;
}
bool write_file(const std::wstring& path, const void* data, std::size_t bytes)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool written = std::fwrite(data, 1, bytes, f) == bytes;
    return std::fclose(f) == 0 && written;
}
std::wstring hex_key(std::uint64_t key)
{
    wchar_t name[24];
    swprintf(name, 24, L"%016llx", static_cast<unsigned long long>(key));
    return name;
}
// A slot-55 patch record (kind 2 = blend description), as the title passes them.
struct Patch { UINT tag; D3D12_BLEND_DESC blend; };
static_assert(sizeof(Patch) == 0x14c);

// Desktop-side work on the test's own queue: clear the depth target, or read a texture back.
struct Observer {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    bool open(ID3D12Device* device)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    bool run()
    {
        if (const HRESULT hr = list->Close(); FAILED(hr)) {
            std::printf("observer list Close -> 0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++value))) return false;
        const auto deadline = GetTickCount64() + 10000;
        while (fence->GetCompletedValue() < value) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr));
    }
};
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &b);
}
// Copies subresource 0 of `texture` (in COMMON) and returns the first 4 bytes of each requested pixel.
bool read_pixels(ID3D12Device* device, Observer& o, ID3D12Resource* texture, bool depth, const std::vector<std::pair<UINT, UINT>>& at,
                 std::vector<std::uint32_t>& out)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return false;
    if (depth) barrier(o.list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    o.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (depth) barrier(o.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (!o.run()) return false;
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
    for (const auto& p : at) {
        std::uint32_t v = 0;
        std::memcpy(&v, static_cast<const unsigned char*>(mapped) + footprint.Footprint.RowPitch * p.second + 4 * p.first, 4);
        out.push_back(v);
    }
    readback->Unmap(0, nullptr);
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string mode = argv[2];
    if (mode != "gpu" && mode != "derived" && mode != "zero" && mode != "huge" && mode != "off" && mode != "missing" && mode != "depth" &&
        mode != "words16" && mode != "words32" && mode != "wordsraw" && mode != "fmaprecise" && mode != "fmabad" && mode != "notargets" &&
        mode != "rtvuint" && mode != "smpheap" && mode != "smpheapswap")
        return 2;
    // smpheap / smpheapswap: the pixel stage samples title sampler-heap entry 65 (`smpheap 2048 1 fallback 2047`);
    // entry 65 is point and 64 linear (smpheap), or the other way round (smpheapswap, the control).
    const bool smp = mode.compare(0, 7, "smpheap") == 0;
    const bool depth = mode == "depth";
    const bool words = mode.compare(0, 5, "words") == 0;  // heap descriptor + vdesc (`heap 1`, `heapwords 1`)
    UINT view_increment = 0;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter> adapter;
    if (std::strcmp(argv[1], "hardware") == 0) {
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) return 77;
            if (FAILED(hr)) return 3;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) return 3;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                adapter = candidate;
                break;
            }
        }
    } else if (std::strcmp(argv[1], "warp") != 0 || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        return 3;
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    LARGE_INTEGER driver{};
    if (FAILED(adapter->GetDesc(&adapter_desc))) return 3;
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    std::printf("adapter=%ls driver=%u.%u.%u.%u mode=%s\n", adapter_desc.Description, HIWORD(driver.HighPart), LOWORD(driver.HighPart),
                HIWORD(driver.LowPart), LOWORD(driver.LowPart), mode.c_str());

    // Capabilities: mesh shaders (tier 1, shader model 6.5) and existing heaps over process memory (the windows).
    // (A probe device only: the stand-in enables the debug layer before it creates its device.)
    {
        ComPtr<ID3D12Device3> desktop;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 4;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        const HRESULT hr7 = desktop->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        const HRESULT hrm = desktop->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
        std::printf("MeshShaderTier %d (0x%08lx), highest shader model 0x%x (0x%08lx)\n", SUCCEEDED(hr7) ? static_cast<int>(options7.MeshShaderTier) : -1,
                    static_cast<unsigned long>(hr7), SUCCEEDED(hrm) ? static_cast<unsigned>(model.HighestShaderModel) : 0u,
                    static_cast<unsigned long>(hrm));
        if (FAILED(hr7) || options7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1 || FAILED(hrm) || model.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
            std::printf("no mesh shader support: capability skip\n");
            return 77;
        }
        void* block = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ComPtr<ID3D12Heap> heap;
        const HRESULT hr = block ? desktop->OpenExistingHeapFromAddress(block, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
        heap.Reset();
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        if (FAILED(hr)) {
            std::printf("OpenExistingHeapFromAddress -> 0x%08lx: capability skip\n", static_cast<unsigned long>(hr));
            return 77;
        }
        view_increment = desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        std::printf("view increment %u\n", view_increment);
    }
    // `heap 1` draws need the console's 32-byte view slots (d3d12_draw.inl draw_translated); with another increment
    // (WARP) the words modes check that the call is skipped as required and then report a capability skip.
    const bool words_skip = words && view_increment != 32;
    // Measured on WARP 10.0.26100.9818 (2026-11-24): a mesh PSO without a pixel stage removes the device on its first
    // DispatchMesh (with depth off and with depth test/write on; with the debug layer on and off; no debug-layer
    // message before the removal). The same stream with a pixel stage draws, and hardware draws it without one.
    if (depth && std::strcmp(argv[1], "warp") == 0) {
        std::printf("WARP removes the device for a mesh pipeline without a pixel stage: capability skip\n");
        return 77;
    }

    // Shader cache and log of this process.
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 5;
    const std::wstring cache = std::wstring(temp) + L"x2w_mesh_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(cache.c_str(), nullptr);
    SetEnvironmentVariableW(L"X2W_SHADER_CACHE", cache.c_str());
    const std::wstring log_path = cache + L"\\trap.log";
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path.c_str());

    // The writer: a container compute pipeline, keyed as the stand-in keys slot-62 pipelines.
    D3D12_ROOT_PARAMETER writer_params[2]{};
    writer_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    writer_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    writer_params[1].Constants = {1, 0, 4};
    const D3D12_ROOT_SIGNATURE_DESC writer_root_desc{2, writer_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> writer_blob;
    if (FAILED(D3D12SerializeRootSignature(&writer_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &writer_blob, nullptr))) return 5;
    if (!write_file(cache + L"\\" + hex_key(fnv(writer_blob->GetBufferPointer(), writer_blob->GetBufferSize(), fnv(startup_mesh_writer, sizeof(startup_mesh_writer)))) +
                        L".cs.dxbc", startup_mesh_writer, sizeof(startup_mesh_writer)))
        return 5;

    // The title's graphics root signature of the mesh record: [0] constants, [1] a table, [2] constants. Its bytes go
    // into the key; the translated root signature is built from the manifest.
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    D3D12_ROOT_PARAMETER title_params[3]{};
    title_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    title_params[0].Constants = {0, 0, 4};
    title_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    title_params[1].DescriptorTable = {1, &range};
    title_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    title_params[2].Constants = {1, 0, 4};
    const D3D12_ROOT_SIGNATURE_DESC title_root_desc{3, title_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> title_blob;
    if (FAILED(D3D12SerializeRootSignature(&title_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &title_blob, nullptr))) return 5;

    // Fake console code blobs (the stand-in only hashes them).
    static unsigned char mesh_code[256], pixel_code[128];
    for (int i = 0; i < 256; ++i) mesh_code[i] = static_cast<unsigned char>(0xA0 ^ i);
    for (int i = 0; i < 128; ++i) pixel_code[i] = static_cast<unsigned char>(0x5C ^ (i * 7));
    // The key rule (board/msgs/195600_meshrt_mesh-key-rule.md): pipeline_key(a or empty, b = mesh code, root).
    constexpr char revision[] = "x2w-startup-shader-3";
    std::uint64_t key = fnv(revision, sizeof(revision) - 1);
    if (!depth) key = fnv(pixel_code, sizeof(pixel_code), key);
    key = fnv(mesh_code, sizeof(mesh_code), key);
    key = fnv(title_blob->GetBufferPointer(), title_blob->GetBufferSize(), key);
    std::printf("mesh key %016llx\n", static_cast<unsigned long long>(key));
    if (mode != "missing") {
        // words modes: no srv line; the vertices are reached through the title heap slot in entry value 2.
        // The fma line (RUNTIME_NEEDS 8): derived has none (unstated), fmaprecise / fmabad name their rule, the rest exact.
        const std::string fma = mode == "derived" ? "" : mode == "fmaprecise" ? "fma precise\n" : mode == "fmabad" ? "fma fused\n" : "fma exact\n";
        const std::string manifest =
            words ? "x2w-manifest 1\nudsrc 0 rect\nudsrc 1 rootconst 2 0\nudsrc 2 rootconst 0 0\nheap 1\nheapwords 1\n" + fma + "stage mesh\nms 3 3 1\n"
            // smp: srv 1 is the table's second descriptor (the table offset is in bytes: one view increment).
            : smp ? "x2w-manifest 1\nudsrc 0 rect\nudsrc 1 rootconst 2 0\nsrv 0 buf table 1 0\nsrv 1 tex2d table 1 " + std::to_string(view_increment) +
                        "\nsmpheap 2048 1 fallback 2047\n" + fma + "stage mesh\nms 3 3 1\n"
                  : "x2w-manifest 1\nudsrc 0 rect\nudsrc 1 rootconst 2 0\nsrv 0 buf table 1 0\n" + fma + "stage mesh\nms 3 3 1\n";
        const std::wstring base = cache + L"\\" + hex_key(key);
        if (!(words ? write_file(base + L".ms.cso", startup_mesh_heap_ms, sizeof(startup_mesh_heap_ms))
                    : write_file(base + L".ms.cso", startup_mesh_ms, sizeof(startup_mesh_ms))) ||
            (!depth && !(smp ? write_file(base + L".ps.cso", startup_mesh_smp_ps, sizeof(startup_mesh_smp_ps))
                             : write_file(base + L".ps.cso", startup_mesh_ps, sizeof(startup_mesh_ps)))) ||
            !write_file(base + L".txt", manifest.data(), manifest.size()))
            return 5;
    }

    std::array<unsigned char, 64> device_parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), device_parameters.data(), __uuidof(ID3D12Device), &device))) return 6;
    ComPtr<ID3D12Device> desktop;  // the same device object as the stand-in's (one device per adapter)
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 6;

    void *writer_root = nullptr, *title_root = nullptr, *writer = nullptr, *pipeline = nullptr;
    if (!ok(invoke(device, 16, 0, arg(writer_blob->GetBufferPointer()), writer_blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(&writer_root))) ||
        !ok(invoke(device, 16, 0, arg(title_blob->GetBufferPointer()), title_blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(&title_root))))
        return 7;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = static_cast<ID3D12RootSignature*>(writer_root);
    compute.CS = {startup_mesh_writer, sizeof(startup_mesh_writer)};
    if (!ok(invoke(device, 62, arg(&compute), 0, 0, arg(&__uuidof(ID3D12PipelineState)), arg(&writer)))) return 8;
    // The slot-57 mesh record.
    alignas(8) static unsigned char record[0x300];
    Word record_words[16] = {};
    record_words[0] = arg(title_root);
    if (!depth) record_words[5] = arg(pixel_code), record_words[6] = sizeof(pixel_code);
    record_words[13] = arg(mesh_code), record_words[14] = sizeof(mesh_code);
    std::memcpy(record, record_words, sizeof(record_words));
    if (!ok(invoke(device, 57, arg(record), arg(&__uuidof(ID3D12PipelineState)), arg(&pipeline)))) return 8;
    if (mode == "derived") {
        Patch patch{2};
        for (auto& t : patch.blend.RenderTarget)
            t = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                 D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
        void* derived = nullptr;
        if (!ok(invoke(device, 55, arg(pipeline), 1, arg(&patch), arg(&__uuidof(ID3D12PipelineState)), arg(&derived))) || !derived) return 8;
        pipeline = derived;
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
    const D3D12_COMMAND_SIGNATURE_DESC signature_desc{12, 1, &argument, 0};
    void* signature = nullptr;
    if (!ok(invoke(device, 41, arg(&signature_desc), 0, arg(&__uuidof(ID3D12CommandSignature)), arg(&signature)))) return 9;

    // Title memory: arguments at +0x1c20 (not 64 KiB-aligned: a private copy backs the wrapper), vertices at +0x30000.
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return 10;
    unsigned char* const args_memory = memory + 0x1c20;
    const std::uint32_t sentinel[3] = {1, 1, 1};
    std::memcpy(args_memory, sentinel, 12);
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    void* args = nullptr;
    if (!ok(invoke(device, 133, arg(args_memory), arg(&buffer), D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)), arg(&args)))) return 11;
    // Group 0: a triangle over the left half; group 1: over the right half (clip space).
    const float vertices[12] = {-0.9f, -0.9f, -0.9f, 0.9f, -0.1f, 0.0f, 0.1f, 0.0f, 0.9f, 0.9f, 0.9f, -0.9f};
    unsigned char* const vertex_memory = memory + 0x30000;
    std::memcpy(vertex_memory, vertices, sizeof(vertices));
    if (mode == "words16") {  // the same positions as half2 (all exactly representable: +-0.875, +-0.125, 0)
        const float halves[12] = {-0.875f, -0.875f, -0.875f, 0.875f, -0.125f, 0.0f, 0.125f, 0.0f, 0.875f, 0.875f, 0.875f, -0.875f};
        std::uint16_t packed[12];
        for (int i = 0; i < 12; ++i) {
            std::uint32_t b = 0;
            std::memcpy(&b, &halves[i], 4);
            packed[i] = halves[i] == 0.0f ? 0 : static_cast<std::uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xff) - 112) << 10) | ((b >> 13) & 0x3ff));
        }
        std::memcpy(vertex_memory, packed, sizeof(packed));
    }

    // Targets: a desktop texture with a view made through the stand-in (as the title's views are wrapped).
    constexpr UINT size = 32;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> target;
    const struct {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Word backing, bytes;
    } target_heap_desc{{depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, 0, 0};
    void* target_heap = nullptr;
    if (!ok(invoke(device, 14, arg(&target_heap_desc), arg(&__uuidof(ID3D12DescriptorHeap)), arg(&target_heap)))) return 12;
    const Word target_handle = invoke(target_heap, 9);
    Observer observer;
    if (!observer.open(desktop.Get())) return 12;
    if (depth) {
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
        const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT, {1.0f, 0}};
        if (FAILED(desktop->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, &clear,
                                                    IID_PPV_ARGS(&target)))) return 12;
        D3D12_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
        invoke(device, 21, arg(target.Get()), arg(&view), target_handle);
        // Clear to 1.0 on the test's own queue (the stand-in has no depth clear slot in render mode).
        ComPtr<ID3D12DescriptorHeap> own_dsv;
        const D3D12_DESCRIPTOR_HEAP_DESC own_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        if (FAILED(desktop->CreateDescriptorHeap(&own_desc, IID_PPV_ARGS(&own_dsv)))) return 12;
        desktop->CreateDepthStencilView(target.Get(), &view, own_dsv->GetCPUDescriptorHandleForHeapStart());
        barrier(observer.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        observer.list->ClearDepthStencilView(own_dsv->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        barrier(observer.list.Get(), target.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
        if (!observer.run()) return 12;
    } else {
        // rtvuint: an integer target (R32_UINT, 4 bytes per pixel like RGBA8): the call must be refused ("rtv variant").
        const DXGI_FORMAT format = mode == "rtvuint" ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        const D3D12_CLEAR_VALUE clear{format, {0, 0, 0, 0}};
        if (FAILED(desktop->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, &clear,
                                                    IID_PPV_ARGS(&target)))) return 12;
        D3D12_RENDER_TARGET_VIEW_DESC view{format, D3D12_RTV_DIMENSION_TEXTURE2D, {}};
        invoke(device, 20, arg(target.Get()), arg(&view), target_handle);
    }
    // The view heap with one hardware buffer descriptor: base, stride 0, 48 records (bytes), over the vertices.
    const struct {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Word backing, bytes;
    } view_heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE}, 0, 0};
    void* view_heap = nullptr;
    if (!ok(invoke(device, 14, arg(&view_heap_desc), arg(&__uuidof(ID3D12DescriptorHeap)), arg(&view_heap)))) return 12;
    const Word view_cpu = invoke(view_heap, 9), view_gpu = invoke(view_heap, 10);
    if (!view_cpu || !view_gpu) return 12;
    const Word vertex_address = arg(vertex_memory);
    const std::uint32_t descriptor[8] = {static_cast<std::uint32_t>(vertex_address), static_cast<std::uint32_t>(vertex_address >> 32) & 0xffff,
                                         sizeof(vertices), 0};
    std::memcpy(reinterpret_cast<void*>(view_cpu), descriptor, sizeof(descriptor));
    // words modes: a title buffer view in heap slot 2 (the stand-in writes its view token there), over a placed
    // buffer on the vertices: 16_16_FLOAT (words16), 32_32_FLOAT (words32) or raw (wordsraw: no typed format).
    constexpr std::uint32_t heap_slot = 2;
    void* vertex_buffer = nullptr;
    if (words) {
        D3D12_RESOURCE_DESC vb{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_NONE};
        if (!ok(invoke(device, 133, arg(vertex_memory), arg(&vb), D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)), arg(&vertex_buffer))))
            return 12;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = mode == "words16" ? DXGI_FORMAT_R16G16_FLOAT : mode == "words32" ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32_TYPELESS;
        view.Buffer.NumElements = mode == "wordsraw" ? 12 : 6;
        view.Buffer.Flags = mode == "wordsraw" ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        invoke(device, 18, arg(vertex_buffer), arg(&view), view_cpu + 32 * heap_slot);
        std::uint32_t token[2] = {};
        std::memcpy(token, reinterpret_cast<const void*>(view_cpu + 32 * heap_slot), 8);
        std::printf("heap slot %u: %08x %08x (view token X2WV = 56573258 + view id)\n", heap_slot, token[0], token[1]);
    }
    // smp modes: a 2x1 RGBA8 texture placed on title memory (the stand-in backs it with a desktop texture and uploads
    // it from that memory at the draw; texel 0 red, texel 1 blue), its SRV at the table's second descriptor, and a
    // shader-visible title sampler heap of 128 raw 16-byte S# entries: 64 and 65 clamp, one point, one linear.
    void *smp_texture = nullptr, *sampler_heap = nullptr;
    if (smp) {
        const std::uint32_t texels[2] = {0xff0000ffu, 0xffff0000u};
        std::memcpy(memory + 0x40000, texels, sizeof(texels));
        D3D12_RESOURCE_DESC tex{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                D3D12_RESOURCE_FLAG_NONE};
        if (!ok(invoke(device, 133, arg(memory + 0x40000), arg(&tex), D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)),
                       arg(&smp_texture))))
            return 12;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        invoke(device, 18, arg(smp_texture), arg(&view), view_cpu + view_increment);
        constexpr UINT sampler_count = 128;
        static std::uint32_t sampler_words[sampler_count][4];
        const std::uint32_t clamp = 2u | (2u << 3) | (2u << 6);
        const auto filter = [](std::uint32_t f) { return (f << 20) | (f << 22); };  // mag, min; 0 point, 1 linear
        sampler_words[64][0] = sampler_words[65][0] = clamp;
        sampler_words[65][2] = filter(mode == "smpheap" ? 0 : 1);
        sampler_words[64][2] = filter(mode == "smpheap" ? 1 : 0);
        const struct {
            D3D12_DESCRIPTOR_HEAP_DESC desc;
            Word backing, bytes;
        } sampler_heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
                            arg(sampler_words), sizeof(sampler_words)};
        if (!ok(invoke(device, 14, arg(&sampler_heap_desc), arg(&__uuidof(ID3D12DescriptorHeap)), arg(&sampler_heap)))) return 12;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    void *queue = nullptr, *fence = nullptr, *allocator = nullptr, *list = nullptr;
    if (!ok(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue))) ||
        !ok(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))) ||
        !ok(invoke(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator))) ||
        !ok(invoke(device, 73, arg(list_desc), arg(allocator), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list))))
        return 13;

    const std::uint32_t x = mode == "zero" ? 0u : mode == "huge" ? 70000u : 2u;
    const std::uint32_t record_values[4] = {x, 1, 1, 0};
    const std::uint32_t colour = 0x00ff40c0;  // R c0, G 40, B ff
    const float clear_colour[4] = {0, 0, 0, 0};
    invoke(list, 139, arg(allocator));                                     // Reset
    invoke(list, 33, arg(writer_root));                                    // SetComputeRootSignature
    invoke(list, 25, arg(writer));                                         // SetPipelineState (writer)
    invoke(list, 45, 0, arg(args_memory));                                 // SetComputeRootUnorderedAccessView(0, title address)
    invoke(list, 39, 1, 4, arg(record_values), 0);                         // SetComputeRoot32BitConstants(1, 4, record, 0)
    invoke(list, 14, 1, 1, 1);                                             // Dispatch: the writer stores {x, 1, 1}
    if (depth) {
        invoke(list, 50, 0, 0, 0, arg(&target_handle));                    // OMSetRenderTargets(0, null, FALSE, &dsv)
    } else {
        invoke(list, 52, target_handle, arg(clear_colour));                // ClearRenderTargetView
        if (mode != "notargets") invoke(list, 50, 1, arg(&target_handle)); // OMSetRenderTargets(1, &rtv); notargets: none bound
    }
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
    invoke(list, 21, 1, arg(&viewport));
    invoke(list, 22, 1, arg(&scissor));
    invoke(list, 34, arg(title_root));                                     // SetGraphicsRootSignature
    void* const heaps[2] = {view_heap, sampler_heap};
    invoke(list, 32, smp ? 2 : 1, arg(heaps));                             // SetDescriptorHeaps (smp: and the sampler heap)
    invoke(list, 36, 1, view_gpu);                                         // SetGraphicsRootDescriptorTable(1, heap start)
    invoke(list, 40, 2, 1, arg(&colour), 0);                               // SetGraphicsRoot32BitConstants(2, 1, &colour, 0)
    if (words) invoke(list, 40, 0, 1, arg(&heap_slot), 0);                 // SetGraphicsRoot32BitConstants(0, 1, &slot, 0): entry value 2
    invoke(list, 25, arg(pipeline));                                       // SetPipelineState (the mesh pipeline)
    invoke(list, 61, arg(signature), 1, arg(args), 0, 0, 0);               // ExecuteIndirect(mesh signature, 1, args, 0)
    invoke(list, 9);                                                       // Close
    void* lists[] = {list};
    invoke(queue, 10, 1, arg(lists));
    if (!ok(invoke(queue, 14, arg(fence), 1))) return 14;
    const auto deadline = GetTickCount64() + 10000;
    while (invoke(fence, 8) < 1) {
        if (GetTickCount64() >= deadline) return 14;
        Sleep(1);
    }

    std::uint32_t stored[3];
    std::memcpy(stored, args_memory, 12);
    std::printf("record in title memory: {%u %u %u} (writer stored {%u 1 1})\n", stored[0], stored[1], stored[2], x);
    std::vector<std::uint32_t> pixels;
    const std::vector<std::pair<UINT, UINT>> at = {{8, 16}, {24, 16}, {16, 2}};  // left triangle, right triangle, outside both
    if (!read_pixels(desktop.Get(), observer, target.Get(), depth, at, pixels)) {
        ComPtr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(desktop.As(&messages)))
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T bytes = 0;
                messages->GetMessage(i, nullptr, &bytes);
                std::vector<char> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (bytes && SUCCEEDED(messages->GetMessage(i, message, &bytes))) std::printf("debug: %s\n", message->pDescription);
            }
        return 15;
    }
    int failures = 0;
    if (stored[0] != x || stored[1] != 1 || stored[2] != 1) {
        std::printf("the record in title memory is not what the writer stored\n");
        ++failures;
    }
    const bool drawn_expected = mode == "gpu" || mode == "derived" || mode == "fmaprecise" || smp || (!words_skip && (mode == "words16" || mode == "words32"));
    // RGBA8 little-endian: R c0, G 40, B ff, A ff. words modes: G = the FORMAT code in vdesc word 3 (29 = 0x1d, 64 = 0x40).
    // smpheap: entry 65 point -> texel 0 (red ff0000ff); smpheapswap: entry 65 linear at u = 0.375 -> 0.75 red + 0.25
    // blue (R bf, B 40, A ff; filtering precision allows +-3 per channel).
    const std::uint32_t drawn = mode == "words16" ? 0xffff1dc0u : mode == "smpheap" ? 0xff0000ffu : mode == "smpheapswap" ? 0xff4000bfu : 0xffff40c0u;
    const auto matches = [&](std::uint32_t got, std::uint32_t want) {
        if (mode != "smpheapswap" || !want) return got == want;
        for (int s = 0; s < 32; s += 8) {
            const int d = static_cast<int>((got >> s) & 255) - static_cast<int>((want >> s) & 255);
            if (d < -3 || d > 3) return false;
        }
        return true;
    };
    if (depth) {
        float values[3];
        std::memcpy(values, pixels.data(), sizeof(values));
        std::printf("depth: left %g right %g outside %g (cleared 1; today's default state has depth off, so not checked)\n", values[0],
                    values[1], values[2]);
    } else {
        std::printf("pixels: left %08x right %08x outside %08x (drawn = %08x, cleared = 00000000)\n", pixels[0], pixels[1], pixels[2], drawn);
        const std::uint32_t want = drawn_expected ? drawn : 0u;
        if (!matches(pixels[0], want) || !matches(pixels[1], want) || pixels[2] != 0) {
            std::printf("FAIL: expected left and right %08x, outside 00000000\n", want);
            ++failures;
        }
    }

    // The log: the per-key lines and the debug layer.
    std::ifstream log_file(log_path, std::ios::binary);
    const std::string log{std::istreambuf_iterator<char>(log_file), {}};
    char key_text[24];
    std::snprintf(key_text, sizeof(key_text), "%016llx", static_cast<unsigned long long>(key));
    const auto has = [&](const std::string& text) { return log.find(text) != std::string::npos; };
    const bool issued = has(std::string("x2w-mesh pipeline ") + key_text + ": first call issued");
    const bool missing = has(std::string("x2w-mesh pipeline ") + key_text + ": no translation");
    const bool any_mesh = has("x2w-mesh ");
    // The coverage line (board/msgs/210900_mesh): "x2w-mesh key K first bound frame F (cache: ok|fail|missing)".
    std::string bound_line;
    if (const std::size_t p = log.find(std::string("x2w-mesh key ") + key_text + " first bound frame "); p != std::string::npos)
        bound_line = log.substr(p, log.find('\n', p) - p);
    const bool bound_ok = bound_line.find(mode == "missing" ? "(cache: missing)" : mode == "fmabad" ? "(cache: fail)" : "(cache: ok)") != std::string::npos;
    // Refusal modes: the once-per-key reason line (d3d12_mesh.inl mesh_refusal_line / load_mesh_stages).
    const std::string pipeline_line = std::string("x2w-mesh pipeline ") + key_text + ": ";
    const bool refusal_mode = mode == "fmabad" || mode == "notargets" || mode == "rtvuint";
    const std::string reason = mode == "fmabad"      ? "translation failed (unknown fma rule \"fused\")"
                               : mode == "notargets" ? "call refused (no targets: "
                               : mode == "rtvuint"   ? "call refused (rtv variant: target formats 42;"
                                                     : "";
    const bool reason_ok = refusal_mode && has(pipeline_line + reason);
    if (refusal_mode) std::printf("log: refusal line \"%s%s\" %d\n", pipeline_line.c_str(), reason.c_str(), reason_ok);
    // The fma rule each loaded translation reports, and the capability line for exact / unstated.
    const char* fma_rule = mode == "derived" ? "fma unstated" : mode == "fmaprecise" ? "fma precise" : "fma exact";
    const bool fma_checked = mode != "off" && mode != "missing" && mode != "fmabad";
    const bool fma_ok = !fma_checked || (has(std::string(", ") + fma_rule) && (mode == "fmaprecise" || has("x2w-mesh DoublePrecisionFloatShaderOps 1 ")));
    if (fma_checked) std::printf("log: mesh stage line ends \"%s\" and capability line as required: %d\n", fma_rule, fma_ok);
    if (!fma_ok) {
        std::printf("FAIL: the fma lines do not match mode %s\n", mode.c_str());
        ++failures;
    }
    std::printf("log: issued %d, no translation %d, any x2w-mesh line %d, first bound line \"%s\"\n", issued, missing, any_mesh,
                bound_line.c_str());
    const bool skipped_32 = has(std::string("x2w-draw-rt ") + key_text + ": heap draw skipped: descriptor increment is not 32");
    if (words_skip) std::printf("log: heap draw skipped (increment not 32) %d\n", skipped_32);
    if (mode == "off"       ? any_mesh
        : mode == "missing" ? (!missing || issued || !bound_ok)
        : refusal_mode      ? (issued || !bound_ok || !reason_ok)
        : words_skip        ? (issued || !bound_ok || !skipped_32)
                            : (!issued || !bound_ok)) {
        std::printf("FAIL: the x2w-mesh lines do not match mode %s\n", mode.c_str());
        ++failures;
    }
    if (smp) {  // the translated sampler heap was built from the title's 128 entries (bound: none unbound)
        std::string sampler_line;
        if (const std::size_t p = log.find("x2w-draw sampler heap: "); p != std::string::npos) sampler_line = log.substr(p, log.find('\n', p) - p);
        std::printf("log: sampler heap line \"%s\"\n", sampler_line.c_str());
        if (sampler_line.find("null_sampler_unbound=0 ") == std::string::npos || sampler_line.find("null_sampler_unreadable=0 ") == std::string::npos) {
            std::printf("FAIL: the sampler heap was not built from the title's sampler heap\n");
            ++failures;
        }
    }
    if (words) {  // the vdesc root parameter and buffer, and the word the stand-in wrote for the view
        const bool root = has(std::string("x2w-vdesc pipeline ") + key_text + ": heapwords 1, vdesc in root parameter ");
        const bool buffer = has("x2w-vdesc buffer 90000 ids x 32 bytes");
        std::string view_line;
        if (const std::size_t p = log.find("x2w-vdesc view "); p != std::string::npos) view_line = log.substr(p, log.find('\n', p) - p);
        std::printf("log: vdesc root parameter %d, vdesc buffer %d, view words \"%s\"\n", root, buffer, view_line.c_str());
        // Skipped (increment not 32): the root parameter exists, but no buffer is made and no view is filled.
        if (!root || (words_skip ? buffer || !view_line.empty() : !buffer || view_line.empty())) {
            std::printf("FAIL: the x2w-vdesc lines do not match mode %s\n", mode.c_str());
            ++failures;
        }
    }
    for (std::size_t p = log.find("x2w-d3d12-debug"); p != std::string::npos; p = log.find("x2w-d3d12-debug", p + 1)) {
        const std::size_t end = log.find('\n', p);
        const std::string line = log.substr(p, end == std::string::npos ? std::string::npos : end - p);
        std::printf("%s\n", line.c_str());
        if (line.find("sev=0 ") != std::string::npos || line.find("sev=1 ") != std::string::npos) ++failures;
    }
    std::printf("device removed reason: 0x%08lx\n", static_cast<unsigned long>(desktop->GetDeviceRemovedReason()));
    if (desktop->GetDeviceRemovedReason() != S_OK) ++failures;
    if (!failures && words_skip) {
        std::printf("view increment %u is not 32: skipped as required, vdesc not exercised: capability skip\n", view_increment);
        return 77;
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 16 : 0;
}
