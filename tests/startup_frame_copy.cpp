#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "startup_stream_setup.h"

// Two draws in one command list read the same constant buffer address through a descriptor table; the left half
// of the target is drawn first, the right half second. X2W_TEST_FRAME_COPY selects what the title does to the
// memory between the draws:
//   changed  the colour is rewritten in place (red, then blue)
//   same     the memory is left alone (red both times)
//   wrap     upload ring reuse (run with a small X2W_UPLOAD_RING_TEST_KIB): red into the left half, then 64 draws
//            with an empty scissor, each with new contents (64 fresh copies, 32 KiB, more than the test ring),
//            then green into the right half, all in ONE list. The left half must still be red: its copy may not be
//            overwritten before the list has executed.
//   reuse    upload chunk recycling across two submits of one frame (16 KiB test chunks): list A draws red into the
//            left half, then 100 empty-scissor draws from a second buffer with fresh contents each (chunks 0-3);
//            A is executed and waited for. The same list is Reset (draw_frame_copies_again): it draws the unchanged
//            red constants into the right half (a frame-copy cache hit on chunk 0, whose fence has passed), then 64
//            fresh filler copies that recycle the chunks A used. Chunk 0 must not be one of them: the right half
//            must be red. X2W_UPLOAD_RING_TEST_NO_REPIN=1 (negative control) skips the re-pin on the cache hit.
//            X2W_TEST_RING_COUNTERS=<file> receives the ring counters (x2w_upload_ring_test) after the second list.
namespace {
// LOCAL stall: one constant buffer and one view heap backing per drawing thread (X2W_TEST_THREADS in
// startup_stream_consumer); the first thread to draw uses slot 0, as a single-threaded run always did.
constexpr int frame_slots = 8;
alignas(256) float frame_constants_by_slot[frame_slots][64];
std::atomic<int> next_frame_slot{0};
int frame_slot()
{
    thread_local const int slot = next_frame_slot.fetch_add(1) % frame_slots;
    return slot;
}
alignas(256) float filler_constants[64] = {};
void* reuse_heap = nullptr;
StreamWord reuse_table = 0, reuse_step = 0;
void* reuse_guest = nullptr;

void filler_draws(void* list, int count, int salt)
{
    const D3D12_RECT none{0, 0, 0, 0};
    invoke(list, 22, 1, reinterpret_cast<std::uintptr_t>(&none));
    invoke(list, 36, 0, reuse_table + reuse_step);  // the filler buffer
    for (int i = 0; i < count; ++i) {
        filler_constants[0] = 1.0f;  // yellow, a different alpha each time: a fresh copy per draw
        filler_constants[1] = 1.0f;
        filler_constants[2] = 0.0f;
        filler_constants[3] = static_cast<float>(salt + i + 1) / 256.0f;
        invoke(list, 12, 3, 1);
    }
}
}  // namespace

