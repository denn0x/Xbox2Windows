// LOCAL stall: a translated slot-67 compute dispatch (list slot 14) issued while its pipeline is not loaded yet
// (d3d12_compute.inl, X2W_CX_ASYNC).
//
//   startup_cx_async <warp|hardware> <async|sync|known|seen|heap>
//   (X2W_D3D12_RENDER=1 X2W_D3D12_PERMISSIVE=1 X2W_D3D12_LIST_NOOP=1 X2W_D3D12_DEBUG=1; async also X2W_CX_ASYNC=1;
//    the test sets X2W_SHADER_CACHE, X2W_TRAP_LOG and X2W_CX_TEST_HOLD_EVENT itself)
//
// The test writes <key>.cs.txt (`cs 1 1 1`, `uav 0 buf root 0 0`) and <key>.cs.cso (dxc-built DXIL of
// startup_cx_async.hlsl: every executed dispatch adds 1 to word 0 of root UAV 0) for a fake slot-67 pipeline
// ([0] a title root signature, [5]/[6] fake console code; key = fnv(root blob, fnv(code)) tagged 0x67). Every load
// of a translation waits for the named event X2W_CX_TEST_HOLD_EVENT (at most 20 s), so the pipeline is held back
// until the test sets it. The title buffer: a placed buffer over 2 MiB of title memory, word 0 = 0.
//
// List 1 (recorded while the load is held): three Dispatch(1, 1, 1) of the pipeline, root UAV 0 = the buffer.
//   async: all three skipped: word 0 stays 0; the log names the key in the "dispatch skipped" line. Then the
//          event is set and a list with one dispatch is executed every 50 ms (at most 200 times) until word 0 is
//          1: the pipeline runs once ready. The "ready (X2W_CX_ASYNC)" line counts >= 3 dispatches skipped while
//          compiling.
//   sync (control, switch off): the event is set 300 ms after recording starts; the recording thread waits for
//          the load, so list 1 runs all three: word 0 is 3; no skip line and no ready line.
//   known (X2W_CX_PREWARM_ALL=1 X2W_CX_ASYNC=1, no hold): a second cached translation B; no keys-seen file. Both
//          keys are queued at device creation ("2 known pipelines queued ... every cached translation") and loaded
//          before the pipeline is created; list 1 runs all three (counter 3), no skip line; the keys-seen file
//          becomes exactly key A (the only dispatched key).
//   seen (as known, with a keys-seen file of B and C; C has no translation): "1 known pipelines queued ... 2 keys,
//          1 with a translation"; the file becomes A, B and C.
// All: the translation's "translated:" line; the hold line "released" in async / sync only; no debug-layer error
// or corruption message, device not removed.
//   heap (LOCAL meshrt, manifest `heap 1`, HeapMain, no hold): a 4-slot title view heap (128 bytes) with word A in
//          slot 1 and word B just past its end; one dispatch stores heapmem's words 32 and 128: A and 0 (heapmem is a
//          bounded view; an unbounded root SRV reads B), plus the "bounded heap views" line. WARP (view increment
//          not 32): the dispatch is skipped by design, capability skip.
// Exit 0 pass, 77 capability skip (no adapter, no existing-heap support), other values name the step.
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

