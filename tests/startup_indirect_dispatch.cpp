// ExecuteIndirect (list slot 61) through the stand-in with GPU-written Dispatch arguments.
//
//   startup_indirect_dispatch <warp|hardware> <gpu|no-writer|zero|off|huge|huge-product>
//   (huge / huge-product: a first record over the argument guard's bounds, which must not execute)
//   (X2W_D3D12_RENDER=1 X2W_D3D12_PERMISSIVE=1; "off" also X2W_INDIRECT_OFF=1)
//
// Title memory is one private VirtualAlloc block. The argument buffer is a slot-133 placed buffer at
// an address that is NOT 64 KiB-aligned, so the stand-in backs the wrapper with a private committed
// copy ("not aliased"): only the in-place window over the title pages holds what the GPU writes. The
// CPU puts a sentinel record {7, 1, 1} at offset 0 and {5, 1, 1} at offset 0x14 (4-aligned, like the
// title's DrawIndexed/DispatchMesh offsets). One list, recorded through the console slots:
//
//   1. Writer (container compute pipeline, slot 62 + <key>.cs.dxbc in X2W_SHADER_CACHE) stores
//      {3, 2, 1} at offset 0 and {1, 1, 1} at offset 0x14 (mode zero: {0, 1, 1} and {0, 1, 1};
//      mode no-writer: no writer dispatch).
//   2. Counter pipeline bound, root UAV on the counter; ExecuteIndirect(Dispatch signature, 1, args, 0)
//      and ExecuteIndirect(..., args, 0x14): every group adds 1 to the counter.
//
// Expected counter after the fence: gpu 3*2*1 + 1 = 7; zero 0; no-writer 7 + 5 = 12 (the sentinels);
// off 0 (nothing executed). The records in title memory must read what the writer stored (sentinels
// for no-writer). Exit 0 pass, 77 capability skip (no adapter, or the adapter cannot open an
// existing heap over process memory), other values name the failing step.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "startup_indirect_writer.h"
#include "startup_indirect_counter.h"

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
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string mode = argv[2];
    const bool huge = mode == "huge", huge_product = mode == "huge-product";
    if (mode != "gpu" && mode != "no-writer" && mode != "zero" && mode != "off" && !huge && !huge_product) return 2;
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
    if (FAILED(adapter->GetDesc(&adapter_desc))) return 3;
    std::printf("adapter=%ls mode=%s\n", adapter_desc.Description, mode.c_str());

    // Capability: the stand-in's windows need OpenExistingHeapFromAddress on this adapter.
    {
        ComPtr<ID3D12Device3> probe;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe)))) return 4;
        void* block = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ComPtr<ID3D12Heap> heap;
        const HRESULT hr = block ? probe->OpenExistingHeapFromAddress(block, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
        heap.Reset();
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        if (FAILED(hr)) {
            std::printf("OpenExistingHeapFromAddress -> 0x%08lx: capability skip\n", static_cast<unsigned long>(hr));
            return 77;
        }
    }

    // Shader cache with the two container pipelines, keyed as the stand-in keys slot-62 pipelines.
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 5;
    const std::wstring cache = std::wstring(temp) + L"x2w_indirect_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(cache.c_str(), nullptr);
    SetEnvironmentVariableW(L"X2W_SHADER_CACHE", cache.c_str());

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 4;
    const D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_blob, nullptr))) return 5;
    const auto cache_file = [&](const unsigned char* code, std::size_t bytes) {
        wchar_t name[32];
        swprintf(name, 32, L"\\%016llx.cs.dxbc", static_cast<unsigned long long>(fnv(root_blob->GetBufferPointer(), root_blob->GetBufferSize(), fnv(code, bytes))));
        return write_file(cache + name, code, bytes);
    };
    if (!cache_file(startup_indirect_writer, sizeof(startup_indirect_writer)) ||
        !cache_file(startup_indirect_counter, sizeof(startup_indirect_counter)))
        return 5;

    std::array<unsigned char, 64> device_parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), device_parameters.data(), __uuidof(ID3D12Device), &device))) return 6;

    void* root = nullptr;
    if (!ok(invoke(device, 16, 0, arg(root_blob->GetBufferPointer()), root_blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(&root))))
        return 7;
    void *writer = nullptr, *counter = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = static_cast<ID3D12RootSignature*>(root);
    pipeline.CS = {startup_indirect_writer, sizeof(startup_indirect_writer)};
    if (!ok(invoke(device, 62, arg(&pipeline), 0, 0, arg(&__uuidof(ID3D12PipelineState)), arg(&writer)))) return 8;
    pipeline.CS = {startup_indirect_counter, sizeof(startup_indirect_counter)};
    if (!ok(invoke(device, 62, arg(&pipeline), 0, 0, arg(&__uuidof(ID3D12PipelineState)), arg(&counter)))) return 8;

    D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    const D3D12_COMMAND_SIGNATURE_DESC signature_desc{12, 1, &argument, 0};
    void* signature = nullptr;
    if (!ok(invoke(device, 41, arg(&signature_desc), 0, arg(&__uuidof(ID3D12CommandSignature)), arg(&signature)))) return 9;

    // Title memory: arguments at +0x1c20 (not 64 KiB-aligned), counter at +0x10000.
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return 10;
    unsigned char* const args_memory = memory + 0x1c20;
    auto* const counter_word = reinterpret_cast<volatile std::uint32_t*>(memory + 0x10000);
    const std::uint32_t sentinel[2][3] = {{7, 1, 1}, {5, 1, 1}};
    std::memcpy(args_memory, sentinel[0], 12);
    std::memcpy(args_memory + 0x14, sentinel[1], 12);
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 64, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    void* args = nullptr;
    if (!ok(invoke(device, 133, arg(args_memory), arg(&buffer), D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)), arg(&args))))
        return 11;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    void *queue = nullptr, *fence = nullptr, *allocator = nullptr, *list = nullptr;
    if (!ok(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue))) ||
        !ok(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))) ||
        !ok(invoke(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator))) ||
        !ok(invoke(device, 73, arg(list_desc), arg(allocator), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list))))
        return 12;

    // huge: the first record has 70000 groups in x (over the desktop's 65535); huge-product: 512*256*256 =
    // 2^25 groups (each dimension legal, product over the guard's 2^24). The guard must zero the record on
    // the GPU, so only the second record ({1, 1, 1}) runs: counter 1.
    std::uint32_t written[2][4] = {{mode == "zero" ? 0u : 3u, mode == "zero" ? 1u : 2u, 1, 0},
                                   {mode == "zero" ? 0u : 1u, 1, 1, 0x14}};
    if (huge) written[0][0] = 70000, written[0][1] = 1, written[0][2] = 1;
    if (huge_product) written[0][0] = 512, written[0][1] = 256, written[0][2] = 256;
    const std::uint32_t count_at[4] = {0, 0, 0, 0};
    invoke(list, 139, arg(allocator));                                     // Reset
    invoke(list, 33, arg(root));                                           // SetComputeRootSignature
    if (mode != "no-writer") {
        invoke(list, 25, arg(writer));                                     // SetPipelineState
        invoke(list, 45, 0, arg(args_memory));                             // SetComputeRootUnorderedAccessView(0, title address)
        for (const auto& record : written) {
            invoke(list, 39, 1, 4, arg(record), 0);                        // SetComputeRoot32BitConstants(1, 4, record, 0)
            invoke(list, 14, 1, 1, 1);                                     // Dispatch(1, 1, 1)
        }
    }
    invoke(list, 25, arg(counter));
    invoke(list, 45, 0, arg(memory + 0x10000));
    invoke(list, 39, 1, 4, arg(count_at), 0);
    invoke(list, 61, arg(signature), 1, arg(args), 0, 0, 0);               // ExecuteIndirect(sig, 1, args, 0, null, 0)
    invoke(list, 61, arg(signature), 1, arg(args), 0x14, 0, 0);            // ExecuteIndirect(sig, 1, args, 0x14, null, 0)
    invoke(list, 9);                                                       // Close
    void* lists[] = {list};
    invoke(queue, 10, 1, arg(lists));
    if (!ok(invoke(queue, 14, arg(fence), 1))) return 13;
    const auto deadline = GetTickCount64() + 10000;
    while (invoke(fence, 8) < 1) {
        if (GetTickCount64() >= deadline) return 14;
        Sleep(1);
    }

    std::uint32_t records[2][3];
    std::memcpy(records[0], args_memory, 12);
    std::memcpy(records[1], args_memory + 0x14, 12);
    const std::uint32_t got = *counter_word;
    const std::uint32_t wanted = mode == "gpu" ? 7 : mode == "no-writer" ? 12 : huge || huge_product ? 1 : 0;
    std::printf("records in title memory: {%u %u %u} {%u %u %u}\n", records[0][0], records[0][1], records[0][2], records[1][0],
                records[1][1], records[1][2]);
    std::printf("counter: %u (expected %u)\n", got, wanted);
    bool records_ok = true;
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 3; ++i)
            records_ok &= records[r][i] == (mode == "no-writer" ? sentinel[r][i] : written[r][i]);
    if (!records_ok) {
        std::printf("the records in title memory are not what the %s wrote\n", mode == "no-writer" ? "CPU" : "writer");
        return 15;
    }
    ComPtr<ID3D12Device> desktop;
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop))))
        std::printf("device removed reason: 0x%08lx\n", static_cast<unsigned long>(desktop->GetDeviceRemovedReason()));
    return got == wanted ? 0 : 16;
}
