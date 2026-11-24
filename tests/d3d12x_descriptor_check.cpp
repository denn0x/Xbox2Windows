// Source-built XCL-09 fixture for CBV/SRV/UAV and sampler descriptor heaps.
// D3D12Xbox publishes no descriptor-heap creation export: the fixture creates the
// device through d3d12_xs.dll!D3D12XboxCreateDevice (XCL-08), then uses the desktop
// ID3D12Device descriptor-heap methods on that device.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "d3d12x_descriptor_cs.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace {

using XboxCreateDevice = HRESULT(WINAPI*)(IUnknown*, const void*, REFIID, void**);

constexpr UINT staging_view_capacity = 16;
constexpr UINT visible_view_capacity = 16;
constexpr UINT staging_sampler_capacity = 8;
constexpr UINT visible_sampler_capacity = 8;

constexpr UINT staging_cbv_slot = 2;
constexpr UINT staging_srv_slot = 6;
constexpr UINT staging_uav_slot = 11;
constexpr UINT visible_cbv_slot = 5;
constexpr UINT visible_srv_slot = 9;
constexpr UINT visible_uav_slot = 13;
constexpr UINT staging_sampler_slot = 1;
constexpr UINT visible_sampler_slot = 3;

struct AdapterChoice {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
};

const char* hresult_name(HRESULT hr)
{
    if (hr == S_OK) return "S_OK";
    if (hr == E_INVALIDARG) return "E_INVALIDARG";
    if (hr == E_POINTER) return "E_POINTER";
    if (hr == E_HANDLE) return "E_HANDLE";
    if (hr == E_NOINTERFACE) return "E_NOINTERFACE";
    return "unexpected";
}

void check(HRESULT hr, const char* stage)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s failed: %s (0x%08lx)\n", stage, hresult_name(hr),
                     static_cast<unsigned long>(hr));
        throw std::runtime_error(stage);
    }
}

bool contains_ignoring_case(const wchar_t* text, const wchar_t* part)
{
    const std::size_t length = std::wcslen(part);
    for (; *text; ++text) {
        if (_wcsnicmp(text, part, length) == 0) return true;
    }
    return false;
}

int select_adapter(bool warp, AdapterChoice& choice)
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&choice.factory));
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateDXGIFactory2 failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    if (warp) {
        hr = choice.factory->EnumWarpAdapter(IID_PPV_ARGS(&choice.adapter));
        if (FAILED(hr) || !choice.adapter) {
            std::fprintf(stderr, "WARP adapter unavailable: 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        hr = choice.adapter->GetDesc1(&choice.desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "WARP adapter description failed: 0x%08lx\n",
                         static_cast<unsigned long>(hr));
            return 1;
        }
        return 0;
    }

    wchar_t wanted[128]{};
    const DWORD filter = GetEnvironmentVariableW(L"XBOX2WINDOWS_ADAPTER", wanted, 128);
    if (filter >= 128) {
        std::fputs("XBOX2WINDOWS_ADAPTER is longer than 127 characters\n", stderr);
        return 1;
    }
    for (UINT index = 0;; ++index) {
        hr = choice.factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(choice.adapter.ReleaseAndGetAddressOf()));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            if (filter) {
                std::fprintf(stderr, "No D3D12 hardware adapter matches XBOX2WINDOWS_ADAPTER=%ls\n", wanted);
                return 1;
            }
            std::fputs("No physical D3D12 adapter available\n", stderr);
            return 77;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "EnumAdapterByGpuPreference failed: 0x%08lx\n",
                         static_cast<unsigned long>(hr));
            return 1;
        }
        hr = choice.adapter->GetDesc1(&choice.desc);
        if (FAILED(hr)) {
            std::fprintf(stderr, "GetDesc1 failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return 1;
        }
        if (choice.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (filter && !contains_ignoring_case(choice.desc.Description, wanted)) {
            std::fprintf(stderr, "XBOX2WINDOWS_ADAPTER: skipping %ls\n", choice.desc.Description);
            continue;
        }
        return 0;
    }
}

