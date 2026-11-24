// CRT-free startup attempt for a forced-wave compute pipeline (Refs #870, compare #1132).
//
// The entry point enables the D3D12 debug layer when it is installed, creates a device on WARP or on
// the first hardware adapter (XBOX2WINDOWS_ADAPTER filters by description), records the adapter's
// D3D12_OPTIONS1 wave lane range, and calls CreateComputePipelineState once with the shader variant
// built into this EXE: no WaveSize attribute, [WaveSize(4)] or [WaveSize(32)]. It prints the exact
// HRESULT and every debug-layer message stored by that call. When creation succeeds it dispatches one
// 64-thread group and reads back WaveGetLaneCount() from every invocation. XBOX2WINDOWS_DEBUG_LAYER=off
// repeats the attempt without the debug layer.
//
// Exit codes: 0 when the outcome matches the reported capabilities (created, and the readback is a
// single width inside the range, equal to the forced width when there is one); 77 when the adapter
// cannot run the variant (no SM 6.6 or WaveOps, or a forced width outside WaveLaneCountMin..Max) and
// creation was refused; 1 for anything else, including creation that contradicts the reported range.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "wave_startup_cs.h"  // per-variant DXC output; WAVE_STARTUP_WIDTH matches its attribute

// The Debug preset enables /RTC1. Turn the checks off here and provide the two bookkeeping symbols it
// still references, instead of linking the CRT.
#pragma runtime_checks("", off)
extern "C" void _RTC_InitBase() {}
extern "C" void _RTC_Shutdown() {}

#pragma function(memset)
extern "C" void* __cdecl memset(void* target, int value, size_t size)
{
    volatile unsigned char* bytes = static_cast<unsigned char*>(target);
    while (size--) *bytes++ = static_cast<unsigned char>(value);
    return target;
}

namespace {

constexpr UINT threads = 64;
constexpr int exit_pass = 0;
constexpr int exit_fail = 1;
constexpr int exit_skip = 77;

HANDLE out_handle;
// Static buffers keep every frame below a page, so no __chkstk from the CRT is needed.
char text_buffer[1024];
unsigned char message_storage[4096];
wchar_t wanted[128];

void write(const char* text)
{
    DWORD length = 0;
    while (text[length]) ++length;
    DWORD written = 0;
    WriteFile(out_handle, text, length, &written, nullptr);
}

void write_hex(UINT value)
{
    char digits[11] = "0x00000000";
    for (int i = 9; i >= 2; --i, value >>= 4) digits[i] = "0123456789abcdef"[value & 15];
    write(digits);
}

void write_dec(UINT value)
{
    char digits[11]{};
    int at = 10;
    do digits[--at] = static_cast<char>('0' + value % 10); while (value /= 10);
    write(digits + at);
}

void write_wide(const wchar_t* text)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, text_buffer, sizeof(text_buffer), nullptr, nullptr);
    text_buffer[bytes > 0 ? bytes - 1 : 0] = 0;
    write(text_buffer);
}

const char* hresult_name(HRESULT hr)
{
    switch (hr) {
    case S_OK: return "S_OK";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_FAIL: return "E_FAIL";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    default: return "unnamed";
    }
}

void write_hr(const char* label, HRESULT hr)
{
    write(label);
    write("=");
    write_hex(static_cast<UINT>(hr));
    write(" (");
    write(hresult_name(hr));
    write(")\n");
}

bool fail(const char* stage, HRESULT hr)
{
    if (SUCCEEDED(hr)) return false;
    write_hr(stage, hr);
    return true;
}

wchar_t fold(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; }

bool contains_ignoring_case(const wchar_t* text, const wchar_t* part)
{
    for (; *text; ++text) {
        const wchar_t* a = text;
        const wchar_t* b = part;
        while (*a && *b && fold(*a) == fold(*b)) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}

const char* severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
    case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
    case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
    case D3D12_MESSAGE_SEVERITY_INFO: return "info";
    default: return "message";
    }
}

