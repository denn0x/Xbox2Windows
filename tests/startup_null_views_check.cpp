#include "xmem_test_device.h"
#include "../src/startup_stubs/null_texture_view.h"
#include "null_binding_cause_cases.h"

#include <cstring>

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

HRESULT create_wrapper(IUnknown* adapter, REFIID iid, void** out)
{
    unsigned char parameters[64]{};
    return D3D12XboxCreateDevice(adapter, parameters, iid, out);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    if (!null_binding_cause_cases()) return 1;
    const bool warp = std::strcmp(argv[1], "warp") == 0;
    bool unmatched = false;
    auto device = create_device(warp, unmatched);
    if (!device) return warp || unmatched ? 1 : 77;

    D3D12_UNORDERED_ACCESS_VIEW_DESC source{};
    source.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    source.Format = DXGI_FORMAT_B5G5R5A1_UNORM;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{source.Format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) return 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC adapted{};
    auto adapt = [&](bool resource, bool counter) {
        return x2w_local::adapt_null_view(device.Get(), resource, counter, source, adapted);
    };
    if (adapt(true, false) || adapt(false, true)) return 1;
    source.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    if (adapt(false, false)) return 1;
    source.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    source.Format = DXGI_FORMAT_R32_UINT;
    if (adapt(false, false)) return 1;
    source.Format = DXGI_FORMAT_B5G5R5A1_UNORM;
    const bool changed = adapt(false, false);
    if (changed == !!(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) return 1;
    if (changed && (adapted.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
                    adapted.ViewDimension != source.ViewDimension)) return 1;
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    D3D12_RENDER_TARGET_VIEW_DESC desktop_rtv{};
    rtv.Format = DXGI_FORMAT_B4G4R4A4_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    if (x2w_local::adapt_null_view(device.Get(), false, rtv, desktop_rtv)) return 1;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (x2w_local::adapt_null_view(device.Get(), true, rtv, desktop_rtv)) return 1;
    support = {rtv.Format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) return 1;
    const bool rtv_changed = x2w_local::adapt_null_view(device.Get(), false, rtv, desktop_rtv);
    if (rtv_changed == !!(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) return 1;
    if (rtv_changed && (desktop_rtv.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
                        desktop_rtv.ViewDimension != rtv.ViewDimension)) return 1;

    SetEnvironmentVariableW(L"X2W_D3D12_PERMISSIVE", L"1");
    auto wrapper = create_device(warp, unmatched, create_wrapper);
    if (!wrapper) return 1;
    alignas(64) unsigned char guest_descriptors[2][256]{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heaps[2];
    for (int i = 0; i < 2; ++i) {
        struct {
            D3D12_DESCRIPTOR_HEAP_DESC desc;
            SIZE_T guest_address;
            SIZE_T bytes;
        } heap{};
        heap.desc.Type = i == 0 ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.desc.NumDescriptors = 1;
        heap.guest_address = reinterpret_cast<SIZE_T>(guest_descriptors[i]);
        heap.bytes = device->GetDescriptorHandleIncrementSize(heap.desc.Type);
        if (!heap.bytes || heap.bytes > sizeof(guest_descriptors[i])) return 1;
        if (FAILED(wrapper->CreateDescriptorHeap(&heap.desc, IID_PPV_ARGS(&heaps[i])))) return 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dest{heap.guest_address};
        if (i == 0) wrapper->CreateUnorderedAccessView(nullptr, nullptr, &source, dest);
        else wrapper->CreateRenderTargetView(nullptr, &rtv, dest);
        if (FAILED(wrapper->GetDeviceRemovedReason())) return 1;
    }
    std::printf("slot 19 format %u -> %u; slot 20 format %u -> %u; device OK\n",
                source.Format, changed ? adapted.Format : source.Format,
                rtv.Format, rtv_changed ? desktop_rtv.Format : rtv.Format);
    return 0;
}