bool enable_debug_layer()
{
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || !debug) return false;
    debug->EnableDebugLayer();
    return true;
}

void print_environment(const AdapterChoice& choice, bool warp, bool debug)
{
    std::printf("backend=%s adapter=%ls vendor=%04x device=%04x\n", warp ? "warp" : "hardware",
                choice.desc.Description, choice.desc.VendorId, choice.desc.DeviceId);
    LARGE_INTEGER driver{};
    if (SUCCEEDED(choice.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
        std::printf("driver=%u.%u.%u.%u\n", HIWORD(driver.HighPart), LOWORD(driver.HighPart),
                    HIWORD(driver.LowPart), LOWORD(driver.LowPart));
    } else {
        std::puts("driver=unavailable via DXGI");
    }
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtl_get_version && rtl_get_version(&os) == 0) {
        std::printf("windows=%lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    } else {
        std::puts("windows=unavailable");
    }
    std::printf("debug_layer=%s\n", debug ? "enabled" : "unavailable");
}

HMODULE load_shim(const char* path, XboxCreateDevice& create)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (length <= 0) {
        std::fputs("Cannot convert the d3d12_xs.dll path\n", stderr);
        return nullptr;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), length)) {
        std::fputs("Cannot convert the d3d12_xs.dll path\n", stderr);
        return nullptr;
    }
    HMODULE module = LoadLibraryExW(wide.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        std::fprintf(stderr, "LoadLibraryExW(%ls) failed: %lu\n", wide.c_str(),
                     static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }
    create = reinterpret_cast<XboxCreateDevice>(GetProcAddress(module, "D3D12XboxCreateDevice"));
    if (!create) {
        std::fputs("d3d12_xs.dll does not export D3D12XboxCreateDevice\n", stderr);
        FreeLibrary(module);
        return nullptr;
    }
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(create), &owner) ||
        owner != module) {
        std::fputs("D3D12XboxCreateDevice does not resolve inside d3d12_xs.dll\n", stderr);
        FreeLibrary(module);
        return nullptr;
    }
    return module;
}

ComPtr<ID3D12Device> create_shim_device(XboxCreateDevice create, IUnknown* adapter, const LUID& expected_luid)
{
    void* raw = nullptr;
    const HRESULT hr = create(adapter, nullptr, __uuidof(ID3D12Device), &raw);
    if (hr != S_OK || !raw) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return {};
    }
    ComPtr<ID3D12Device> device;
    device.Attach(static_cast<ID3D12Device*>(raw));
    const LUID actual = device->GetAdapterLuid();
    if (actual.LowPart != expected_luid.LowPart || actual.HighPart != expected_luid.HighPart ||
        device->GetNodeCount() != 1) {
        std::fputs("D3D12XboxCreateDevice returned the wrong adapter or node count\n", stderr);
        return {};
    }
    return device;
}

struct Heap {
    const char* name = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    UINT capacity = 0;
    UINT increment = 0;
    ComPtr<ID3D12DescriptorHeap> object;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
};

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(ID3D12DescriptorHeap* heap, UINT index, UINT increment)
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T{index} * increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(ID3D12DescriptorHeap* heap, UINT index, UINT increment)
{
    auto handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64{index} * increment;
    return handle;
}

