// Source-built XCL-10 fixture for a compute command list recorded through the XCL-08 device.
// D3D12Xbox publishes no command-list creation or recording export. The fixture creates the
// device through d3d12_xs.dll!D3D12XboxCreateDevice and records with the documented desktop
// ID3D12Device::CreateCommandList / ID3D12GraphicsCommandList methods on that device.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "desktop_gpu.h"
#include "d3d12x_compute_list_cs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using XboxCreateDevice = HRESULT(WINAPI*)(IUnknown*, const void*, REFIID, void**);

constexpr UINT word_count = 320;
constexpr UINT first_dispatch_words = 64;
constexpr UINT second_dispatch_offset = 64;
constexpr UINT second_dispatch_words = 192;
constexpr UINT tail_offset = 256;
constexpr UINT tail_words = 64;
constexpr UINT first_addend = 0x01020304u;
constexpr UINT second_addend = 0x0f0e0d0cu;
constexpr UINT clear_value = 0xdeadbeefu;
constexpr UINT64 buffer_bytes = UINT64{word_count} * sizeof(UINT);
constexpr UINT first_groups = 1;
constexpr UINT second_groups = 3;

static_assert(first_dispatch_words == first_groups * 64);
static_assert(second_dispatch_words == second_groups * 64);
static_assert(tail_offset + tail_words == word_count);

struct AdapterChoice {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
};

bool contains_ignoring_case(const wchar_t* text, const wchar_t* part)
{
    const std::size_t length = std::wcslen(part);
    for (; *text; ++text) {
        if (_wcsnicmp(text, part, length) == 0) return true;
    }
    return false;
}

// Same selection convention as the other D3D12 fixtures: WARP on request, otherwise the first
// non-software adapter in high-performance order. XBOX2WINDOWS_ADAPTER filters by description.
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
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(choice.adapter.ReleaseAndGetAddressOf()));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            if (filter) {
                std::fprintf(stderr, "No D3D12 hardware adapter matches XBOX2WINDOWS_ADAPTER=%ls\n",
                             wanted);
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
        std::printf("windows=%lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion,
                    os.dwBuildNumber);
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
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(create), &owner) ||
        owner != module) {
        std::fputs("D3D12XboxCreateDevice does not resolve inside d3d12_xs.dll\n", stderr);
        FreeLibrary(module);
        return nullptr;
    }
    return module;
}

