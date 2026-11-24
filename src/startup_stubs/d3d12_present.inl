// Experimental (docs/startup-runtime.md): shows the presented surface in the title's window.
// Included inside d3d12_xs_device.cpp's anonymous namespace, before queue_present.
//
// A flip-model swap chain on the title window, created on the queue the title presents with; each
// present draws the presented surface into the back buffer with a full-window triangle (the surface
// is usually larger than the window) and flips. Diagnostics go to the window title, because GDI
// drawing does not mix with a flip-model swap chain.

struct WindowPresenter {
    IDXGISwapChain3* chain = nullptr;
    ID3D12DescriptorHeap* targets = nullptr;
    ID3D12DescriptorHeap* textures = nullptr;
    ID3D12Resource* buffers[2] = {};
    // LOCAL stall: two copy slots (allocator, list, view), each reused only once the fence value of its
    // last copy has passed; a present whose slot is still busy skips the window copy instead of waiting.
    ID3D12CommandAllocator* allocators[2] = {};
    ID3D12GraphicsCommandList* lists[2] = {};
    UINT64 slot_values[2] = {};
    UINT next_slot = 0;
    LONG skipped = 0;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE reached = nullptr;
    UINT64 fence_value = 0;
    UINT width = 0, height = 0;
    bool failed = false;
};
WindowPresenter window_presenter;
LONG window_presents;

// The output window belongs to this layer and has its own thread and message loop; the title's
// window is left alone (a swap chain on it made the title stop at one of its own fatal checks).
HWND output_handle;
DWORD WINAPI output_window_thread(void* ready)
{
    WNDCLASSW cls{};
    cls.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
        if (message == WM_CLOSE) {
            ShowWindow(window, SW_MINIMIZE);
            return 0;
        }
        return DefWindowProcW(window, message, w, l);
    };
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = x2w_startup::presenter_class;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
    RECT area{0, 0, 1600, 900};
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    output_handle = CreateWindowExW(0, cls.lpszClassName, L"Xbox2Windows - output", WS_OVERLAPPEDWINDOW, 60, 60,
                                    area.right - area.left, area.bottom - area.top, nullptr, nullptr, cls.hInstance, nullptr);
    if (output_handle) ShowWindow(output_handle, SW_SHOWNOACTIVATE);
    SetEvent(static_cast<HANDLE>(ready));
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
HWND output_window()
{
    static bool started = false;
    if (!started) {
        started = true;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (HANDLE thread = CreateThread(nullptr, 0, output_window_thread, ready, 0, nullptr)) {
            WaitForSingleObject(ready, 5000);
            CloseHandle(thread);
        }
        CloseHandle(ready);
    }
    return output_handle;
}

bool create_window_presenter(ID3D12CommandQueue* queue, HWND window)
{
    WindowPresenter& p = window_presenter;
    ID3D12Device* device = device_of(the_device);
    RECT client{};
    GetClientRect(window, &client);
    p.width = client.right > 0 ? client.right : 1280;
    p.height = client.bottom > 0 ? client.bottom : 720;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = p.width;
        desc.Height = p.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain1);
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    }
    if (SUCCEEDED(hr)) hr = chain1->QueryInterface(IID_PPV_ARGS(&p.chain));
    if (chain1) chain1->Release();
    if (factory) factory->Release();
    const D3D12_DESCRIPTOR_HEAP_DESC target_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC texture_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};  // one view per copy slot
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&target_heap, IID_PPV_ARGS(&p.targets));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&texture_heap, IID_PPV_ARGS(&p.textures));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; SUCCEEDED(hr) && i < 2; ++i) {
        hr = p.chain->GetBuffer(i, IID_PPV_ARGS(&p.buffers[i]));
        if (SUCCEEDED(hr))
            device->CreateRenderTargetView(p.buffers[i], nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{p.targets->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{i} * step});
    }
    for (int i = 0; i < 2; ++i) {
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.allocators[i]));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocators[i], nullptr, IID_PPV_ARGS(&p.lists[i]));
        if (SUCCEEDED(hr)) hr = p.lists[i]->Close();
    }
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence));
    p.reached = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    static const char source[] =
        "Texture2D<float4> surface : register(t0); SamplerState smooth : register(s0);\n"
        "struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target0 { return float4(surface.Sample(smooth, i.uv).rgb, 1); }\n";
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* blob = nullptr;
    if (SUCCEEDED(hr)) hr = D3DCompile(source, sizeof(source) - 1, "present", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, nullptr);
    if (SUCCEEDED(hr)) hr = D3DCompile(source, sizeof(source) - 1, "present", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, nullptr);
    if (SUCCEEDED(hr)) {
        const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root));
    }
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = p.root;
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = ~0u;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&p.pipeline));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (blob) blob->Release();
    log("x2w-present window swap chain %ux%u -> 0x%08lx", p.width, p.height, static_cast<unsigned long>(hr));
    check_device("window presenter", 0);
    return SUCCEEDED(hr);
}