Heap create_heap(ID3D12Device* device, const char* name, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity,
                 D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = flags;
    desc.NodeMask = 0;
    Heap heap;
    heap.name = name;
    heap.type = type;
    heap.flags = flags;
    heap.capacity = capacity;
    check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.object)), name);
    const D3D12_DESCRIPTOR_HEAP_DESC actual = heap.object->GetDesc();
    // The in-box runtime reports NodeMask 1 for a heap requested with 0 on a single-node adapter.
    if (actual.Type != type || actual.NumDescriptors != capacity || actual.Flags != flags ||
        (actual.NodeMask != 0 && actual.NodeMask != 1)) {
        throw std::runtime_error("descriptor heap description changed");
    }
    heap.increment = device->GetDescriptorHandleIncrementSize(type);
    heap.cpu_start = heap.object->GetCPUDescriptorHandleForHeapStart();
    heap.gpu_start = heap.object->GetGPUDescriptorHandleForHeapStart();
    std::printf("heap=%s type=%u flags=0x%x capacity=%u increment=%u cpu_start=0x%llx gpu_start=0x%llx\n",
                name, static_cast<unsigned>(type), static_cast<unsigned>(flags), capacity, heap.increment,
                static_cast<unsigned long long>(heap.cpu_start.ptr),
                static_cast<unsigned long long>(heap.gpu_start.ptr));
    if (heap.increment == 0 || heap.cpu_start.ptr == 0) {
        throw std::runtime_error("descriptor heap has no CPU start or increment");
    }
    if ((flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && heap.gpu_start.ptr == 0) {
        throw std::runtime_error("shader-visible descriptor heap has no GPU start");
    }
    return heap;
}

void check_increment(const Heap& heap)
{
    const auto first = cpu_slot(heap.object.Get(), 0, heap.increment);
    const auto second = cpu_slot(heap.object.Get(), 1, heap.increment);
    if (second.ptr - first.ptr != heap.increment) {
        throw std::runtime_error("CPU descriptor increment is not linear");
    }
    if ((heap.flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        const auto gpu_first = gpu_slot(heap.object.Get(), 0, heap.increment);
        const auto gpu_second = gpu_slot(heap.object.Get(), 1, heap.increment);
        if (gpu_second.ptr - gpu_first.ptr != heap.increment) {
            throw std::runtime_error("GPU descriptor increment is not linear");
        }
    }
}

const char* range_name(D3D12_DESCRIPTOR_RANGE_TYPE type)
{
    switch (type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return "CBV";
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return "SRV";
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return "UAV";
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: return "SAMPLER";
    default: return "UNKNOWN";
    }
}

struct TableRecord {
    const char* name = nullptr;
    D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ID3D12DescriptorHeap* source = nullptr;
    UINT source_index = 0;
    UINT source_capacity = 0;
    ID3D12DescriptorHeap* destination = nullptr;
    UINT destination_index = 0;
    UINT destination_capacity = 0;
    UINT count = 0;
    UINT descriptor_size = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE source_cpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE destination_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE destination_gpu{};
};

HRESULT validate_record(const TableRecord& record, ID3D12Device* device, ID3D12Device* expected_device)
{
    if (!device) return E_POINTER;
    if (device != expected_device) return E_HANDLE;
    if (!record.source || !record.destination) return E_POINTER;
    if (record.source->GetDesc().Type != record.heap_type ||
        record.destination->GetDesc().Type != record.heap_type) {
        return E_INVALIDARG;
    }
    if (record.count == 0) return E_INVALIDARG;
    if (record.descriptor_size == 0 ||
        record.descriptor_size != device->GetDescriptorHandleIncrementSize(record.heap_type)) {
        return E_INVALIDARG;
    }
    if (record.source_index > record.source_capacity ||
        record.count > record.source_capacity - record.source_index ||
        record.destination_index > record.destination_capacity ||
        record.count > record.destination_capacity - record.destination_index) {
        return E_INVALIDARG;
    }
    switch (record.range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
        return record.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? S_OK : E_INVALIDARG;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
        return record.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? S_OK : E_INVALIDARG;
    default:
        return E_INVALIDARG;
    }
}

TableRecord make_record(const char* name, D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                        const Heap& source, UINT source_index,
                        const Heap& destination, UINT destination_index,
                        UINT count, UINT descriptor_size)
{
    TableRecord record;
    record.name = name;
    record.range_type = range_type;
    record.heap_type = destination.type;
    record.source = source.object.Get();
    record.source_index = source_index;
    record.source_capacity = source.capacity;
    record.destination = destination.object.Get();
    record.destination_index = destination_index;
    record.destination_capacity = destination.capacity;
    record.count = count;
    record.descriptor_size = descriptor_size;
    record.source_cpu = cpu_slot(record.source, source_index, descriptor_size);
    record.destination_cpu = cpu_slot(record.destination, destination_index, descriptor_size);
    record.destination_gpu = gpu_slot(record.destination, destination_index, descriptor_size);
    return record;
}

void verify_record_handles(const TableRecord& record)
{
    const auto source_cpu = cpu_slot(record.source, record.source_index, record.descriptor_size);
    const auto destination_cpu = cpu_slot(record.destination, record.destination_index, record.descriptor_size);
    const auto destination_gpu = gpu_slot(record.destination, record.destination_index, record.descriptor_size);
    if (source_cpu.ptr != record.source_cpu.ptr || destination_cpu.ptr != record.destination_cpu.ptr ||
        destination_gpu.ptr != record.destination_gpu.ptr) {
        throw std::runtime_error("descriptor table record handle does not match its slot");
    }
    if (record.source_index + 1 < record.source_capacity) {
        const auto next = cpu_slot(record.source, record.source_index + 1, record.descriptor_size);
        if (next.ptr - source_cpu.ptr != record.descriptor_size) {
            throw std::runtime_error("source table record does not advance by DescriptorSizeInBytes");
        }
    }
    if (record.destination_index + 1 < record.destination_capacity) {
        const auto next_cpu = cpu_slot(record.destination, record.destination_index + 1, record.descriptor_size);
        const auto next_gpu = gpu_slot(record.destination, record.destination_index + 1, record.descriptor_size);
        if (next_cpu.ptr - destination_cpu.ptr != record.descriptor_size ||
            next_gpu.ptr - destination_gpu.ptr != record.descriptor_size) {
            throw std::runtime_error("destination table record does not advance by DescriptorSizeInBytes");
        }
    }
}

void print_record(const TableRecord& record)
{
    std::printf("record=%s range=%s source=%u destination=%u source_cpu=0x%llx destination_cpu=0x%llx "
                "destination_gpu=0x%llx size=%u\n",
                record.name, range_name(record.range_type), record.source_index, record.destination_index,
                static_cast<unsigned long long>(record.source_cpu.ptr),
                static_cast<unsigned long long>(record.destination_cpu.ptr),
                static_cast<unsigned long long>(record.destination_gpu.ptr), record.descriptor_size);
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, UINT64 length, D3D12_HEAP_TYPE heap_type,
                                     D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = length;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap_type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)),
          "Create descriptor fixture buffer");
    return resource;
}