ComPtr<ID3D12Device> create_shim_device(XboxCreateDevice create, IUnknown* adapter,
                                        const LUID& expected_luid)
{
    void* raw = nullptr;
    const HRESULT hr = create(adapter, nullptr, __uuidof(ID3D12Device), &raw);
    if (hr != S_OK || !raw) {
        std::fprintf(stderr, "D3D12XboxCreateDevice failed: 0x%08lx\n",
                     static_cast<unsigned long>(hr));
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

std::string state_name(D3D12_RESOURCE_STATES state)
{
    switch (state) {
    case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
    case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_DEST";
    case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_SOURCE";
    case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return "UNORDERED_ACCESS";
    case D3D12_RESOURCE_STATE_GENERIC_READ: return "GENERIC_READ";
    default: {
        char text[32]{};
        std::snprintf(text, sizeof(text), "STATE_0x%08x", static_cast<unsigned>(state));
        return text;
    }
    }
}

struct Tracked {
    std::string name;
    D3D12_RESOURCE_STATES state;
    bool fixed;
};

// Source-owned state model. It validates the sequence the fixture records; it is not a claim that
// the runtime rejects a wrong-state command without the debug layer.
struct StateAudit {
    std::vector<Tracked> tracked;
    std::vector<std::string> failures;
    int transitions = 0;
    int uav_barriers = 0;

    Tracked* find(const char* name)
    {
        for (auto& entry : tracked) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void track(const char* name, D3D12_RESOURCE_STATES state, bool fixed)
    {
        if (find(name)) throw std::runtime_error("duplicate tracked resource");
        tracked.push_back({name, state, fixed});
    }

    bool require(const char* name, D3D12_RESOURCE_STATES expected, const char* operation)
    {
        const Tracked* entry = find(name);
        if (!entry) {
            failures.push_back(std::string("wrong_state unknown_resource=") + name +
                               " operation=" + operation);
            return false;
        }
        if (entry->state != expected) {
            failures.push_back("wrong_state " + entry->name + "=" + state_name(entry->state) +
                               " expected=" + state_name(expected) + " operation=" + operation);
            return false;
        }
        return true;
    }

    bool transition(const char* name, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                    const char* operation)
    {
        Tracked* entry = find(name);
        if (!entry) {
            failures.push_back(std::string("wrong_state unknown_resource=") + name +
                               " operation=" + operation);
            return false;
        }
        if (entry->fixed) {
            failures.push_back("wrong_state " + entry->name +
                               " fixed_state=" + state_name(entry->state) +
                               " transition_to=" + state_name(after) + " operation=" + operation);
            return false;
        }
        if (entry->state != before) {
            failures.push_back("wrong_state " + entry->name + "=" + state_name(entry->state) +
                               " expected=" + state_name(before) + " operation=" + operation);
            return false;
        }
        entry->state = after;
        ++transitions;
        return true;
    }

    bool uav(const char* name, const char* operation)
    {
        if (!require(name, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, operation)) return false;
        ++uav_barriers;
        return true;
    }
};

bool contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

void print_audit(const char* label, const StateAudit& audit)
{
    for (const auto& entry : audit.tracked) {
        std::printf("%s state %s=%s%s\n", label, entry.name.c_str(),
                    state_name(entry.state).c_str(), entry.fixed ? " fixed" : "");
    }
    std::printf("%s state_audit transitions=%d uav_barriers=%d failures=%zu\n", label,
                audit.transitions, audit.uav_barriers, audit.failures.size());
    for (const auto& failure : audit.failures) {
        std::printf("%s failure %s\n", label, failure.c_str());
    }
}

StateAudit positive_audit()
{
    StateAudit audit;
    audit.track("upload", D3D12_RESOURCE_STATE_GENERIC_READ, true);
    audit.track("target", D3D12_RESOURCE_STATE_COMMON, false);
    audit.track("readback", D3D12_RESOURCE_STATE_COPY_DEST, true);
    audit.transition("target", D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST,
                     "CopyBufferRegion(upload->target)");
    audit.require("upload", D3D12_RESOURCE_STATE_GENERIC_READ,
                  "CopyBufferRegion(upload->target)");
    audit.transition("target", D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "ClearUnorderedAccessViewUint");
    audit.require("target", D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Dispatch(1,1,1)");
    audit.uav("target", "UAV barrier before Dispatch(3,1,1)");
    audit.transition("target", D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE, "CopyBufferRegion(target->readback)");
    audit.require("readback", D3D12_RESOURCE_STATE_COPY_DEST,
                  "CopyBufferRegion(target->readback)");
    audit.transition("target", D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON,
                     "end of list");
    return audit;
}

bool wrong_state_audit()
{
    StateAudit audit;
    audit.track("upload", D3D12_RESOURCE_STATE_GENERIC_READ, true);
    audit.track("target", D3D12_RESOURCE_STATE_COMMON, false);
    audit.track("readback", D3D12_RESOURCE_STATE_COPY_DEST, true);
    const bool dispatch_without_uav =
        !audit.require("target", D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Dispatch(1,1,1)");
    const bool upload_transition =
        !audit.transition("upload", D3D12_RESOURCE_STATE_GENERIC_READ,
                          D3D12_RESOURCE_STATE_COPY_DEST, "CopyBufferRegion(upload->target)");
    const bool readback_transition =
        !audit.transition("readback", D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_COPY_SOURCE, "CopyBufferRegion(target->readback)");
    const bool copy_from_common =
        !audit.require("target", D3D12_RESOURCE_STATE_COPY_SOURCE,
                       "CopyBufferRegion(target->readback)");
    print_audit("negative", audit);
    bool named = audit.failures.size() == 4;
    if (named) {
        named = contains(audit.failures[0], "target=COMMON expected=UNORDERED_ACCESS") &&
                contains(audit.failures[1], "upload") &&
                contains(audit.failures[1], "fixed_state") &&
                contains(audit.failures[2], "readback") &&
                contains(audit.failures[2], "fixed_state") &&
                contains(audit.failures[3], "target=COMMON expected=COPY_SOURCE");
    }
    const bool rejected = dispatch_without_uav && upload_transition && readback_transition &&
                          copy_from_common;
    std::printf("negative wrong_state_controls=4 rejected=%zu expected=4 named=%s\n",
                audit.failures.size(), named ? "yes" : "no");
    return rejected && named;
}

struct Heap {
    const char* name = nullptr;
    ComPtr<ID3D12DescriptorHeap> object;
    UINT increment = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
};

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(const Heap& heap, UINT index)
{
    auto handle = heap.cpu_start;
    handle.ptr += SIZE_T{index} * heap.increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(const Heap& heap, UINT index)
{
    auto handle = heap.gpu_start;
    handle.ptr += UINT64{index} * heap.increment;
    return handle;
}

Heap create_heap(ID3D12Device* device, const char* name, D3D12_DESCRIPTOR_HEAP_TYPE type,
                 UINT capacity, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = flags;
    desc.NodeMask = 0;
    Heap heap;
    heap.name = name;
    check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.object)), name);
    const D3D12_DESCRIPTOR_HEAP_DESC actual = heap.object->GetDesc();
    if (actual.Type != type || actual.NumDescriptors != capacity || actual.Flags != flags) {
        throw std::runtime_error("descriptor heap description changed");
    }
    heap.increment = device->GetDescriptorHandleIncrementSize(type);
    heap.cpu_start = heap.object->GetCPUDescriptorHandleForHeapStart();
    heap.gpu_start = heap.object->GetGPUDescriptorHandleForHeapStart();
    std::printf("heap=%s type=%u flags=0x%x capacity=%u increment=%u cpu_start=0x%llx "
                "gpu_start=0x%llx\n",
                name, static_cast<unsigned>(type), static_cast<unsigned>(flags), capacity,
                heap.increment, static_cast<unsigned long long>(heap.cpu_start.ptr),
                static_cast<unsigned long long>(heap.gpu_start.ptr));
    if (heap.increment == 0 || heap.cpu_start.ptr == 0) {
        throw std::runtime_error("descriptor heap has no CPU start or increment");
    }
    if ((flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && heap.gpu_start.ptr == 0) {
        throw std::runtime_error("shader-visible descriptor heap has no GPU start");
    }
    return heap;
}

void write_upload(ID3D12Resource* resource, const void* data, std::size_t size)
{
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(resource->Map(0, &no_read, &mapped), "Map compute-list upload");
    std::memcpy(mapped, data, size);
    const D3D12_RANGE written{0, size};
    resource->Unmap(0, &written);
}

UINT input_word(UINT index)
{
    return 0x10000000u + index * 7u;
}

UINT expected_word(UINT index)
{
    if (index < first_dispatch_words) return input_word(index) + first_addend;
    if (index < tail_offset) return input_word(index) + second_addend;
    return clear_value;
}

ComPtr<ID3D12RootSignature> create_root_signature(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.RegisterSpace = 0;
    parameters[1].Constants.Num32BitValues = 4;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 2;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.NumStaticSamplers = 0;
    desc.Desc_1_1.pStaticSamplers = nullptr;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT serialize = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(serialize)) {
        if (error) {
            std::fprintf(stderr, "Root signature error: %.*s\n",
                         static_cast<int>(error->GetBufferSize()),
                         static_cast<const char*>(error->GetBufferPointer()));
        }
        check(serialize, "Serialize compute-list root signature");
    }
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)),
          "Create compute-list root signature");
    return root;
}

