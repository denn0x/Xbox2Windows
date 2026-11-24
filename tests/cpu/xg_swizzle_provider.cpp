// Test-only recording provider for xg_xs.dll exports (#870, #1032):
//   xg_xs.dll!XGSuggestSwizzleMode
//   xg_xs.dll!XGCreateTextureComputer
//
// Public sources:
// - DirectXTex (DirectXTexXboxTile.cpp at a8e5dbf, MIT):
//   Caller passes texture descriptor with unknown layout and selector flags (literal 0),
//   receives suggested layout mode, and feeds it into texture computer.
// - DirectXTex Issue #498 (https://github.com/microsoft/DirectXTex/issues/498):
//   Public three-argument call shape:
//     HRESULT XGSuggestSwizzleMode(const D3D12_RESOURCE_DESC*, UINT, D3D12_TEXTURE_LAYOUT* out)
//   Layout encoding: 0x100 | mode (or 0x100 for linear).
// - Full texel reference (#979, #1032):
//   AddrLib observations on gfx10.3 profile (4KB_D_X / 64KB_R_X).
//
// Labeled as fixture behavior / policy:
// - PE imports: exposing these APIs as xg_xs.dll exports is a fixture choice.
// - Numeric swizzle modes (0x105 for tiled, 0x100 for linear) and the 147-texel Morton
//   bit-order oracle are fixture policies for verifying caller/provider handshake only.
//   They are not an independent match to #979 and not evidence of console layout encoding.
//   See #1032 for the full texel reference using pinned PC AddrLib.
// - FAULT_CLOBBER: mutant provider that overwrites the output pointer on invalid-descriptor refusal.
// - FAULT_LINEAR: mutant provider that bypasses swizzling and always copies linear texels.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {
std::atomic<std::uint64_t> g_suggest_calls{0};
std::atomic<std::uint64_t> g_create_computer_calls{0};
std::atomic<std::uint64_t> g_copy_into_calls{0};
std::atomic<std::uint64_t> g_copy_from_calls{0};
std::atomic<std::uint64_t> g_invalid_refusals{0};
std::atomic<std::uint32_t> g_last_suggested_mode{0};

uint32_t morton2d(uint32_t x, uint32_t y)
{
    uint32_t res = 0;
    for (int i = 0; i < 16; ++i) {
        res |= ((x & (1u << i)) << i) | ((y & (1u << i)) << (i + 1));
    }
    return res;
}

// Minimal COM-style computer interface for fixture testing matching public XG layout methods
struct IXGTextureComputer;

struct XGTextureComputerVtbl {
    ULONG(STDMETHODCALLTYPE* AddRef)(IXGTextureComputer* This);
    ULONG(STDMETHODCALLTYPE* Release)(IXGTextureComputer* This);
    HRESULT(STDMETHODCALLTYPE* GetResourceLayout)(IXGTextureComputer* This, void* pLayout);
    UINT64(STDMETHODCALLTYPE* GetResourceSizeBytes)(IXGTextureComputer* This);
    UINT64(STDMETHODCALLTYPE* GetResourceBaseAlignmentBytes)(IXGTextureComputer* This);
    UINT64(STDMETHODCALLTYPE* GetMipLevelOffsetBytes)(IXGTextureComputer* This, UINT plane, UINT mip);
    UINT64(STDMETHODCALLTYPE* GetTexelElementOffsetBytes)(
        IXGTextureComputer* This, UINT plane, UINT mip, UINT64 x, UINT y, UINT slice, UINT sample, UINT* pBit);
    HRESULT(STDMETHODCALLTYPE* CopyIntoSubresource)(
        IXGTextureComputer* This, void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch);
    HRESULT(STDMETHODCALLTYPE* CopyFromSubresource)(
        IXGTextureComputer* This, void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch);
};

struct IXGTextureComputer {
    const XGTextureComputerVtbl* lpVtbl;
};

class TextureComputerImpl : public IXGTextureComputer {
public:
    TextureComputerImpl(const D3D12_RESOURCE_DESC& desc)
        : m_refCount(1)
        , m_desc(desc)
    {
        lpVtbl = &s_vtbl;
#ifdef FAULT_LINEAR
        m_isTiled = false;
#else
        // If layout specifies linear (ROW_MAJOR / 1 or 0x100), not tiled
        m_isTiled = (desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR && desc.Layout != static_cast<D3D12_TEXTURE_LAYOUT>(0x100));
#endif
        computeOffsets();
    }

    ULONG STDMETHODCALLTYPE AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release()
    {
        ULONG res = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
        if (res == 0) {
            delete this;
        }
        return res;
    }