ComPtr<ID3D12Resource> create_texture(ID3D12Device* device)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
          "Create descriptor fixture texture");
    return resource;
}

void write_upload(ID3D12Resource* resource, const void* data, std::size_t size)
{
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(resource->Map(0, &no_read, &mapped), "Map descriptor fixture upload");
    std::memcpy(mapped, data, size);
    const D3D12_RANGE written{0, size};
    resource->Unmap(0, &written);
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> upload_texture(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                                      ID3D12Resource* texture, const std::array<float, 4>& texel,
                                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total_bytes);
    auto upload = create_buffer(device, total_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(upload->Map(0, &no_read, &mapped), "Map descriptor fixture texture upload");
    std::memset(mapped, 0, static_cast<std::size_t>(total_bytes));
    std::memcpy(static_cast<std::uint8_t*>(mapped) + footprint.Offset, texel.data(), sizeof(texel));
    const D3D12_RANGE written{0, static_cast<SIZE_T>(total_bytes)};
    upload->Unmap(0, &written);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::printf("texture_upload rows=%u row_size=%llu total=%llu row_pitch=%u\n", rows,
                static_cast<unsigned long long>(row_size), static_cast<unsigned long long>(total_bytes),
                footprint.Footprint.RowPitch);
    return upload;
}

ComPtr<ID3D12RootSignature> create_root_signature(ID3D12Device* device)
{
    const D3D12_DESCRIPTOR_RANGE_TYPE range_types[4]{
        D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER};
    D3D12_DESCRIPTOR_RANGE1 ranges[4]{};
    for (UINT i = 0; i < 4; ++i) {
        ranges[i].RangeType = range_types[i];
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = 0;
        ranges[i].RegisterSpace = 0;
        ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER1 parameters[4]{};
    for (UINT i = 0; i < 4; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 4;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.NumStaticSamplers = 0;
    desc.Desc_1_1.pStaticSamplers = nullptr;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT serialize = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(serialize)) {
        if (error) {
            std::fprintf(stderr, "Root signature error: %.*s\n", static_cast<int>(error->GetBufferSize()),
                         static_cast<const char*>(error->GetBufferPointer()));
        }
        check(serialize, "Serialize descriptor table root signature");
    }
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)),
          "Create descriptor table root signature");
    return root;
}

