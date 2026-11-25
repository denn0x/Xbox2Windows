#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace x2w_startup {

using Microsoft::WRL::ComPtr;
using SamplerWords = std::array<std::uint32_t, 4>;
constexpr UINT translated_sampler_count = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
constexpr UINT translated_sampler_space = 1;
constexpr UINT translated_sampler_fallback = translated_sampler_count - 1;
constexpr UINT title_sampler_stride = sizeof(SamplerWords);

// LOCAL (textures): X2W_SAMPLER_LOD=1 also carries the LOD and border fields of the S# words (sampler_lod_fields).
// Off by default: fixtures and manifests written with word 1 = 0 would otherwise be limited to mip 0 (MAX_LOD 0).
inline bool sampler_lod_fields_on()
{
    static const bool on = [] {
        char value[4]{};
        const DWORD saved = GetLastError();
        const bool v = GetEnvironmentVariableA("X2W_SAMPLER_LOD", value, sizeof(value)) && value[0] == '1';
        SetLastError(saved);
        return v;
    }();
    return on;
}

struct SamplerLodFields {
    float bias, min_lod, max_lod;
    D3D12_STATIC_BORDER_COLOR border;
    bool border_register;  // BORDER_COLOR_TYPE 3 (a border colour table entry): not translated, transparent black
};

// Field positions of SQ_IMG_SAMP_WORD1..3 (Mesa src/amd/registers/gfx10-rsrc.json, as cited in tools/pixel_aot.py):
// word 1 bits 11:0 MIN_LOD and 23:12 MAX_LOD, unsigned 4.8 fixed point; word 2 bits 13:0 LOD_BIAS, signed 5.8
// (bits 19:14 LOD_BIAS_SEC, the secondary bias, are not carried); word 3 bits 31:30 BORDER_COLOR_TYPE
// (0 transparent black, 1 opaque black, 2 opaque white, 3 register), numbered as D3D12_STATIC_BORDER_COLOR 0..2.
// MAX_LOD 0xfff (15.996) stays unlimited. D3D12 limits the bias to [-16, 15.99].
inline SamplerLodFields sampler_lod_fields(const SamplerWords& words)
{
    SamplerLodFields f{};
    int bias = static_cast<int>(words[2] & 0x3fff);
    if (bias & 0x2000) bias -= 0x4000;
    f.bias = static_cast<float>(bias) / 256.0f;
    if (f.bias < D3D12_MIP_LOD_BIAS_MIN) f.bias = D3D12_MIP_LOD_BIAS_MIN;
    if (f.bias > D3D12_MIP_LOD_BIAS_MAX) f.bias = D3D12_MIP_LOD_BIAS_MAX;
    f.min_lod = static_cast<float>(words[1] & 0xfff) / 256.0f;
    const UINT max_raw = (words[1] >> 12) & 0xfff;
    f.max_lod = max_raw == 0xfff ? D3D12_FLOAT32_MAX : static_cast<float>(max_raw) / 256.0f;
    if (f.max_lod < f.min_lod) f.max_lod = f.min_lod;
    const UINT type = words[3] >> 30;
    f.border_register = type == 3;
    f.border = type == 3 ? D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK : static_cast<D3D12_STATIC_BORDER_COLOR>(type);
    return f;
}