    HRESULT STDMETHODCALLTYPE GetResourceLayout(void* pLayout)
    {
        if (!pLayout) return E_POINTER;
        std::memset(pLayout, 0, 64);
        return S_OK;
    }

    UINT64 STDMETHODCALLTYPE GetResourceSizeBytes()
    {
        return m_isTiled ? 65536 : m_totalSize;
    }

    UINT64 STDMETHODCALLTYPE GetResourceBaseAlignmentBytes()
    {
        return m_isTiled ? 65536 : 256;
    }

    UINT64 STDMETHODCALLTYPE GetMipLevelOffsetBytes(UINT plane, UINT mip)
    {
        (void)plane;
        if (mip >= 3) return 0;
        return m_mipOffsets[mip];
    }

    UINT64 STDMETHODCALLTYPE GetTexelElementOffsetBytes(
        UINT plane, UINT mip, UINT64 x, UINT y, UINT slice, UINT sample, UINT* pBit)
    {
        (void)plane;
        (void)slice;
        (void)sample;
        if (pBit) *pBit = 0;
        if (mip >= 3) return 0;
        UINT64 base = m_mipOffsets[mip];
        if (m_isTiled) {
            return base + morton2d(static_cast<uint32_t>(x), y) * 4;
        } else {
            uint32_t mw = std::max(1u, static_cast<uint32_t>(m_desc.Width >> mip));
            return base + (y * mw + static_cast<uint32_t>(x)) * 4;
        }
    }

    HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch)
    {
        (void)plane;
        (void)slicePitch;
        if (!dst || !src) return E_INVALIDARG;
        if (subresource >= m_desc.MipLevels) return E_INVALIDARG;

        g_copy_into_calls.fetch_add(1);
        uint32_t mw = std::max(1u, static_cast<uint32_t>(m_desc.Width >> subresource));
        uint32_t mh = std::max(1u, static_cast<uint32_t>(m_desc.Height >> subresource));
        uint8_t* dstBytes = static_cast<uint8_t*>(dst) + m_mipOffsets[subresource];
        const uint8_t* srcBytes = static_cast<const uint8_t*>(src);

        for (uint32_t y = 0; y < mh; ++y) {
            for (uint32_t x = 0; x < mw; ++x) {
                uint32_t srcTexel = *reinterpret_cast<const uint32_t*>(srcBytes + y * rowPitch + x * 4);
                if (m_isTiled) {
                    uint32_t tiledIdx = morton2d(x, y);
                    reinterpret_cast<uint32_t*>(dstBytes)[tiledIdx] = srcTexel;
                } else {
                    reinterpret_cast<uint32_t*>(dstBytes)[y * mw + x] = srcTexel;
                }
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch)
    {
        (void)plane;
        (void)slicePitch;
        if (!dst || !src) return E_INVALIDARG;
        if (subresource >= m_desc.MipLevels) return E_INVALIDARG;

        g_copy_from_calls.fetch_add(1);
        uint32_t mw = std::max(1u, static_cast<uint32_t>(m_desc.Width >> subresource));
        uint32_t mh = std::max(1u, static_cast<uint32_t>(m_desc.Height >> subresource));
        const uint8_t* srcBytes = static_cast<const uint8_t*>(src) + m_mipOffsets[subresource];
        uint8_t* dstBytes = static_cast<uint8_t*>(dst);

        for (uint32_t y = 0; y < mh; ++y) {
            for (uint32_t x = 0; x < mw; ++x) {
                uint32_t texelVal;
                if (m_isTiled) {
                    uint32_t tiledIdx = morton2d(x, y);
                    texelVal = reinterpret_cast<const uint32_t*>(srcBytes)[tiledIdx];
                } else {
                    texelVal = reinterpret_cast<const uint32_t*>(srcBytes)[y * mw + x];
                }
                *reinterpret_cast<uint32_t*>(dstBytes + y * rowPitch + x * 4) = texelVal;
            }
        }
        return S_OK;
    }

private:
    void computeOffsets()
    {
        m_mipOffsets[0] = 0;
        // For w=28, h=4: Mip 0 is 112 texels = 448 bytes.
        // Mip 1 starts at 512 bytes (aligned)
        // Mip 1 is 28 texels = 112 bytes
        // Mip 2 starts at 640 bytes
        // Mip 2 is 7 texels = 28 bytes
        m_mipOffsets[1] = 512;
        m_mipOffsets[2] = 640;
        m_totalSize = 668;
    }

    LONG m_refCount;
    D3D12_RESOURCE_DESC m_desc;
    bool m_isTiled;
    uint64_t m_mipOffsets[3];
    uint64_t m_totalSize;

    static const XGTextureComputerVtbl s_vtbl;
};

const XGTextureComputerVtbl TextureComputerImpl::s_vtbl = {
    [](IXGTextureComputer* This) { return static_cast<TextureComputerImpl*>(This)->AddRef(); },
    [](IXGTextureComputer* This) { return static_cast<TextureComputerImpl*>(This)->Release(); },
    [](IXGTextureComputer* This, void* pLayout) { return static_cast<TextureComputerImpl*>(This)->GetResourceLayout(pLayout); },
    [](IXGTextureComputer* This) { return static_cast<TextureComputerImpl*>(This)->GetResourceSizeBytes(); },
    [](IXGTextureComputer* This) { return static_cast<TextureComputerImpl*>(This)->GetResourceBaseAlignmentBytes(); },
    [](IXGTextureComputer* This, UINT plane, UINT mip) { return static_cast<TextureComputerImpl*>(This)->GetMipLevelOffsetBytes(plane, mip); },
    [](IXGTextureComputer* This, UINT plane, UINT mip, UINT64 x, UINT y, UINT slice, UINT sample, UINT* pBit) {
        return static_cast<TextureComputerImpl*>(This)->GetTexelElementOffsetBytes(plane, mip, x, y, slice, sample, pBit);
    },
    [](IXGTextureComputer* This, void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch) {
        return static_cast<TextureComputerImpl*>(This)->CopyIntoSubresource(dst, plane, subresource, src, rowPitch, slicePitch);
    },
    [](IXGTextureComputer* This, void* dst, UINT plane, UINT subresource, const void* src, UINT rowPitch, UINT slicePitch) {
        return static_cast<TextureComputerImpl*>(This)->CopyFromSubresource(dst, plane, subresource, src, rowPitch, slicePitch);
    },
};

} // namespace

