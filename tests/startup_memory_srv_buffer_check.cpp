// Checks validation, offset/element adjustment, and GPU virtual address resolution for
// buffer and raytracing acceleration-structure memory SRVs without a device.
#include "startup_stubs/memory_srv_buffer.h"

#include <cstdio>

namespace {

int failures;

void check(const char* name, bool condition)
{
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", name);
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 width)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC raw_view(UINT64 first_element, UINT num_elements)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.FirstElement = first_element;
    v.Buffer.NumElements = num_elements;
    v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return v;
}

D3D12_SHADER_RESOURCE_VIEW_DESC structured_view(UINT64 first_element, UINT num_elements, UINT stride)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.FirstElement = first_element;
    v.Buffer.NumElements = num_elements;
    v.Buffer.StructureByteStride = stride;
    return v;
}

D3D12_SHADER_RESOURCE_VIEW_DESC typed_view(DXGI_FORMAT format, UINT64 first_element, UINT num_elements)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = format;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.FirstElement = first_element;
    v.Buffer.NumElements = num_elements;
    return v;
}

}  // namespace

int main()
{
    const auto buf = buffer_desc(1024);

    // Raw buffer SRV validation and interior offset resolution.
    const auto raw_ok = raw_view(4, 16);
    check("raw valid", x2w_startup::supports_buffer_memory_srv(buf, raw_ok));
    auto raw_bad_fmt = raw_ok;
    raw_bad_fmt.Format = DXGI_FORMAT_R32_FLOAT;
    check("raw bad format", !x2w_startup::supports_buffer_memory_srv(buf, raw_bad_fmt));
    auto raw_bad_stride = raw_ok;
    raw_bad_stride.Buffer.StructureByteStride = 4;
    check("raw nonzero stride", !x2w_startup::supports_buffer_memory_srv(buf, raw_bad_stride));
    check("raw zero count", !x2w_startup::supports_buffer_memory_srv(buf, raw_view(0, 0)));
    check("raw oob", !x2w_startup::supports_buffer_memory_srv(buf, raw_view(250, 10)));

    D3D12_SHADER_RESOURCE_VIEW_DESC resolved{};
    check("raw resolve base", x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10000, raw_ok, resolved) &&
                                  resolved.Buffer.FirstElement == 4 && resolved.Buffer.NumElements == 16);
    check("raw resolve interior 16B",
          x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10010, raw_ok, resolved) &&
              resolved.Buffer.FirstElement == 8);
    check("raw resolve unaligned 4B",
          !x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10004, raw_view(0, 4), resolved));
    check("raw resolve oob",
          !x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x103e0, raw_view(0, 16), resolved));

    // Structured buffer SRV validation and interior offset resolution.
    const auto str_ok = structured_view(2, 8, 16);
    check("structured valid", x2w_startup::supports_buffer_memory_srv(buf, str_ok));
    auto str_bad_fmt = str_ok;
    str_bad_fmt.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    check("structured bad format", !x2w_startup::supports_buffer_memory_srv(buf, str_bad_fmt));
    check("structured stride not multiple of 4", !x2w_startup::supports_buffer_memory_srv(buf, structured_view(0, 1, 6)));
    check("structured stride 4 ok", x2w_startup::supports_buffer_memory_srv(buf, structured_view(0, 1, 4)));
    check("structured stride too large", !x2w_startup::supports_buffer_memory_srv(buf, structured_view(0, 1, 2052)));
    check("structured resolve interior",
          x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10020, str_ok, resolved) &&
              resolved.Buffer.FirstElement == 4);
    check("structured resolve unaligned",
          !x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10008, str_ok, resolved));

    // Typed buffer SRV validation and interior offset resolution.
    const auto typ_ok = typed_view(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 4);
    check("typed valid", x2w_startup::supports_buffer_memory_srv(buf, typ_ok));
    check("typed unknown format", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_UNKNOWN, 0, 1)));
    check("typed element bytes 16", x2w_startup::typed_buffer_format_bytes(DXGI_FORMAT_R32G32B32A32_FLOAT) == 16);
    check("typed element bytes 12", x2w_startup::typed_buffer_format_bytes(DXGI_FORMAT_R32G32B32_UINT) == 12);
    check("typed element bytes 2", x2w_startup::typed_buffer_format_bytes(DXGI_FORMAT_R16_UNORM) == 2);
    check("typed srgb rejected", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 1)));
    check("typed bgra srgb rejected", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0, 1)));
    check("typed bc rejected", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_BC7_UNORM, 0, 1)));
    check("typed depth rejected", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_D32_FLOAT, 0, 1)));
    check("typed typeless rejected", !x2w_startup::supports_buffer_memory_srv(buf, typed_view(DXGI_FORMAT_R32G32B32A32_TYPELESS, 0, 1)));
    check("typed resolve interior",
          x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10010, typ_ok, resolved) &&
              resolved.Buffer.FirstElement == 2);
    check("typed resolve unaligned",
          !x2w_startup::resolve_buffer_srv_desc(0x10000, 1024, 0x10004, typ_ok, resolved));

    // Denied shader resource flag on buffer resource.
    auto denied_buf = buf;
    denied_buf.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    check("denied srv flag", !x2w_startup::supports_buffer_memory_srv(denied_buf, raw_ok));

    if (failures) return 1;
    std::printf("slot 70 buffer memory SRV validation: all checks passed\n");
    return 0;
}
