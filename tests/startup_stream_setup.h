#pragma once
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

int prepare_stream_pipeline(const char* vs_path, const char* ps_path, const char* root_path,
                            bool hardware, bool textured, Microsoft::WRL::ComPtr<IDXGIAdapter>& adapter,
                            void*& device, void*& pso);

struct StreamAttachment {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_bytes = 0;
};

struct StreamTarget {
    std::array<StreamAttachment, 2> attachments;
    UINT count{};
    void* rtv_heap{};
    void* list{};
    void* queue{};
    void* allocator{};
    void* fence{};
};
int read_stream_target(ID3D12Device* desktop, StreamTarget& state, const char* pixels);
int open_stream_target(ID3D12Device* desktop, void* guest, void* pso, UINT width, UINT height,
                       DXGI_FORMAT format, UINT scissor_width, StreamTarget& state);
using StreamWord = std::uint64_t;
StreamWord invoke(void*, unsigned, StreamWord = 0, StreamWord = 0, StreamWord = 0, StreamWord = 0,
                  StreamWord = 0, StreamWord = 0, StreamWord = 0, StreamWord = 0);