// LOCAL (video): X2W_PRESENT_PLANES_LOG=1 logs the present call's planes when the plane count, the
// first plane's resource count or the surface's size and format change, or a surface not seen before
// appears (at most 32 lines). The title flips between two swap surfaces every frame, which is not a
// change. Plus one summary line per 300 frames (at most 200). Run 84 showed planes=1 throughout
// frames 1-32; this shows whether a later present (a movie, the loading screen) brings more planes,
// which the window presenter does not draw.
void note_present_planes(LONG frame, UINT planes, const void* plane_parameters, const void* present_parameters)
{
    static const bool on = [] {
        const bool value = x2w_local::env_on("X2W_PRESENT_PLANES_LOG");
        if (value) log("x2w-present X2W_PRESENT_PLANES_LOG=1: plane changes and one line per 300 frames are logged");
        return value;
    }();
    if (!on) return;
    static UINT last_planes = ~0u;
    static u64 last_count = ~0ull, last_shape = ~0ull;
    static u64 known[8];
    static UINT known_count, flips_since, extra_planes_since;
    static u64 last_surface;
    static LONG lines, summaries;
    const auto* words = static_cast<const u64*>(plane_parameters);
    const bool header = planes && words && x2w_local::readable(words, 0x18);
    const u64 count = header ? words[1] : 0;
    u64 surface = 0, shape = 0;
    if (header && count >= 1 && x2w_local::readable(reinterpret_cast<const void*>(words[2]), 8)) {
        surface = *reinterpret_cast<const u64*>(words[2]);
        Wrapper* wrapper = as_wrapper(reinterpret_cast<const void*>(surface));
        if (wrapper && wrapper->own().kind == resource_kind && wrapper->own().real) {
            const D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(wrapper->own().real)->GetDesc();
            shape = (desc.Width & 0xFFFFF) | (u64{desc.Height & 0xFFFFF} << 20) | (u64{static_cast<UINT>(desc.Format) & 0xFFF} << 40);
        }
    }
    // Called from queue_present only, one thread at a time per queue; the statics are diagnostics.
    if (surface != last_surface) ++flips_since;
    last_surface = surface;
    if (planes > 1 || count > 1) ++extra_planes_since;
    bool fresh = false;
    if (surface) {
        UINT i = 0;
        while (i < known_count && known[i] != surface) ++i;
        if (i == known_count && known_count < 8) {
            known[known_count++] = surface;
            fresh = true;
        }
    }
    if (frame % 300 == 0 && first_calls(summaries, 200)) {
        log("x2w-present planes summary frame %ld: planes=%u resources=%llu %llux%llu format %llu; surfaces seen %u; surface changes %u and "
            "presents with more than one plane or resource %u in the last 300 frames",
            frame, planes, count, shape & 0xFFFFF, (shape >> 20) & 0xFFFFF, (shape >> 40) & 0xFFF, known_count, flips_since, extra_planes_since);
        flips_since = extra_planes_since = 0;
    }
    if (planes == last_planes && count == last_count && shape == last_shape && !fresh) return;
    last_planes = planes;
    last_count = count;
    last_shape = shape;
    if (!first_calls(lines, 32)) return;
    char a[3 * 0x60 + 1] = "", b[3 * 0x40 + 1] = "";
    if (plane_parameters && x2w_local::readable(plane_parameters, 0x60)) hex_text(plane_parameters, 0x60, a, sizeof(a));
    if (present_parameters && x2w_local::readable(present_parameters, 0x40)) hex_text(present_parameters, 0x40, b, sizeof(b));
    log("x2w-present planes frame %ld: planes=%u resources=%llu surface=0x%llx %llux%llu format %llu; plane=[%s] present=[%s]", frame, planes,
        count, surface, shape & 0xFFFFF, (shape >> 20) & 0xFFFFF, (shape >> 40) & 0xFFF, a, b);
}