// Prints and clears every stored debug-layer message; returns the number of errors and corruptions.
UINT drain_messages(ID3D12InfoQueue* queue, const char* stage)
{
    if (!queue) return 0;
    UINT errors = 0;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes > sizeof(message_storage)) {
            write("debug_message_unreadable stage=");
            write(stage);
            write("\n");
            ++errors;
            continue;
        }
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(message_storage);
        if (FAILED(queue->GetMessage(i, message, &bytes))) continue;
        write("debug_message stage=");
        write(stage);
        write(" severity=");
        write(severity_name(message->Severity));
        write(" id=");
        write_dec(static_cast<UINT>(message->ID));
        write(": ");
        write(message->pDescription);
        write("\n");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++errors;
    }
    queue->ClearStoredMessages();
    write("debug_messages stage=");
    write(stage);
    write(" count=");
    write_dec(static_cast<UINT>(count));
    write("\n");
    return errors;
}

IDXGIAdapter1* select_adapter(IDXGIFactory6* factory, ID3D12Device** device)
{
    IDXGIAdapter1* adapter = nullptr;
#if WAVE_STARTUP_WARP
    if (fail("EnumWarpAdapter", factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return nullptr;
    if (!fail("D3D12CreateDevice", D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device))))
        return adapter;
    adapter->Release();
    return nullptr;
#else
    const DWORD filter = GetEnvironmentVariableW(L"XBOX2WINDOWS_ADAPTER", wanted, 128);
    if (filter >= 128) {
        write("XBOX2WINDOWS_ADAPTER is longer than 127 characters\n");
        return nullptr;
    }
    for (UINT index = 0;
         factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
         DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!filter || contains_ignoring_case(desc.Description, wanted)) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device))))
            return adapter;
        adapter->Release();
        adapter = nullptr;
    }
    write(filter ? "No D3D12 hardware adapter matches XBOX2WINDOWS_ADAPTER\n" : "No D3D12-capable hardware adapter visible\n");
    return nullptr;
#endif
}

ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = threads * sizeof(UINT);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    if (fail("CreateCommittedResource",
             device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

// One dispatch of the created pipeline; returns the width every invocation reported, or 0 when the
// readback is missing or the invocations disagree.
UINT dispatch_and_read(ID3D12Device* device, ID3D12RootSignature* root, ID3D12PipelineState* pipeline)
{
    UINT width = 0;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* lanes = buffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* readback = buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (lanes && readback && event &&
        !fail("CreateCommandQueue", device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
        !fail("CreateCommandAllocator",
              device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator))) &&
        !fail("CreateCommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, pipeline,
                                                             IID_PPV_ARGS(&list))) &&
        !fail("CreateFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        list->SetComputeRootSignature(root);
        list->SetComputeRootUnorderedAccessView(0, lanes->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = lanes;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, lanes);
        ID3D12CommandList* lists[] = {list};
        if (!fail("Close", list->Close())) {
            queue->ExecuteCommandLists(1, lists);
            if (!fail("Signal", queue->Signal(fence, 1)) &&
                !fail("SetEventOnCompletion", fence->SetEventOnCompletion(1, event))) {
                if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) {
                    write("fence_wait=timeout\n");
                } else {
                    const D3D12_RANGE whole{0, threads * sizeof(UINT)};
                    void* mapped = nullptr;
                    if (!fail("Map", readback->Map(0, &whole, &mapped))) {
                        const UINT* values = static_cast<const UINT*>(mapped);
                        UINT mismatches = 0;
                        for (UINT i = 1; i < threads; ++i) mismatches += values[i] != values[0];
                        write("readback lane_count=");
                        write_dec(values[0]);
                        write(" invocations=");
                        write_dec(threads);
                        write(" mismatches=");
                        write_dec(mismatches);
                        write("\n");
                        if (!mismatches) width = values[0];
                        const D3D12_RANGE none{0, 0};
                        readback->Unmap(0, &none);
                    }
                }
            }
        }
    }
    write_hr("device_removed_reason", device->GetDeviceRemovedReason());
    if (event) CloseHandle(event);
    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    if (queue) queue->Release();
    if (readback) readback->Release();
    if (lanes) lanes->Release();
    return width;
}

