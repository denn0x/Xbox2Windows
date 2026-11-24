// LOCAL experiment (never upstream): the runtime half of the raw-address lowering for container compute
// shaders (out/gamerun/dxil_work/RAWADDR_DESIGN.md). Pure helpers with no state of the stand-in, so the
// offline test tool (dxil_work/rawaddr/rawtool.cpp) runs exactly this code.
//
// A lowered shader reaches title memory only through:
//   b0 space100        kTableDwords root constants: kRegions x {base_lo, base_hi, size}
//   u0..u9 space100    u0..u7 one raw UAV per region, u8 a null view (misses), u9 the miss statistics
// The runtime appends these two parameters to the title's root signature (augmented_root) and fills
// them per dispatch from the seeds in <key>.rawaddr.txt (parse_sidecar).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x2w_startup::rawaddr {

constexpr unsigned kRegions = 8;
constexpr unsigned kSpace = 100;
constexpr unsigned kNullSlot = kRegions;
constexpr unsigned kStatsSlot = kRegions + 1;
constexpr unsigned kSlots = kRegions + 2;
constexpr unsigned kTableDwords = 3 * kRegions;
// Statistics slot written by the shader: [0] miss count, [16 + 8 * (n & 7)] the missed address (lo, hi).
constexpr unsigned kStatsBytes = 128;
constexpr unsigned kStatsRing = 8;

enum class SeedKind { rootconst64, rootconst32, rootdesc };
struct Seed {
    SeedKind kind;
    unsigned param = 0, word = 0, shift = 0;
};

// Parses a sidecar; false when it is not a version-1 file with the expected table shape.
inline bool parse_sidecar(const std::string& text, std::vector<Seed>& seeds)
{
    seeds.clear();
    bool version = false, shape = false;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        unsigned a = 0, b = 0, c = 0;
        char kind[24] = "";
        if (line.rfind("rawaddr 1", 0) == 0) {
            version = true;
        } else if (std::sscanf(line.c_str(), "slots %u space %u", &a, &b) == 2) {
            shape = a == kRegions && b == kSpace;
        } else if (std::sscanf(line.c_str(), "seed %23s %u %u %u", kind, &a, &b, &c) >= 2) {
            Seed s{};
            if (!std::strcmp(kind, "rootconst64")) s = {SeedKind::rootconst64, a, b, 0};
            else if (!std::strcmp(kind, "rootconst32")) s = {SeedKind::rootconst32, a, b, c};
            else if (!std::strcmp(kind, "rootdesc")) s = {SeedKind::rootdesc, a, 0, 0};
            else return false;
            if (s.param >= 16 || s.word >= 16 || (s.kind == SeedKind::rootconst64 && s.word >= 15) || s.shift >= 40) return false;
            seeds.push_back(s);
        }
    }
    return version && shape;
}

// The title address a seed names, from the title's root state (16 parameters x 16 constant words, and the
// title address of each root descriptor). 0 when the parameter is not of the expected kind.
inline std::uint64_t seed_address(const Seed& s, const int kind[16], const std::uint32_t constants[16][16], const std::uint64_t address[16])
{
    if (s.param >= 16) return 0;
    switch (s.kind) {
    case SeedKind::rootconst64:
        return kind[s.param] == 1 && s.word < 15 ? (constants[s.param][s.word] | (std::uint64_t{constants[s.param][s.word + 1]} << 32)) : 0;
    case SeedKind::rootconst32:
        return kind[s.param] == 1 && s.word < 16 ? (std::uint64_t{constants[s.param][s.word]} << s.shift) : 0;
    case SeedKind::rootdesc:
        return kind[s.param] == 3 || kind[s.param] == 4 ? address[s.param] : 0;
    }
    return 0;
}

// One region of the table: the title address the view's first byte stands for, and the bytes it covers.
// A raw view must start 16-byte aligned, so the window offset is rounded down and the difference is folded
// into the base (the shader then also accepts the 0..15 bytes before the title address: still the same
// window memory).
struct Region {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    std::uint64_t view_offset = 0;  // byte offset of the view in the window buffer (16-byte aligned)
};
inline Region make_region(std::uint64_t title_address, std::uint64_t bytes, std::uint64_t window_offset)
{
    Region r;
    const std::uint64_t slack = window_offset & 15;
    r.view_offset = window_offset - slack;
    const std::uint64_t size = bytes + slack;
    r.size = static_cast<std::uint32_t>(size > 0xfffffff0ull ? 0xfffffff0ull : size) & ~3u;
    r.base = title_address - slack;
    return r;
}
inline bool region_contains(const Region& r, std::uint64_t address) { return r.size && address - r.base < r.size; }

inline void fill_table(const Region* regions, unsigned count, std::uint32_t words[kTableDwords])
{
    std::memset(words, 0, sizeof(std::uint32_t) * kTableDwords);
    for (unsigned k = 0; k < count && k < kRegions; ++k) {
        words[3 * k] = static_cast<std::uint32_t>(regions[k].base);
        words[3 * k + 1] = static_cast<std::uint32_t>(regions[k].base >> 32);
        words[3 * k + 2] = regions[k].size;
    }
}

// The title root signature plus [n] kTableDwords constants b0 space100 and [n + 1] a table of kSlots UAVs
// u0 space100. The heap-directly-indexed flags are cleared as device_create_root_signature does.
// *first_extra = n. Returns the serialized blob (caller releases) or null; *error gets the failing step.
inline ID3DBlob* augmented_root(const void* blob, std::size_t bytes, UINT* first_extra, const char** error)
{
    *error = nullptr;
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, bytes, IID_PPV_ARGS(&reader)))) {
        *error = "deserialize";
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* source = nullptr;
    if (FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &source)) || !source) {
        reader->Release();
        *error = "version 1.1";
        return nullptr;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& in = source->Desc_1_1;
    std::vector<D3D12_ROOT_PARAMETER1> params(in.pParameters, in.pParameters + in.NumParameters);
    for (const auto& p : params)
        if ((p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && [&] {
                 for (UINT i = 0; i < p.DescriptorTable.NumDescriptorRanges; ++i)
                     if (p.DescriptorTable.pDescriptorRanges[i].RegisterSpace == kSpace) return true;
                 return false;
             }()) ||
            ((p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) && p.Constants.RegisterSpace == kSpace) ||
            ((p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV || p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
              p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) && p.Descriptor.RegisterSpace == kSpace)) {
            reader->Release();
            *error = "title root uses register space 100";
            return nullptr;
        }
    *first_extra = static_cast<UINT>(params.size());
    D3D12_ROOT_PARAMETER1 table{};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    table.Constants = {0, kSpace, kTableDwords};
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(table);
    static const D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kSlots, 0, kSpace,
                                               D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    D3D12_ROOT_PARAMETER1 uavs{};
    uavs.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    uavs.DescriptorTable = {1, &range};
    uavs.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(uavs);
    constexpr UINT indexed = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC out{};
    out.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    out.Desc_1_1 = {static_cast<UINT>(params.size()), params.data(), in.NumStaticSamplers, in.pStaticSamplers,
                    static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(in.Flags & ~indexed)};
    ID3DBlob* result = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(&out, &result, &errors);
    if (errors) errors->Release();
    reader->Release();  // owns source: released only after serialization
    if (FAILED(hr)) {
        *error = "serialize (over 64 root dwords?)";
        return nullptr;
    }
    return result;
}

}  // namespace x2w_startup::rawaddr