// LOCAL (video): X2W_D3D12_TRACE_QUIET=<n> traces one frame ("x2w-frame" lines, as X2W_D3D12_TRACE_FRAME)
// once n presented frames in a row each had 1 to 8 translated draws, after at least one frame with 20
// or more (menu pages) seen 300 times. This fires on the flat loading screen after setup without knowing its
// frame number: run 84 drew 3 per frame at boot, 6 during the title movie, 22-58 on the setup pages
// and 5 after setup. At most 3 traces, at least 3000 frames apart. With X2W_DRAW_DUMP_DIR set and no
// X2W_DRAW_DUMP_FRAME, the first traced frame also gets the per-draw target dumps.
void note_quiet_frame(LONG frame)
{
    static const LONG wanted = [] {
        char value[16]{};
        const LONG n = GetEnvironmentVariableA("X2W_D3D12_TRACE_QUIET", value, sizeof(value)) ? std::atol(value) : 0L;
        if (n > 0) log("x2w-frame X2W_D3D12_TRACE_QUIET=%ld: one frame is traced after %ld quiet frames (1-8 translated draws) past a busy one", n, n);
        return n > 0 ? n : 0L;
    }();
    if (!wanted) return;
    static const bool dump_dir = GetEnvironmentVariableA("X2W_DRAW_DUMP_DIR", nullptr, 0) != 0;
    static LONG last_total, quiet_run, end_frame, next_allowed, traces;
    static bool armed;
    const LONG total = translated_draws;
    const LONG drawn = total - last_total;
    last_total = total;
    if (end_frame && frame == end_frame) {
        InterlockedExchange(&trace_frame_active, 0);
        log("x2w-frame ---- end quiet frame %ld (%ld translated draws)", frame, drawn);
        summarise_indirect_calls();
        end_frame = 0;
        return;
    }
    // Armed by 300 busy frames in all, so a short busy stretch before the title movie (run 84 frames
    // 6600-6900 averaged 12.9 draws) does not point the trace at the movie.
    static LONG busy_frames;
    if (drawn >= 20 && ++busy_frames >= 300) armed = true;
    quiet_run = armed && drawn >= 1 && drawn <= 8 ? quiet_run + 1 : 0;
    if (end_frame || quiet_run < wanted || frame < next_allowed || traces >= 3) return;
    ++traces;
    quiet_run = 0;
    next_allowed = frame + 3000;
    end_frame = frame + 1;
    if (dump_dir && !draw_dump_frame()) InterlockedExchange(&quiet_dump_frame, frame);
    log("x2w-frame ---- begin quiet frame %ld (previous frame: %ld translated draws, %ld quiet frames)%s", frame + 1, drawn, wanted,
        quiet_dump_frame == frame ? "; per-draw target dumps on" : "");
    InterlockedExchange(&trace_frame_active, 1);
}