extern "C" {

HRESULT WINAPI XGSuggestSwizzleMode(
    const D3D12_RESOURCE_DESC* pDesc,
    UINT flags,
    D3D12_TEXTURE_LAYOUT* pLayout)
{
    (void)flags;
    if (!pDesc || !pLayout) {
        return E_POINTER;
    }

    // Refusal for invalid descriptor (e.g. Width == 0, Height == 0, MipLevels == 0)
    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->MipLevels == 0) {
#ifdef FAULT_CLOBBER
        // Fault control: clobber the output sentinel on refusal
        *pLayout = static_cast<D3D12_TEXTURE_LAYOUT>(0xBAD0F00D);
#endif
        g_invalid_refusals.fetch_add(1);
        return static_cast<HRESULT>(0x80070057); // E_INVALIDARG
    }

    // Determine layout mode
    // If resource is linear (D3D12_TEXTURE_LAYOUT_ROW_MAJOR / 1), suggest 0x100 (linear)
    // Otherwise, suggest 0x105 (4KB_D_X / 64KB mode per #979)
    D3D12_TEXTURE_LAYOUT suggested;
    if (pDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || pDesc->Layout == static_cast<D3D12_TEXTURE_LAYOUT>(0x100)) {
        suggested = static_cast<D3D12_TEXTURE_LAYOUT>(0x100);
    } else {
        suggested = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
    }

    *pLayout = suggested;
    g_last_suggested_mode.store(static_cast<uint32_t>(suggested));
    g_suggest_calls.fetch_add(1);
    return S_OK;
}

HRESULT WINAPI XGCreateTextureComputer(
    const D3D12_RESOURCE_DESC* pDesc,
    IXGTextureComputer** ppComputer)
{
    if (!pDesc || !ppComputer) {
        return E_POINTER;
    }

    // Refusal control for invalid descriptor
    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->MipLevels == 0) {
#ifdef FAULT_CLOBBER
        *ppComputer = reinterpret_cast<IXGTextureComputer*>(static_cast<uintptr_t>(0xBAD0F00D));
#endif
        g_invalid_refusals.fetch_add(1);
        return static_cast<HRESULT>(0x80070057); // E_INVALIDARG
    }

    *ppComputer = new TextureComputerImpl(*pDesc);
    g_create_computer_calls.fetch_add(1);
    return S_OK;
}

// Introspection counters for test harness
uint64_t WINAPI get_xg_suggest_calls() { return g_suggest_calls.load(); }
uint64_t WINAPI get_xg_create_computer_calls() { return g_create_computer_calls.load(); }
uint64_t WINAPI get_xg_copy_into_calls() { return g_copy_into_calls.load(); }
uint64_t WINAPI get_xg_copy_from_calls() { return g_copy_from_calls.load(); }
uint64_t WINAPI get_xg_invalid_refusals() { return g_invalid_refusals.load(); }
uint32_t WINAPI get_xg_last_suggested_mode() { return g_last_suggested_mode.load(); }

} // extern "C"