int draw_frame_copies(ID3D12Device* desktop, void* guest, void* list, UINT width, UINT height)
{
    char mode[16]{};
    GetEnvironmentVariableA("X2W_TEST_FRAME_COPY", mode, sizeof(mode));
    const bool changed = !std::strcmp(mode, "changed");
    const bool wrap = !std::strcmp(mode, "wrap");
    const bool reuse = !std::strcmp(mode, "reuse");
    if (!changed && !wrap && !reuse && std::strcmp(mode, "same")) return 2;

    const int slot = frame_slot();
    float* const constants = frame_constants_by_slot[slot];
    std::memset(constants, 0, sizeof(frame_constants_by_slot[slot]));
    constants[0] = 1.0f;  // red; set on every call, as "changed" leaves blue here for the next repeat
    constants[3] = 1.0f;
    const UINT increment = desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT entries = (32 + increment - 1) / increment + (reuse ? 2 : 0);
    static std::vector<unsigned char> views_by_slot[frame_slots];
    std::vector<unsigned char>& views = views_by_slot[slot];
    views.assign(static_cast<std::size_t>(entries) * increment, 0);
    const struct {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        StreamWord backing, bytes;
    } heap_desc{{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, entries, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
                reinterpret_cast<std::uintptr_t>(views.data()), views.size()};
    void* heap = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 14, reinterpret_cast<std::uintptr_t>(&heap_desc),
                                           reinterpret_cast<std::uintptr_t>(&__uuidof(ID3D12DescriptorHeap)),
                                           reinterpret_cast<std::uintptr_t>(&heap))))) return 41;
    const StreamWord cpu = invoke(heap, 9);
    if (cpu != reinterpret_cast<std::uintptr_t>(views.data())) return 42;
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{reinterpret_cast<std::uintptr_t>(constants), sizeof(frame_constants_by_slot[slot])};
    invoke(guest, 17, reinterpret_cast<std::uintptr_t>(&view), cpu);  // CreateConstantBufferView
    const StreamWord table = invoke(heap, 10);
    if (!table) return 43;
    if (reuse) {
        const D3D12_CONSTANT_BUFFER_VIEW_DESC filler{reinterpret_cast<std::uintptr_t>(filler_constants), sizeof(filler_constants)};
        invoke(guest, 17, reinterpret_cast<std::uintptr_t>(&filler), cpu + increment);
        reuse_heap = heap;
        reuse_table = table;
        reuse_step = increment;
        reuse_guest = guest;
    }

    invoke(list, 32, 1, reinterpret_cast<std::uintptr_t>(&heap));  // SetDescriptorHeaps
    invoke(list, 36, 0, table);                                    // SetGraphicsRootDescriptorTable
    invoke(list, 12, 3, 1);                                        // draw into the left half
    if (reuse) {
        filler_draws(list, 100, 0);  // the second half of the frame is recorded by draw_frame_copies_again
        return 0;
    }
    if (wrap) {
        const D3D12_RECT none{0, 0, 0, 0};
        invoke(list, 22, 1, reinterpret_cast<std::uintptr_t>(&none));
        for (int i = 0; i < 64; ++i) {
            constants[0] = 1.0f;  // yellow, a different alpha each time: a fresh copy per draw
            constants[1] = 1.0f;
            constants[2] = 0.0f;
            constants[3] = static_cast<float>(i + 1) / 128.0f;
            invoke(list, 12, 3, 1);
        }
        constants[0] = 0.0f;  // green
        constants[1] = 1.0f;
        constants[2] = 0.0f;
        constants[3] = 1.0f;
    }
    if (changed) {
        constants[0] = 0.0f;  // the same address now holds blue
        constants[2] = 1.0f;
    }
    const D3D12_RECT right{static_cast<LONG>(width / 2), 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    invoke(list, 22, 1, reinterpret_cast<std::uintptr_t>(&right));  // RSSetScissorRects
    invoke(list, 12, 3, 1);                                         // draw into the right half
    return 0;
}

// reuse, the second list (the caller has executed the first, waited for it, and Reset the list with the target and
// pipeline bound again).
int draw_frame_copies_again(void* list, UINT width, UINT height)
{
    if (!reuse_heap) return 44;
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    invoke(list, 21, 1, reinterpret_cast<std::uintptr_t>(&viewport));
    const D3D12_RECT right{static_cast<LONG>(width / 2), 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    invoke(list, 22, 1, reinterpret_cast<std::uintptr_t>(&right));
    invoke(list, 32, 1, reinterpret_cast<std::uintptr_t>(&reuse_heap));
    invoke(list, 36, 0, reuse_table);  // the red constants, unchanged since list A: a frame-copy cache hit
    invoke(list, 12, 3, 1);
    filler_draws(list, 64, 100);
    return 0;
}

// reuse: the ring counters after the second list, for the check script.
int write_ring_counters()
{
    char path[MAX_PATH]{};
    if (!GetEnvironmentVariableA("X2W_TEST_RING_COUNTERS", path, sizeof(path))) return 0;
    HMODULE module = nullptr;
    // The module that implements the guest device (its vtable lives there).
    if (!reuse_guest || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            *static_cast<LPCWSTR*>(reuse_guest), &module))
        return 45;
    using Counter = unsigned long long (*)(int);
    const auto counter = reinterpret_cast<Counter>(GetProcAddress(module, "x2w_upload_ring_test"));
    if (!counter) return 46;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "w") || !file) return 47;
    std::fprintf(file, "reserved=%llu\nchunks=%llu\nmade=%llu\nrepinned=%llu\nstale=%llu\nrefused=%llu\n", counter(0), counter(4),
                 counter(8), counter(9), counter(5), counter(1));
    std::fclose(file);
    return 0;
}