ComPtr<ID3D12PipelineState> create_pipeline(ID3D12Device* device, ID3D12RootSignature* root)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {d3d12x_descriptor_cs, sizeof(d3d12x_descriptor_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)),
          "Create descriptor table compute pipeline");
    return pipeline;
}

void print_options19(ID3D12Device* device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options{};
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options, sizeof(options));
    if (SUCCEEDED(hr)) {
        std::printf("options19 max_view_descriptor_heap=%u max_sampler_descriptor_heap=%u\n",
                    options.MaxViewDescriptorHeapSize, options.MaxSamplerDescriptorHeapSize);
    } else {
        std::printf("options19=unavailable (0x%08lx)\n", static_cast<unsigned long>(hr));
    }
}

bool run_positive(ID3D12Device* device, bool bind_table)
{
    print_options19(device);
    const Heap staging_views = create_heap(device, "staging_views", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                           staging_view_capacity, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const Heap visible_views = create_heap(device, "visible_views", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                           visible_view_capacity, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    const Heap staging_samplers = create_heap(device, "staging_samplers", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                              staging_sampler_capacity, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const Heap visible_samplers = create_heap(device, "visible_samplers", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                              visible_sampler_capacity, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    check_increment(staging_views);
    check_increment(visible_views);
    check_increment(staging_samplers);
    check_increment(visible_samplers);

    const std::array<float, 4> factor{0.5f, 0.25f, 0.125f, 1.0f};
    const std::array<float, 4> texel{0.25f, 0.5f, 0.75f, 1.0f};
    std::array<std::uint8_t, 256> constant_bytes{};
    std::memcpy(constant_bytes.data(), factor.data(), sizeof(factor));
    auto constants = create_buffer(device, constant_bytes.size(), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    write_upload(constants.Get(), constant_bytes.data(), constant_bytes.size());
    auto texture = create_texture(device);
    auto output = create_buffer(device, 16, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = create_buffer(device, 16, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = constants->GetGPUVirtualAddress();
    cbv.SizeInBytes = static_cast<UINT>(constant_bytes.size());
    device->CreateConstantBufferView(&cbv, cpu_slot(staging_views.object.Get(), staging_cbv_slot,
                                                    staging_views.increment));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.Get(), &srv,
                                     cpu_slot(staging_views.object.Get(), staging_srv_slot,
                                              staging_views.increment));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1;
    uav.Buffer.StructureByteStride = 16;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav,
                                      cpu_slot(staging_views.object.Get(), staging_uav_slot,
                                               staging_views.increment));

    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = 0.0f;
    sampler.BorderColor[2] = sampler.BorderColor[3] = 0.0f;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, cpu_slot(staging_samplers.object.Get(), staging_sampler_slot,
                                             staging_samplers.increment));

    TableRecord records[4]{
        make_record("cbv", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, staging_views, staging_cbv_slot,
                    visible_views, visible_cbv_slot, 1, staging_views.increment),
        make_record("srv", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, staging_views, staging_srv_slot,
                    visible_views, visible_srv_slot, 1, staging_views.increment),
        make_record("uav", D3D12_DESCRIPTOR_RANGE_TYPE_UAV, staging_views, staging_uav_slot,
                    visible_views, visible_uav_slot, 1, staging_views.increment),
        make_record("sampler", D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, staging_samplers, staging_sampler_slot,
                    visible_samplers, visible_sampler_slot, 1, staging_samplers.increment),
    };
    for (const auto& record : records) {
        check(validate_record(record, device, device), "Validate positive table record");
        verify_record_handles(record);
        print_record(record);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE destination_ranges[2]{records[0].destination_cpu, records[1].destination_cpu};
    const UINT destination_sizes[2]{1, 1};
    D3D12_CPU_DESCRIPTOR_HANDLE source_ranges[2]{records[0].source_cpu, records[1].source_cpu};
    const UINT source_sizes[2]{1, 1};
    device->CopyDescriptors(2, destination_ranges, destination_sizes, 2, source_ranges, source_sizes,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CopyDescriptorsSimple(1, records[2].destination_cpu, records[2].source_cpu,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CopyDescriptorsSimple(1, records[3].destination_cpu, records[3].source_cpu,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    check(device->GetDeviceRemovedReason(), "Descriptor copies");
    std::printf("descriptor_copies=4 ranges=2 simple=2\n");
    if (!bind_table) {
        std::puts("descriptor_table_bound=no layout_only");
        return true;
    }

    auto root = create_root_signature(device);
    auto pipeline = create_pipeline(device, root.Get());
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Create descriptor fixture queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)),
          "Create descriptor fixture allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), pipeline.Get(),
                                    IID_PPV_ARGS(&list)),
          "Create descriptor fixture command list");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    auto texture_upload = upload_texture(device, list.Get(), texture.Get(), texel, footprint);
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[]{visible_views.object.Get(), visible_samplers.object.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootDescriptorTable(0, records[0].destination_gpu);
    list->SetComputeRootDescriptorTable(1, records[1].destination_gpu);
    list->SetComputeRootDescriptorTable(2, records[2].destination_gpu);
    list->SetComputeRootDescriptorTable(3, records[3].destination_gpu);
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 16);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    check(list->Close(), "Close descriptor fixture list");

    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create descriptor fixture fence");
    ID3D12CommandList* submitted[]{list.Get()};
    queue->ExecuteCommandLists(1, submitted);
    check(queue->Signal(fence.Get(), 1), "Signal descriptor fixture fence");
    check(fence->SetEventOnCompletion(1, nullptr), "Wait for descriptor fixture fence");
    check(device->GetDeviceRemovedReason(), "Descriptor fixture submission");

    std::array<float, 4> actual{};
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, sizeof(actual)};
    check(readback->Map(0, &read_range, &mapped), "Map descriptor fixture readback");
    std::memcpy(actual.data(), mapped, sizeof(actual));
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);

    std::array<float, 4> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = texel[i] * factor[i];
    bool matched = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (std::fabs(actual[i] - expected[i]) > 1.0e-6f) matched = false;
    }
    std::printf("dispatch expected=%.6f,%.6f,%.6f,%.6f actual=%.6f,%.6f,%.6f,%.6f\n",
                expected[0], expected[1], expected[2], expected[3],
                actual[0], actual[1], actual[2], actual[3]);
    if (!matched) throw std::runtime_error("descriptor table readback differs from the copied records");
    std::puts("descriptor_table_bound=yes readback=matched");
    return true;
}

ComPtr<IDXGIAdapter1> other_backend_adapter(const AdapterChoice& choice, bool warp)
{
    ComPtr<IDXGIAdapter1> adapter;
    if (!warp) {
        choice.factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        return adapter;
    }
    for (UINT index = 0;; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        const HRESULT hr = choice.factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) return {};
        if (FAILED(hr) || FAILED(adapter->GetDesc1(&desc))) return {};
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) return adapter;
        adapter.Reset();
    }
}

