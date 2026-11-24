// Device check for src/startup_stubs/console_plane_size.h: both allocation-info entries of the
// startup device wrapper (slots 25 and 108), and a placed texture (slot 133) created over memory of
// the answered size and alignment. usage: startup_plane_size_device_check warp|hardware
#include "xmem_test_device.h"
#include "../src/startup_stubs/console_plane_size.h"

#include <cstring>

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {
HRESULT create_wrapper(IUnknown* adapter, REFIID iid, void** out)
{
    unsigned char parameters[64]{};
    return D3D12XboxCreateDevice(adapter, parameters, iid, out);
}

// The console device's slot numbers; the first argument is the object, the second the result buffer.
using Info25 = void* (*)(void*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*);
using Info108 = void* (*)(void*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*);
using Place133 = HRESULT (*)(void*, void*, const D3D12_RESOURCE_DESC*, UINT, const void*, const GUID*, void**);

template <class F>
F slot(ID3D12Device* device, int index)
{
    return reinterpret_cast<F>((*reinterpret_cast<void***>(device))[index]);
}

D3D12_RESOURCE_DESC texture(DXGI_FORMAT format, UINT width, UINT height, UINT samples, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Flags = flags;
    return desc;
}

bool same(const D3D12_RESOURCE_ALLOCATION_INFO& a, const D3D12_RESOURCE_ALLOCATION_INFO& b)
{
    return a.SizeInBytes == b.SizeInBytes && a.Alignment == b.Alignment;
}

// Returns 0 on success. The expectation is the helper applied to this adapter's own desktop
// answer, so the check holds whether or not the adapter's size is larger than the console's.
int check(const char* name, ID3D12Device* desktop, ID3D12Device* wrapper, const D3D12_RESOURCE_DESC& desc, bool place, int* changed_count)
{
    const D3D12_RESOURCE_ALLOCATION_INFO from_desktop = desktop->GetResourceAllocationInfo(0, 1, &desc);
    D3D12_RESOURCE_ALLOCATION_INFO expected = from_desktop;
    const bool changed = x2w_startup::console_plane_size(desc, expected);

    D3D12_RESOURCE_ALLOCATION_INFO by25{1, 1}, by108{1, 1};
    D3D12_RESOURCE_ALLOCATION_INFO1 each{1, 1, 1};
    slot<Info25>(wrapper, 25)(wrapper, &by25, 0, 1, &desc);
    slot<Info108>(wrapper, 108)(wrapper, &by108, 0, 1, &desc, &each);
    std::printf("%s: desktop size 0x%llx alignment 0x%llx; wrapper slot 25 size 0x%llx alignment 0x%llx (%s)\n", name,
                static_cast<unsigned long long>(from_desktop.SizeInBytes), static_cast<unsigned long long>(from_desktop.Alignment),
                static_cast<unsigned long long>(by25.SizeInBytes), static_cast<unsigned long long>(by25.Alignment),
                changed ? "console size" : "desktop answer kept");
    if (!same(by25, expected) || !same(by108, expected)) {
        std::printf("FAIL %s: slot 25 or slot 108 differs from the expected size 0x%llx alignment 0x%llx (slot 108: 0x%llx, 0x%llx)\n", name,
                    static_cast<unsigned long long>(expected.SizeInBytes), static_cast<unsigned long long>(expected.Alignment),
                    static_cast<unsigned long long>(by108.SizeInBytes), static_cast<unsigned long long>(by108.Alignment));
        return 1;
    }
    if (changed && (each.Offset != 0 || each.SizeInBytes != expected.SizeInBytes || each.Alignment != expected.Alignment)) {
        std::printf("FAIL %s: slot 108 per-resource entry does not describe the answered size\n", name);
        return 1;
    }
    if (changed) ++*changed_count;
    if (!place) return 0;
    if (expected.SizeInBytes == ~0ull || !expected.Alignment) {
        std::printf("FAIL %s: no size to place a texture on\n", name);
        return 1;
    }
    // Memory of exactly the answered size at the answered alignment, as a caller that trusts the answer makes it.
    const SIZE_T bytes = static_cast<SIZE_T>(expected.SizeInBytes), alignment = static_cast<SIZE_T>(expected.Alignment);
    auto* const block = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes + alignment, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block) return 1;
    void* const address = reinterpret_cast<void*>((reinterpret_cast<SIZE_T>(block) + alignment - 1) / alignment * alignment);
    void* placed = nullptr;
    const GUID iid = __uuidof(ID3D12Resource);
    const HRESULT hr = slot<Place133>(wrapper, 133)(wrapper, address, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &iid, &placed);
    const HRESULT removed = wrapper->GetDeviceRemovedReason();
    int result = 0;
    if (FAILED(hr) || !placed || FAILED(removed)) {
        std::printf("FAIL %s: placed texture hr 0x%08lx object %p device 0x%08lx\n", name, static_cast<unsigned long>(hr), placed,
                    static_cast<unsigned long>(removed));
        result = 1;
    }
    if (placed) static_cast<IUnknown*>(placed)->Release();
    VirtualFree(block, 0, MEM_RELEASE);
    return result;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const bool warp = std::strcmp(argv[1], "warp") == 0;
    bool unmatched = false;
    auto desktop = create_device(warp, unmatched);
    if (!desktop) return warp || unmatched ? 1 : 77;
    SetEnvironmentVariableW(L"X2W_D3D12_PERMISSIVE", L"1");
    auto wrapper = create_device(warp, unmatched, create_wrapper);
    if (!wrapper) return 1;

    int failures = 0, changed = 0;
    // Depth with stencil, single sample and four samples; a four-sample colour target.
    failures += check("d32s8 1024x512", desktop.Get(), wrapper.Get(),
                      texture(DXGI_FORMAT_R32G8X24_TYPELESS, 1024, 512, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), true, &changed);
    failures += check("d32s8 200x100 4 samples", desktop.Get(), wrapper.Get(),
                      texture(DXGI_FORMAT_R32G8X24_TYPELESS, 200, 100, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), true, &changed);
    failures += check("rgba8 256x256 4 samples", desktop.Get(), wrapper.Get(),
                      texture(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), true, &changed);
    // Controls outside the rule: the wrapper must answer exactly what desktop answers.
    int control_changed = 0;
    failures += check("control rgba8 512x512", desktop.Get(), wrapper.Get(),
                      texture(DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), true, &control_changed);
    failures += check("control d32 1024x512", desktop.Get(), wrapper.Get(),
                      texture(DXGI_FORMAT_D32_FLOAT, 1024, 512, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), true, &control_changed);
    // A shape outside the rule keeps the desktop answer, failure included: a width no 2D texture can
    // have. (Desktop answers all ones for it; a translation layer may answer a size instead.)
    D3D12_RESOURCE_DESC invalid = texture(DXGI_FORMAT_R32G8X24_TYPELESS, 1024, 512, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    invalid.Width = 1u << 20;
    const D3D12_RESOURCE_ALLOCATION_INFO desktop_refusal = desktop->GetResourceAllocationInfo(0, 1, &invalid);
    D3D12_RESOURCE_ALLOCATION_INFO refused{1, 1};
    slot<Info25>(wrapper.Get(), 25)(wrapper.Get(), &refused, 0, 1, &invalid);
    std::printf("invalid width: desktop size 0x%llx; wrapper size 0x%llx\n", static_cast<unsigned long long>(desktop_refusal.SizeInBytes),
                static_cast<unsigned long long>(refused.SizeInBytes));
    if (!same(refused, desktop_refusal)) {
        std::printf("FAIL invalid width: the wrapper changed the desktop answer\n");
        ++failures;
    }
    if (control_changed) {
        std::printf("FAIL controls: %d description(s) outside the rule were changed\n", control_changed);
        ++failures;
    }
    if (failures) return 1;
    std::printf("startup_plane_size_device: PASS (%d of 3 descriptions answered with the console size on this adapter, "
                "5 placed textures, out-of-rule answers unchanged)\n", changed);
    return 0;
}