// comparison: the sampler is read through a SamplerComparisonState (manifest line smpc). SQ_IMG_SAMP_WORD0 bits
// 14:12 hold SQ_TEX_DEPTH_COMPARE, which lists NEVER..ALWAYS in the order of D3D12_COMPARISON_FUNC minus one.
inline D3D12_STATIC_SAMPLER_DESC decode_sampler(const SamplerWords& words, UINT shader_register, bool comparison = false,
                                                bool lod_fields = sampler_lod_fields_on())
{
    static const D3D12_TEXTURE_ADDRESS_MODE modes[8] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
    const UINT mag = (words[2] >> 20) & 3, min = (words[2] >> 22) & 3, mip = (words[2] >> 26) & 3;
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = (mag >= 2 || min >= 2) ? D3D12_FILTER_ANISOTROPIC
        : D3D12_ENCODE_BASIC_FILTER(min & 1, mag & 1, mip == 2 ? 1 : 0, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    s.AddressU = modes[words[0] & 7];
    s.AddressV = modes[(words[0] >> 3) & 7];
    s.AddressW = modes[(words[0] >> 6) & 7];
    s.MaxAnisotropy = 1u << ((words[0] >> 9) & 7);
    if (s.MaxAnisotropy > 16) s.MaxAnisotropy = 16;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (comparison) {
        s.Filter = D3D12_ENCODE_BASIC_FILTER(min & 1, mag & 1, mip == 2 ? 1 : 0, D3D12_FILTER_REDUCTION_TYPE_COMPARISON);
        s.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(((words[0] >> 12) & 7) + 1);
    }
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    if (lod_fields) {
        const auto f = sampler_lod_fields(words);
        s.MipLODBias = f.bias;
        s.MinLOD = f.min_lod;
        s.MaxLOD = f.max_lod;
        s.BorderColor = f.border;
    }
    s.ShaderRegister = shader_register;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

struct TranslatedSamplers {
    std::vector<SamplerWords> statics;
    std::vector<bool> comparison;  // parallel to statics: bound as SamplerComparisonState
    bool heap = false;
    bool reserved_fallback = false;

    bool read_manifest_line(const char* line)
    {
        if (std::strncmp(line, "smpheap", 7) == 0) {
            unsigned count = 0, space = 0, fallback = 0;
            char policy[16]{};
            char trailing = 0;
            const int fields = std::sscanf(line + 7, "%u %u %15s %u %c", &count, &space, policy, &fallback, &trailing);
            if ((line[7] != ' ' && line[7] != '\t') || heap ||
                (fields != 2 && (fields != 4 || std::strcmp(policy, "fallback") != 0 ||
                                 fallback != translated_sampler_fallback)) ||
                count != translated_sampler_count || space != translated_sampler_space)
                return false;
            heap = true;
            reserved_fallback = fields == 4;
        } else {
            int index = 0;
            unsigned words[4] = {};
            const bool compare = std::strncmp(line, "smpc", 4) == 0;
            if (std::sscanf(line, compare ? "smpc %d %x %x %x %x" : "smp %d %x %x %x %x", &index, &words[0], &words[1],
                            &words[2], &words[3]) == 5) {
                statics.push_back({words[0], words[1], words[2], words[3]});
                comparison.push_back(compare);
            }
        }
        return true;
    }
};

// The static sampler bound at register i, in manifest order.
inline std::vector<D3D12_STATIC_SAMPLER_DESC> decode_statics(const TranslatedSamplers& samplers)
{
    std::vector<D3D12_STATIC_SAMPLER_DESC> statics;
    for (UINT i = 0; i < samplers.statics.size(); ++i)
        statics.push_back(decode_sampler(samplers.statics[i], i, i < samplers.comparison.size() && samplers.comparison[i]));
    return statics;
}

inline UINT sampler_root_parameter(UINT srvs) { return srvs ? 2u : 1u; }

inline HRESULT create_translated_root(ID3D12Device* device, UINT constants, UINT srvs,
                                      const TranslatedSamplers& samplers, ID3D12RootSignature** out)
{
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = constants;
    const D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, 0, 0, 0};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &srv_range};
    const D3D12_DESCRIPTOR_RANGE sampler_range{
        D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, translated_sampler_count, 0, translated_sampler_space, 0};
    UINT count = sampler_root_parameter(srvs);
    if (samplers.heap) {
        parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[count].DescriptorTable = {1, &sampler_range};
        ++count;
    }
    const auto statics = decode_statics(samplers);
    const D3D12_ROOT_SIGNATURE_DESC desc{count, parameters, static_cast<UINT>(statics.size()), statics.data(),
                                         D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out));
    return hr;
}

struct SamplerSource {
    std::uintptr_t address = 0;
    UINT count = 0;
};

struct SamplerSnapshot {
    std::array<SamplerWords, translated_sampler_count> words{};
    std::bitset<translated_sampler_count> readable;
    UINT count = 0;
    bool reserved_fallback = false;

    bool operator==(const SamplerSnapshot& other) const
    {
        return reserved_fallback == other.reserved_fallback && count == other.count &&
               readable == other.readable && words == other.words;
    }
};