bool run_negative(ID3D12Device* device, const AdapterChoice& choice, bool warp)
{
    const Heap views = create_heap(device, "negative_views", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const Heap samplers = create_heap(device, "negative_samplers", D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const TableRecord base = make_record("cbv", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, views, 0, views, 1, 1,
                                         views.increment);
    bool ok = true;
    auto expect = [&ok](const char* name, HRESULT actual, HRESULT expected) {
        std::printf("negative %s=%s (0x%08lx) expected=%s\n", name, hresult_name(actual),
                    static_cast<unsigned long>(actual), hresult_name(expected));
        if (actual != expected) ok = false;
    };

    TableRecord invalid_count = base;
    invalid_count.count = 0;
    expect("invalid_count", validate_record(invalid_count, device, device), E_INVALIDARG);

    TableRecord invalid_size = base;
    invalid_size.descriptor_size = 0;
    expect("invalid_size", validate_record(invalid_size, device, device), E_INVALIDARG);

    const TableRecord sampler_cbv = make_record("cbv_in_sampler", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, samplers,
                                                0, samplers, 1, 1, samplers.increment);
    expect("sampler_cbv", validate_record(sampler_cbv, device, device), E_INVALIDARG);

    TableRecord unknown_range = base;
    unknown_range.range_type = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(99);
    expect("unknown_range", validate_record(unknown_range, device, device), E_INVALIDARG);

    expect("null_device", validate_record(base, nullptr, device), E_POINTER);

    ComPtr<IDXGIAdapter1> foreign_adapter = other_backend_adapter(choice, warp);
    ComPtr<ID3D12Device> foreign_owner;
    if (foreign_adapter) {
        D3D12CreateDevice(foreign_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&foreign_owner));
    }
    int sentinel = 0;
    ID3D12Device* foreign = foreign_owner.Get();
    if (!foreign) foreign = reinterpret_cast<ID3D12Device*>(&sentinel);
    std::printf("foreign_device=%s\n", foreign_owner ? "desktop-device" : "sentinel-address");
    expect("foreign_device", validate_record(base, foreign, device), E_HANDLE);

    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(99);
    range.NumDescriptors = 1;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 1;
    desc.Desc_1_1.pParameters = &parameter;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob;
    const HRESULT desktop_range_hr = D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr);
    expect("desktop_unknown_range", desktop_range_hr, E_INVALIDARG);
    if (blob) {
        std::fputs("Unknown descriptor range type produced a blob\n", stderr);
        ok = false;
    }
    std::printf("negative controls=7\n");
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <path-to-d3d12_xs.dll> <heap|table|negative> <warp|hardware>\n",
                     argv[0]);
        return 2;
    }
    const std::string_view scenario = argv[2];
    const std::string_view backend = argv[3];
    if ((scenario != "heap" && scenario != "table" && scenario != "negative") ||
        (backend != "warp" && backend != "hardware")) {
        std::fputs("unknown scenario or backend\n", stderr);
        return 2;
    }
    const bool warp = backend == "warp";

    XboxCreateDevice create = nullptr;
    HMODULE shim = load_shim(argv[1], create);
    if (!shim) return 1;

    AdapterChoice choice;
    const int selected = select_adapter(warp, choice);
    if (selected) {
        FreeLibrary(shim);
        return selected;
    }
    const bool debug = enable_debug_layer();
    print_environment(choice, warp, debug);

    ComPtr<ID3D12Device> device = create_shim_device(create, choice.adapter.Get(), choice.desc.AdapterLuid);
    int result = 1;
    if (device) {
        try {
            if (scenario == "heap") {
                result = run_positive(device.Get(), false) ? 0 : 1;
            } else if (scenario == "table") {
                result = run_positive(device.Get(), true) ? 0 : 1;
            } else {
                result = run_negative(device.Get(), choice, warp) ? 0 : 1;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "d3d12x_descriptor_check: %s\n", error.what());
            result = 1;
        }
    }
    device.Reset();
    FreeLibrary(shim);
    std::printf("d3d12x_descriptor_check %s %s: %s\n", argv[2], argv[3], result == 0 ? "PASS" : "FAIL");
    return result;
}