ComPtr<ID3D12PipelineState> create_pipeline(ID3D12Device* device, ID3D12RootSignature* root)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {d3d12x_compute_list_cs, sizeof(d3d12x_compute_list_cs)};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)),
          "Create compute-list pipeline");
    return pipeline;
}

struct Fixture {
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12Resource> readback;
    Heap staging;
    Heap visible;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
};

Fixture create_fixture(ID3D12Device* device)
{
    Fixture fixture;
    fixture.upload = make_buffer(device, buffer_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
    fixture.target = make_buffer(device, buffer_bytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    fixture.readback = make_buffer(device, buffer_bytes, D3D12_HEAP_TYPE_READBACK,
                                   D3D12_RESOURCE_STATE_COPY_DEST);

    std::array<UINT, word_count> initial{};
    for (UINT i = 0; i < word_count; ++i) initial[i] = input_word(i);
    write_upload(fixture.upload.Get(), initial.data(), static_cast<std::size_t>(buffer_bytes));

    fixture.staging = create_heap(device, "staging_views", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                  4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    fixture.visible = create_heap(device, "visible_views", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                  4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    if (fixture.staging.increment != fixture.visible.increment) {
        throw std::runtime_error("view heaps use different descriptor increments");
    }
    if (fixture.staging.increment != device->GetDescriptorHandleIncrementSize(
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
        throw std::runtime_error("view heap increment does not match the device");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = word_count;
    uav.Buffer.StructureByteStride = sizeof(UINT);
    uav.Buffer.CounterOffsetInBytes = 0;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(fixture.target.Get(), nullptr, &uav,
                                      cpu_slot(fixture.staging, 0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC tail = uav;
    tail.Buffer.FirstElement = tail_offset;
    tail.Buffer.NumElements = tail_words;
    device->CreateUnorderedAccessView(fixture.target.Get(), nullptr, &tail,
                                      cpu_slot(fixture.staging, 1));
    device->CopyDescriptorsSimple(2, cpu_slot(fixture.visible, 0), cpu_slot(fixture.staging, 0),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    check(device->GetDeviceRemovedReason(), "Copy compute-list descriptors");
    std::printf("descriptors uav_slots=0,1 source=staging destination=visible\n");

    fixture.root = create_root_signature(device);
    fixture.pipeline = create_pipeline(device, fixture.root.Get());
    std::printf("fixture words=%u bytes=%llu upload=GENERIC_READ target=COMMON "
                "readback=COPY_DEST\n",
                word_count, static_cast<unsigned long long>(buffer_bytes));
    return fixture;
}

void record(Fixture& fixture, ComputeCommands& commands, bool include_second_dispatch)
{
    const StateAudit audit = positive_audit();
    print_audit("record", audit);
    if (!audit.failures.empty()) throw std::runtime_error("compute-list state audit failed");

    ID3D12DescriptorHeap* heaps[]{fixture.visible.object.Get()};
    commands.list->SetDescriptorHeaps(1, heaps);
    commands.list->SetComputeRootSignature(fixture.root.Get());
    commands.list->SetPipelineState(fixture.pipeline.Get());
    commands.list->SetComputeRootDescriptorTable(0, gpu_slot(fixture.visible, 0));

    transition(commands.list.Get(), fixture.target.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_DEST);
    commands.list->CopyBufferRegion(fixture.target.Get(), 0, fixture.upload.Get(), 0,
                                    buffer_bytes);
    transition(commands.list.Get(), fixture.target.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT clear_values[4]{clear_value, 0, 0, 0};
    commands.list->ClearUnorderedAccessViewUint(gpu_slot(fixture.visible, 1),
                                                cpu_slot(fixture.staging, 1),
                                                fixture.target.Get(), clear_values, 0, nullptr);

    const UINT first_constants[4]{0, first_addend, 0, 0};
    commands.list->SetComputeRoot32BitConstants(1, 4, first_constants, 0);
    commands.list->Dispatch(first_groups, 1, 1);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = fixture.target.Get();
    commands.list->ResourceBarrier(1, &uav);

    if (include_second_dispatch) {
        const UINT second_constants[4]{second_dispatch_offset, second_addend, 0, 0};
        commands.list->SetComputeRoot32BitConstants(1, 4, second_constants, 0);
        commands.list->Dispatch(second_groups, 1, 1);
    }

    transition(commands.list.Get(), fixture.target.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->CopyBufferRegion(fixture.readback.Get(), 0, fixture.target.Get(), 0,
                                    buffer_bytes);
    transition(commands.list.Get(), fixture.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    std::printf("record dispatch1=%ux1x1 dispatch2=%s barriers=4 uav_barriers=1 "
                "clear=%u tail=%u..%u\n",
                first_groups, include_second_dispatch ? "3x1x1" : "omitted", clear_value,
                tail_offset, tail_offset + tail_words - 1);
}

std::array<UINT, word_count> read_back(Fixture& fixture)
{
    std::array<UINT, word_count> words{};
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(buffer_bytes)};
    check(fixture.readback->Map(0, &read_range, &mapped), "Map compute-list readback");
    std::memcpy(words.data(), mapped, static_cast<std::size_t>(buffer_bytes));
    const D3D12_RANGE no_write{0, 0};
    fixture.readback->Unmap(0, &no_write);
    return words;
}

UINT fnv1a(const std::array<UINT, word_count>& words)
{
    UINT hash = 2166136261u;
    for (UINT word : words) {
        hash ^= word;
        hash *= 16777619u;
    }
    return hash;
}

bool run_positive(ID3D12Device* device)
{
    Fixture fixture = create_fixture(device);
    ComputeCommands commands(device, fixture.pipeline.Get());
    record(fixture, commands, true);
    commands.submit_and_wait(device);
    const auto actual = read_back(fixture);

    UINT mismatches = 0;
    UINT first = UINT32_MAX;
    for (UINT i = 0; i < word_count; ++i) {
        if (actual[i] != expected_word(i)) {
            if (!mismatches) first = i;
            ++mismatches;
        }
    }
    if (mismatches) {
        std::printf("positive readback words=%u mismatches=%u first=%u hash=0x%08x\n",
                    word_count, mismatches, first, fnv1a(actual));
        std::printf("positive first_mismatch word=%u expected=0x%08x actual=0x%08x\n", first,
                    expected_word(first), actual[first]);
        return false;
    }
    std::printf("positive readback words=%u mismatches=0 first=none hash=0x%08x\n", word_count,
                fnv1a(actual));
    std::puts("positive readback=matched");
    return true;
}

bool run_negative(ID3D12Device* device)
{
    const bool audit_ok = wrong_state_audit();
    Fixture fixture = create_fixture(device);
    ComputeCommands commands(device, fixture.pipeline.Get());
    record(fixture, commands, false);
    commands.submit_and_wait(device);
    const auto actual = read_back(fixture);

    UINT mismatches = 0;
    UINT first = UINT32_MAX;
    for (UINT i = 0; i < word_count; ++i) {
        if (actual[i] != expected_word(i)) {
            if (!mismatches) first = i;
            ++mismatches;
        }
    }
    std::printf("negative omitted_dispatch2 readback_words=%u mismatches=%u "
                "expected_mismatches=%u first=%u\n",
                word_count, mismatches, second_dispatch_words, first);
    if (!mismatches) {
        std::puts("negative expected mismatches but the readback matched");
        return false;
    }

    const UINT first_expected = expected_word(first);
    const UINT first_actual = actual[first];
    std::printf("negative first_mismatch word=%u expected=0x%08x actual=0x%08x\n", first,
                first_expected, first_actual);

    bool prefix_ok = true;
    for (UINT i = 0; i < first_dispatch_words; ++i) {
        if (actual[i] != expected_word(i)) prefix_ok = false;
    }
    bool tail_ok = true;
    for (UINT i = tail_offset; i < word_count; ++i) {
        if (actual[i] != clear_value) tail_ok = false;
    }
    const bool first_ok =
        first == second_dispatch_offset &&
        first_expected == input_word(second_dispatch_offset) + second_addend &&
        first_actual == input_word(second_dispatch_offset);
    const bool count_ok = mismatches == second_dispatch_words;
    std::printf("negative controls omitted_dispatch=1 wrong_state=4 first_word=%u "
                "mismatches=%u expected_mismatches=%u prefix=%s tail=%s\n",
                first, mismatches, second_dispatch_words, prefix_ok ? "ok" : "bad",
                tail_ok ? "ok" : "bad");
    return audit_ok && first_ok && count_ok && prefix_ok && tail_ok;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <path-to-d3d12_xs.dll> <warp|hardware>\n", argv[0]);
        return 2;
    }
    const std::string_view backend = argv[2];
    if (backend != "warp" && backend != "hardware") {
        std::fputs("unknown backend\n", stderr);
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

    ComPtr<ID3D12Device> device =
        create_shim_device(create, choice.adapter.Get(), choice.desc.AdapterLuid);
    int result = 1;
    if (device) {
        try {
            const bool positive = run_positive(device.Get());
            const bool negative = run_negative(device.Get());
            result = positive && negative ? 0 : 1;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "d3d12x_compute_list_check: %s\n", error.what());
            result = 1;
        }
    }
    device.Reset();
    FreeLibrary(shim);
    std::printf("d3d12x_compute_list_check %s: %s\n", argv[2],
                result == 0 ? "PASS" : "FAIL");
    return result;
}