int run()
{
    write("forced_wave_size=");
    write_dec(WAVE_STARTUP_WIDTH);
    write(WAVE_STARTUP_WIDTH ? "" : " (no attribute)");
    write(" adapter_kind=");
    write(WAVE_STARTUP_WARP ? "warp\n" : "hardware\n");
    // XBOX2WINDOWS_DEBUG_LAYER=off runs the same attempt without the layer, as a title would.
    wchar_t layer[4]{};
    const DWORD layer_length = GetEnvironmentVariableW(L"XBOX2WINDOWS_DEBUG_LAYER", layer, 4);
    const bool layer_off = layer_length == 3 && fold(layer[0]) == L'o' && fold(layer[1]) == L'f' && fold(layer[2]) == L'f';
    ID3D12Debug* debug = nullptr;
    if (layer_off) {
        write("debug_layer=off (XBOX2WINDOWS_DEBUG_LAYER)\n");
    } else {
        const HRESULT debug_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (debug) debug->EnableDebugLayer();
        write("debug_layer=");
        write(debug ? "enabled\n" : "unavailable\n");
        if (!debug) write_hr("D3D12GetDebugInterface", debug_hr);
    }

    IDXGIFactory6* factory = nullptr;
    if (fail("CreateDXGIFactory2", CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return exit_fail;
    ID3D12Device* device = nullptr;
    IDXGIAdapter1* adapter = select_adapter(factory, &device);
    if (!adapter) return WAVE_STARTUP_WARP ? exit_fail : exit_skip;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    write("adapter=");
    write_wide(desc.Description);
    write(" vendor=");
    write_hex(desc.VendorId);
    write(" device=");
    write_hex(desc.DeviceId);
    LARGE_INTEGER driver{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
        write(" driver=");
        write_dec(HIWORD(driver.HighPart));
        write(".");
        write_dec(LOWORD(driver.HighPart));
        write(".");
        write_dec(HIWORD(driver.LowPart));
        write(".");
        write_dec(LOWORD(driver.LowPart));
    }
    write("\n");

    ID3D12InfoQueue* messages = nullptr;
    if (debug && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
        // Keep everything, so the refusal text is stored even when it is not an error.
        messages->SetMessageCountLimit(UINT64(-1));
        drain_messages(messages, "device");
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    const HRESULT options_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
    const HRESULT model_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    if (FAILED(model_hr)) model.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    write("wave_ops=");
    write_dec(options1.WaveOps ? 1 : 0);
    write(" wave_lane_count_min=");
    write_dec(options1.WaveLaneCountMin);
    write(" wave_lane_count_max=");
    write_dec(options1.WaveLaneCountMax);
    write(" highest_shader_model=");
    write_hex(static_cast<UINT>(model.HighestShaderModel));
    write("\n");
    if (FAILED(options_hr)) write_hr("CheckFeatureSupport(OPTIONS1)", options_hr);

    const UINT forced = WAVE_STARTUP_WIDTH;
    const bool sm66 = SUCCEEDED(model_hr) && model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    const bool in_range = !forced || (options1.WaveLaneCountMin <= forced && forced <= options1.WaveLaneCountMax);
    const bool supported = SUCCEEDED(options_hr) && options1.WaveOps && sm66 && in_range;

    ID3D12RootSignature* root = nullptr;
    if (fail("CreateRootSignature", device->CreateRootSignature(0, wave_startup_cs, sizeof(wave_startup_cs),
                                                                IID_PPV_ARGS(&root)))) {
        drain_messages(messages, "root_signature");
        return supported ? exit_fail : exit_skip;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root;
    pso.CS = {wave_startup_cs, sizeof(wave_startup_cs)};
    ID3D12PipelineState* pipeline = nullptr;
    const HRESULT create_hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
    write_hr("CreateComputePipelineState", create_hr);
    drain_messages(messages, "create_pipeline");

    int result = exit_fail;
    if (SUCCEEDED(create_hr)) {
        const UINT width = dispatch_and_read(device, root, pipeline);
        const UINT errors = drain_messages(messages, "dispatch");
        const bool width_ok = width && (forced ? width == forced
                                               : options1.WaveLaneCountMin <= width && width <= options1.WaveLaneCountMax);
        if (!supported) {
            write("result=fail: created a pipeline the reported capabilities exclude\n");
        } else if (!width_ok || errors) {
            write("result=fail: readback width or debug errors do not match\n");
        } else {
            write("result=pass\n");
            result = exit_pass;
        }
        pipeline->Release();
    } else if (supported) {
        write("result=fail: refused a pipeline the reported capabilities allow\n");
    } else {
        write("skip: wave startup variant requires SM 6.6, WaveOps and a wave lane range containing the forced width\n");
        result = exit_skip;
    }
    root->Release();
    if (messages) messages->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    if (debug) debug->Release();
    return result;
}

}  // namespace

extern "C" int WINAPI fixture_entry()
{
    out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    ExitProcess(static_cast<UINT>(run()));
}
