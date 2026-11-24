#pragma once

#include <cstdint>
#include <dxgiformat.h>

struct IDXGIAdapter;

struct StartupBc7Input {
    const char* blocks;
    std::uint32_t width, height;
    bool tiled;
    // "resource format:resource flags:view format" in decimal, hex and decimal: instead of BC7 data,
    // a texture of that description is created through slot 133, cleared through a render-target
    // view of the view format and sampled through a shader-resource view of the view format.
    const char* packed = nullptr;
};

int check_stream_draw(IDXGIAdapter* adapter, void* guest, void* pso, const char* pixels,
                      const StartupBc7Input* bc7 = nullptr, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                      std::uint32_t export_word = 0, bool two_targets = false);