// Draws `surface` (resting in COMMON) into the window and flips. Returns false if not shown.
bool present_to_window(ID3D12CommandQueue* queue, ID3D12Resource* surface)
{
    WindowPresenter& p = window_presenter;
    if (p.failed || !queue || !surface) return false;
    const D3D12_RESOURCE_DESC desc = surface->GetDesc();
    if (!x2w_startup::present_surface_supported(desc)) {
        static LONG refused;
        if (first_calls(refused, 4))
            log("x2w-present surface dim=%d samples=%u flags=0x%x format=%d not supported (present_view_format.h)",
                desc.Dimension, desc.SampleDesc.Count, desc.Flags, desc.Format);
        return false;
    }
    HWND window = output_window();
    if (!window) return false;
    if (!p.chain && !create_window_presenter(queue, window)) {
        p.failed = true;
        return false;
    }
    // LOCAL stall: X2W_PRESENT_WAIT=1 restores the old order (one slot, wait for the queue after each
    // present). Without it the window copy never waits: run 88 spent ~590 ms per present in that wait
    // (present.inl:323 in the stop dump), which is the whole queue draining, not the copy.
    static const bool wait_each = x2w_local::env_on("X2W_PRESENT_WAIT");
    const UINT slot = wait_each ? 0 : p.next_slot;
    if (!wait_each && p.fence->GetCompletedValue() < p.slot_values[slot]) {
        const LONG n = InterlockedIncrement(&p.skipped);
        if (n <= 4 || n % 500 == 0) log("x2w-present window copy skipped %ld times (previous copies still queued)", n);
        return true;  // the window keeps its last picture
    }
    ID3D12Device* device = device_of(the_device);
    const DXGI_FORMAT view_format = x2w_startup::present_view_format(desc.Format);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = view_format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    const UINT view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(surface, &view, D3D12_CPU_DESCRIPTOR_HANDLE{p.textures->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T{slot} * view_step});
    const UINT index = p.chain->GetCurrentBackBufferIndex();
    const D3D12_CPU_DESCRIPTOR_HANDLE target{p.targets->GetCPUDescriptorHandleForHeapStart().ptr +
                                             SIZE_T{index} * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    ID3D12GraphicsCommandList* list = p.lists[slot];
    if (FAILED(p.allocators[slot]->Reset()) || FAILED(list->Reset(p.allocators[slot], p.pipeline))) return false;
    transition(list, surface, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, p.buffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(p.width), static_cast<LONG>(p.height)};
    list->SetGraphicsRootSignature(p.root);
    list->SetDescriptorHeaps(1, &p.textures);
    list->SetGraphicsRootDescriptorTable(0, D3D12_GPU_DESCRIPTOR_HANDLE{p.textures->GetGPUDescriptorHandleForHeapStart().ptr + UINT64{slot} * view_step});
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, p.buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    transition(list, surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list->Close())) {
        p.failed = true;
        return false;
    }
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    // Without the wait, DO_NOT_WAIT: a full flip queue drops this window frame instead of blocking.
    HRESULT hr = p.chain->Present(0, wait_each ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        const LONG n = InterlockedIncrement(&p.skipped);
        if (n <= 4 || n % 500 == 0) log("x2w-present window flip skipped %ld times (flip queue full)", n);
        hr = S_OK;
    }
    if (SUCCEEDED(queue->Signal(p.fence, ++p.fence_value))) p.slot_values[slot] = p.fence_value;
    p.next_slot = (slot + 1) % 2;
    if (wait_each && p.fence->GetCompletedValue() < p.fence_value && SUCCEEDED(p.fence->SetEventOnCompletion(p.fence_value, p.reached)))
        WaitForSingleObject(p.reached, 2000);  // the allocator is reused next frame
    static LONG failures;
    if (FAILED(hr) && first_calls(failures, 4)) log("x2w-present Present failed 0x%08lx", static_cast<unsigned long>(hr));
    check_device("window present", 0);
    if (SUCCEEDED(hr)) InterlockedIncrement(&window_presents);
    return SUCCEEDED(hr);
}