#include "startup_cx_async_cs.h"
#include "startup_cx_heap_cs.h"

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
std::string read_log(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(file), {}};
}
HANDLE hold_event;
DWORD WINAPI release_later(void*)
{
    Sleep(300);
    SetEvent(hold_event);
    return 0;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const std::string mode = argv[2];
    if (mode != "async" && mode != "sync" && mode != "known" && mode != "seen" && mode != "heap") return 2;
    const bool async = mode == "async";
    const bool heap = mode == "heap";  // LOCAL meshrt: `heap 1`, HeapMain, a 4-slot title view heap (no hold)
    const bool prewarm = mode == "known" || mode == "seen";  // X2W_CX_PREWARM_ALL (no hold)
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
    {  // existing heaps over process memory (the windows); a probe device only
        ComPtr<ID3D12Device3> desktop;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 4;
        void* block = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ComPtr<ID3D12Heap> heap;
        const HRESULT hr = block ? desktop->OpenExistingHeapFromAddress(block, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
        heap.Reset();
        if (block) VirtualFree(block, 0, MEM_RELEASE);
        if (FAILED(hr)) {
            std::printf("OpenExistingHeapFromAddress -> 0x%08lx: capability skip\n", static_cast<unsigned long>(hr));
            return 77;
        }
    }

    // Shader cache, log and hold event of this process (process id + tick count: never an earlier run's files).
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 5;
    const std::wstring tag = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const std::wstring cache = std::wstring(temp) + L"x2w_cx_async_" + tag;
    CreateDirectoryW(cache.c_str(), nullptr);
    SetEnvironmentVariableW(L"X2W_SHADER_CACHE", cache.c_str());
    const std::wstring log_path = cache + L"\\trap.log";
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path.c_str());
    const std::wstring event_name = L"Local\\x2w_cx_hold_" + tag;
    hold_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!hold_event) return 5;
    if (!prewarm && !heap) SetEnvironmentVariableW(L"X2W_CX_TEST_HOLD_EVENT", event_name.c_str());

    // The title's compute root signature (its bytes go into the key; the translated one comes from the manifest).
    D3D12_ROOT_PARAMETER title_params[1]{};
    title_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    const D3D12_ROOT_SIGNATURE_DESC title_root_desc{1, title_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> title_blob;
    if (FAILED(D3D12SerializeRootSignature(&title_root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &title_blob, nullptr))) return 5;
    static unsigned char code[192];  // fake console code (the stand-in only hashes it)
    for (int i = 0; i < 192; ++i) code[i] = static_cast<unsigned char>(0x3B ^ (i * 5));
    const std::uint64_t key = (fnv(title_blob->GetBufferPointer(), title_blob->GetBufferSize(), fnv(code, sizeof(code))) & 0x00ffffffffffffffull) |
                              (0x67ull << 56);
    char key_text[24];
    std::snprintf(key_text, sizeof(key_text), "%016llx", static_cast<unsigned long long>(key));
    std::printf("compute key %s\n", key_text);
    // known / seen: a second cached translation (key B, never created or dispatched); seen: a keys-seen file of
    // B and C (C has no translation), so only B is queued at start and the file afterwards holds A, B and C.
    const std::uint64_t key_b = key ^ 0x10, key_c = 0x67000000000000c0ull;
    for (const std::uint64_t k : {key, key_b}) {
        if (k == key_b && !prewarm) continue;
        wchar_t name[24];
        swprintf(name, 24, L"%016llx", static_cast<unsigned long long>(k));
        const std::wstring base = cache + L"\\" + name;
        const std::string manifest = heap ? "cs 1 1 1\nuav 0 buf root 0 0\nheap 1\n" : "cs 1 1 1\nuav 0 buf root 0 0\n";
        if (!(heap ? write_file(base + L".cs.cso", startup_cx_heap_cs, sizeof(startup_cx_heap_cs))
                   : write_file(base + L".cs.cso", startup_cx_async_cs, sizeof(startup_cx_async_cs))) ||
            !write_file(base + L".cs.txt", manifest.data(), manifest.size()))
            return 5;
    }
    const auto key_line = [](std::uint64_t k) {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx\n", static_cast<unsigned long long>(k));
        return std::string(text);
    };
    const std::wstring seen_path = cache + L"\\cx_keys_seen.lst";
    if (mode == "seen") {
        const std::string seen = key_line(key_b) + key_line(key_c);
        if (!write_file(seen_path, seen.data(), seen.size())) return 5;
    }

    std::array<unsigned char, 64> device_parameters{};
    void* device = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), device_parameters.data(), __uuidof(ID3D12Device), &device))) return 6;
    ComPtr<ID3D12Device> desktop;  // the same device object as the stand-in's (one device per adapter)
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 6;
    // known / seen: the start prewarm loads the known keys before the title creates anything (at most 15 s).
    const std::string all_loaded = std::string("x2w-cx prewarm: all ") + (mode == "known" ? "2" : "1") + " known pipelines loaded";
    if (prewarm) {
        bool loaded = false;
        for (int i = 0; i < 300 && !loaded; ++i) {
            loaded = read_log(log_path).find(all_loaded) != std::string::npos;
            if (!loaded) Sleep(50);
        }
        std::printf("start prewarm: \"%s\" %s\n", all_loaded.c_str(), loaded ? "seen" : "NOT seen within 15 s");
        if (!loaded) return 9;
    }
    void *title_root = nullptr, *pipeline = nullptr;
    if (!ok(invoke(device, 16, 0, arg(title_blob->GetBufferPointer()), title_blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(&title_root))))
        return 7;
    // The slot-67 record: [0] root signature, [5]/[6] code. Creation queues the key for the prewarm workers, whose
    // load now waits for the event.
    alignas(8) static unsigned char record[0x300];
    Word record_words[9] = {};
    record_words[0] = arg(title_root);
    record_words[5] = arg(code), record_words[6] = sizeof(code);
    std::memcpy(record, record_words, sizeof(record_words));
    if (!ok(invoke(device, 67, arg(record), arg(&__uuidof(ID3D12PipelineState)), arg(&pipeline))) || !pipeline) return 8;

    // Title memory: 2 MiB, word 0 the counter; a placed buffer over all of it (the window the UAV is made on; a root
    // UAV is bound over the readable bytes from its address, at most 1 MiB, which must lie inside one window).
    auto* memory = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, 2u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return 10;
    D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2u << 20, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    void* placed = nullptr;
    if (!ok(invoke(device, 133, arg(memory), arg(&buffer), D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)), arg(&placed)))) return 11;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    void *queue = nullptr, *fence = nullptr, *allocator = nullptr, *list = nullptr;
    if (!ok(invoke(device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue))) ||
        !ok(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))) ||
        !ok(invoke(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator))) ||
        !ok(invoke(device, 73, arg(list_desc), arg(allocator), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list))))
        return 13;
    Word fence_value = 0;
    // One list of `dispatches` Dispatch(1, 1, 1), executed and waited for.
    const auto run_list = [&](int dispatches) {
        invoke(list, 139, arg(allocator));            // Reset
        invoke(list, 33, arg(title_root));            // SetComputeRootSignature
        invoke(list, 25, arg(pipeline));              // SetPipelineState (the slot-67 pipeline)
        invoke(list, 45, 0, arg(memory));             // SetComputeRootUnorderedAccessView(0, title address)
        for (int i = 0; i < dispatches; ++i) invoke(list, 14, 1, 1, 1);
        invoke(list, 9);                              // Close
        void* lists[] = {list};
        invoke(queue, 10, 1, arg(lists));
        if (!ok(invoke(queue, 14, arg(fence), ++fence_value))) return false;
        const auto deadline = GetTickCount64() + 30000;
        while (invoke(fence, 8) < fence_value) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
        return true;
    };

    int failures = 0;
    if (heap) {
        // A shader-visible 4-slot title view heap at title + 1 MiB (128 bytes); word 0 of slot 1 = A, and word 0 of
        // "slot 4", the first word past the heap, = B. heapmem is a bounded view of the heap, so the shader must
        // read A and 0. (An unbounded root SRV, the earlier binding, reads B there.)
        const struct {
            D3D12_DESCRIPTOR_HEAP_DESC desc;
            Word backing, bytes;
        } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE}, arg(memory + 0x40000), 4 * 32};
        void* view_heap = nullptr;
        if (!ok(invoke(device, 14, arg(&heap_desc), arg(&__uuidof(ID3D12DescriptorHeap)), arg(&view_heap))) || !view_heap) return 12;
        constexpr std::uint32_t A = 0xa1a1a1a1u, B = 0xb2b2b2b2u;
        memory[0x40000 + 8] = A;   // slot 1 (byte 32)
        memory[0x40000 + 32] = B;  // byte 128: past the heap
        memory[1] = memory[2] = 0x11111111u;
        if (!run_list(1)) return 14;
        const std::string log = read_log(log_path);
        const auto has = [&](const std::string& text) { return log.find(text) != std::string::npos; };
        const bool skipped32 = has(std::string("x2w-cx ") + key_text + ": heap dispatch skipped: increment not 32");
        std::printf("heap: in range %08x (want %08x), past the end %08x (want 00000000; unbounded would read %08x); increment skip %d\n",
                    memory[1], A, memory[2], B, skipped32);
        if (skipped32) {  // WARP: views are not 32 bytes apart, so `heap 1` dispatches are skipped by design
            std::printf("heap dispatches need a 32-byte view increment: capability skip\n");
            return memory[1] == 0x11111111u && memory[2] == 0x11111111u ? 77 : 16;
        }
        const bool bounded = has(std::string("x2w-cx ") + key_text + ": bounded heap views: heapmem 128 bytes (title heap 128), vinfo ");
        std::printf("log: bounded heap views line %d\n", bounded);
        if (memory[1] != A || memory[2] != 0 || !bounded) {
            std::printf("FAIL: heapmem is not a bounded view of the 128-byte heap\n");
            ++failures;
        }
        for (std::size_t p = log.find("x2w-d3d12-debug"); p != std::string::npos; p = log.find("x2w-d3d12-debug", p + 1)) {
            const std::size_t end = log.find('\n', p);
            const std::string line = log.substr(p, end == std::string::npos ? std::string::npos : end - p);
            std::printf("%s\n", line.c_str());
            if (line.find("sev=0 ") != std::string::npos || line.find("sev=1 ") != std::string::npos) ++failures;
        }
        std::printf("device removed reason: 0x%08lx\n", static_cast<unsigned long>(desktop->GetDeviceRemovedReason()));
        if (desktop->GetDeviceRemovedReason() != S_OK) ++failures;
        std::printf("%s\n", failures ? "FAIL" : "PASS");
        return failures ? 16 : 0;
    }
    if (mode == "sync") {
        if (const HANDLE t = CreateThread(nullptr, 0, release_later, nullptr, 0, nullptr)) CloseHandle(t);
        else return 13;
    }
    const ULONGLONG list1_start = GetTickCount64();
    if (!run_list(3)) return 14;
    const std::uint32_t after_first = memory[0];
    std::printf(prewarm ? "list 1 (3 dispatches, pipeline prewarmed at start): counter %u, recorded and executed in %llu ms\n"
                        : "list 1 (3 dispatches, pipeline held back): counter %u, recorded and executed in %llu ms\n", after_first,
                static_cast<unsigned long long>(GetTickCount64() - list1_start));
    if (after_first != (async ? 0u : 3u)) {
        std::printf("FAIL: list 1 should leave the counter at %u\n", async ? 0u : 3u);
        ++failures;
    }
    int tries = 0;
    if (async) {
        SetEvent(hold_event);
        while (memory[0] == 0 && tries < 200) {
            Sleep(50);
            ++tries;
            if (!run_list(1)) return 14;
        }
        std::printf("after the release: counter %u after %d one-dispatch lists\n", memory[0], tries);
        if (memory[0] != 1) {
            std::printf("FAIL: once ready, exactly one dispatch should have run (counter 1)\n");
            ++failures;
        }
    }

    // The log (the worker writes the ready line just after the load completes: wait for it at most 5 s).
    const std::string skip_line = std::string("x2w-cx ") + key_text + ": pipeline not loaded yet, dispatch skipped";
    const std::string ready_line = std::string("x2w-cx ") + key_text + " ready (X2W_CX_ASYNC)";
    std::string log = read_log(log_path);
    for (int i = 0; async && i < 100 && log.find(ready_line) == std::string::npos; ++i) {
        Sleep(50);
        log = read_log(log_path);
    }
    const auto has = [&](const std::string& text) { return log.find(text) != std::string::npos; };
    long skipped = -1;
    if (const std::size_t p = log.find(ready_line); p != std::string::npos) {
        const std::string line = log.substr(p, log.find('\n', p) - p);
        std::printf("log: %s\n", line.c_str());
        if (const std::size_t q = line.find("; "); q != std::string::npos) skipped = std::strtol(line.c_str() + q + 2, nullptr, 10);
    }
    const bool translated = has(std::string("x2w-cx ") + key_text + " translated: ");
    const bool released = has(std::string("x2w-cx ") + key_text + ": load held by X2W_CX_TEST_HOLD_EVENT (released)");
    std::printf("log: translated %d, hold released %d, skip line %d, ready line %d (%ld dispatches skipped while compiling)\n", translated, released,
                has(skip_line), has(ready_line), skipped);
    if (!translated || released == prewarm) {
        std::printf("FAIL: the translation was not loaded %s\n", prewarm ? "(or the hold was set)" : "through the hold");
        ++failures;
    }
    if (prewarm) {
        // The queued line, and the keys-seen file the writer thread writes every 10 s (wait at most 25 s).
        const std::string queued = mode == "known"
                                       ? "x2w-cx prewarm: 2 known pipelines queued (X2W_CX_PREWARM_ALL; no keys seen file: every cached translation)"
                                       : "x2w-cx prewarm: 1 known pipelines queued (X2W_CX_PREWARM_ALL; keys seen file ";
        const std::string counts = ": 2 keys, 1 with a translation)";
        const bool queued_ok = has(queued) && (mode == "known" || has(counts));
        std::printf("log: queued line %d\n", queued_ok);
        if (!queued_ok) {
            std::printf("FAIL: the start prewarm did not queue what mode %s expects\n", mode.c_str());
            ++failures;
        }
        std::uint64_t sorted[3] = {key_c, key < key_b ? key : key_b, key < key_b ? key_b : key};  // key_c is the smallest
        const std::string want = mode == "known" ? key_line(key) : key_line(sorted[0]) + key_line(sorted[1]) + key_line(sorted[2]);
        std::string seen;
        for (int i = 0; i < 250; ++i) {
            seen = read_log(seen_path);
            if (seen == want) break;
            Sleep(100);
        }
        std::printf("keys seen file: \"%s\" (want \"%s\")\n", seen.c_str(), want.c_str());
        if (seen != want) {
            std::printf("FAIL: the keys seen file is not the union of the earlier file and the dispatched key\n");
            ++failures;
        }
    }
    if (async ? (!has(skip_line) || skipped < 3) : (has(skip_line) || has(ready_line))) {
        std::printf("FAIL: the X2W_CX_ASYNC lines do not match mode %s\n", mode.c_str());
        ++failures;
    }
    for (std::size_t p = log.find("x2w-d3d12-debug"); p != std::string::npos; p = log.find("x2w-d3d12-debug", p + 1)) {
        const std::size_t end = log.find('\n', p);
        const std::string line = log.substr(p, end == std::string::npos ? std::string::npos : end - p);
        std::printf("%s\n", line.c_str());
        if (line.find("sev=0 ") != std::string::npos || line.find("sev=1 ") != std::string::npos) ++failures;
    }
    std::printf("device removed reason: 0x%08lx\n", static_cast<unsigned long>(desktop->GetDeviceRemovedReason()));
    if (desktop->GetDeviceRemovedReason() != S_OK) ++failures;
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 16 : 0;
}
