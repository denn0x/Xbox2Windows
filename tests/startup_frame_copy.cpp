#include <windows.h>

#include <array>
#include <cstdint>
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
int draw_frame_copies(ID3D12Device* desktop, void* guest, void* list, UINT width, UINT height)
{
    char mode[16]{};
    GetEnvironmentVariableA("X2W_TEST_FRAME_COPY", mode, sizeof(mode));
    const bool changed = !std::strcmp(mode, "changed");
    const bool wrap = !std::strcmp(mode, "wrap");
    if (!changed && !wrap && std::strcmp(mode, "same")) return 2;

    alignas(256) static float constants[64] = {1.0f, 0.0f, 0.0f, 1.0f};
    const UINT increment = desktop->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT entries = (32 + increment - 1) / increment;
    static std::vector<unsigned char> views;
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
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{reinterpret_cast<std::uintptr_t>(constants), sizeof(constants)};
    invoke(guest, 17, reinterpret_cast<std::uintptr_t>(&view), cpu);  // CreateConstantBufferView
    const StreamWord table = invoke(heap, 10);
    if (!table) return 43;

    invoke(list, 32, 1, reinterpret_cast<std::uintptr_t>(&heap));  // SetDescriptorHeaps
    invoke(list, 36, 0, table);                                    // SetGraphicsRootDescriptorTable
    invoke(list, 12, 3, 1);                                        // draw into the left half
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