inline SamplerSnapshot snapshot_samplers(SamplerSource source, bool reserved_fallback)
{
    SamplerSnapshot snapshot;
    snapshot.reserved_fallback = reserved_fallback;
    if (!source.address || source.address > UINTPTR_MAX - translated_sampler_count * title_sampler_stride) return snapshot;
    const UINT capacity = reserved_fallback ? translated_sampler_fallback : translated_sampler_count;
    snapshot.count = source.count < capacity ? source.count : capacity;
    // Read page-sized groups without touching inaccessible memory in-process. A
    // single unaligned record can cross a page, so give it its own read.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page_size = std::uintptr_t{system.dwPageSize};
    for (UINT i = 0; i < snapshot.count;) {
        const auto address = source.address + std::uintptr_t{i} * title_sampler_stride;
        const UINT room = static_cast<UINT>((page_size - address % page_size) / title_sampler_stride);
        const UINT available = snapshot.count - i;
        const UINT run = room ? (room < available ? room : available) : 1;
        SIZE_T copied = 0;
        const SIZE_T bytes = SIZE_T{run} * title_sampler_stride;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &snapshot.words[i], bytes, &copied) &&
            copied == bytes) {
            for (UINT j = i; j < i + run; ++j) snapshot.readable.set(j);
        } else {
            // ReadProcessMemory can copy part of a record on failure.
            for (UINT j = i; j < i + run; ++j) snapshot.words[j] = {};
        }
        i += run;
    }
    return snapshot;
}

inline D3D12_SAMPLER_DESC dynamic_sampler(const SamplerWords& words, bool lod_fields = sampler_lod_fields_on())
{
    const auto s = decode_sampler(words, 0, false, lod_fields);
    D3D12_SAMPLER_DESC d{s.Filter, s.AddressU, s.AddressV, s.AddressW, s.MipLODBias, s.MaxAnisotropy, s.ComparisonFunc,
                         {}, s.MinLOD, s.MaxLOD};
    if (lod_fields) {
        const float rgb = s.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ? 1.0f : 0.0f;
        const float alpha = s.BorderColor == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK ? 0.0f : 1.0f;
        d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = rgb;
        d.BorderColor[3] = alpha;
    }
    return d;
}

inline D3D12_SAMPLER_DESC border_sampler()
{
    return dynamic_sampler({6u | (6u << 3) | (6u << 6), 0, 0, 0}, false);  // the fallback: no LOD limit, whatever the switch
}

struct NullSamplerCounts {
    UINT unbound = 0;
    UINT out_of_range = 0;
    UINT unreadable = 0;
    UINT invalid_token = 0;
};

// A resolver returns false for a readable entry that cannot be represented.
template<class Resolve>
HRESULT create_sampler_snapshot_heap(ID3D12Device* device, const SamplerSnapshot& snapshot, Resolve resolve,
                                     ID3D12DescriptorHeap** out, NullSamplerCounts& counts,
                                     const D3D12_SAMPLER_DESC* reserved = nullptr)
{
    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, translated_sampler_count,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(out));
    if (FAILED(hr)) return hr;
    counts = {};
    const auto fallback = border_sampler();
    auto dest = (*out)->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    const UINT capacity = snapshot.reserved_fallback ? translated_sampler_fallback : translated_sampler_count;
    for (UINT i = 0; i < capacity; ++i) {
        D3D12_SAMPLER_DESC sampler = fallback;
        if (!snapshot.count) ++counts.unbound;
        else if (i >= snapshot.count) ++counts.out_of_range;
        else if (!snapshot.readable.test(i)) ++counts.unreadable;
        else if (!resolve(snapshot.words[i], sampler)) {
            ++counts.invalid_token;
            sampler = fallback;
        }
        device->CreateSampler(&sampler, dest);
        dest.ptr += stride;
    }
    if (snapshot.reserved_fallback) device->CreateSampler(reserved ? reserved : &fallback, dest);
    return S_OK;
}

inline void bind_translated_heaps(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* views,
                                  ID3D12DescriptorHeap* samplers)
{
    ID3D12DescriptorHeap* heaps[]{views, samplers};
    list->SetDescriptorHeaps(samplers ? 2 : 1, heaps);
}

}  // namespace x2w_startup
