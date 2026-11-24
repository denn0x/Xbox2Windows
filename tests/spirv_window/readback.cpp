#include "../desktop_gpu.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
struct WindowCase {
    const char* name;
    std::array<UINT, 4> parameters;  // first base, second address, destination address, first base high
    UINT expected;
    int atomic_word = -1;
};

constexpr UINT left = 0xfedcba98u;
constexpr UINT right = 0x12345678u;
constexpr WindowCase cases[] = {
    {"baseline", {8, 28, 44, 0}, left - right},
    {"swapped", {24, 12, 44, 0}, right - left},
    {"moved output", {8, 28, 52, 0}, left - right},
    {"high address", {8, 28, 44, 1}, 0u - right},
    {"unaligned", {9, 28, 44, 0}, 0u - right},
    {"index addition wraps", {0xfffffffcu, 28, 44, 0xffffffffu}, 0u - right},
    {"unaligned write", {8, 28, 45, 0}, 0},
    {"write beyond descriptor", {8, 28, 64, 0}, 0},
};

constexpr WindowCase atomic_cases[] = {
    // Base low, addend, old-value destination, base high. The shader adds one DWORD to the base.
    {"atomic valid", {8, 7, 44, 0}, 0, 3},
    {"atomic misaligned", {9, 7, 44, 0}, 0},
    {"atomic out of window", {0xfffffff8u, 7, 44, 0}, 0},
    {"atomic index addition wraps", {0xfffffffcu, 7, 44, 0xffffffffu}, 0},
};

constexpr UINT arena_fill = 0xcba98765u;

// A signed case: base low, operand bits, destination byte address, base high. The shader runs SMin on the DWORD at
// base + 4, SMax at base + 8 and a plain load at base + 12, then a sequence on the DWORD at base + 16: add and
// exchange of the operand, and with 0xffff0000, or with 0x0f0f, xor with 0x0ff00ff0, a compare-exchange that misses
// (comparator off in the sign bit only) and one that hits (new value 0xfffe1dc0). It stores the ten old values at
// the destination address and the nine DWORDs after it.
struct SignedCase {
    const char* name;
    std::array<UINT, 4> parameters;
};

constexpr SignedCase signed_cases[] = {
    {"signed positive operand", {4, 2, 24, 0}},
    {"signed negative operand", {4, 0xfffffff6u, 24, 0}},
    {"signed minimum operand", {4, 0x80000000u, 24, 0}},
    {"signed maximum operand", {4, 0x7fffffffu, 24, 0}},
    {"signed add crosses zero", {4, 9, 24, 0}},
    {"signed misaligned base", {5, 2, 24, 0}},
    {"signed window end", {0xfffffff4u, 2, 24, 0}},
    {"signed index addition wraps", {0xfffffffcu, 2, 24, 0xffffffffu}},
    {"signed misaligned write", {4, 2, 25, 0}},
    {"signed write beyond descriptor", {4, 2, 64, 0}},
    {"signed write above the window", {4, 2, 0xfffffffcu, 0}},
    {"signed write straddling the end of the descriptor", {4, 2, 52, 0}},
};

// A float case: base low, operand bits, destination byte address, base high. The shader loads the floats at base + 4,
// base + 8 and base + 12, stores them at the destination address and the two words after it, then stores the operand
// at the fourth word. Every move is a bit-cast, so a NaN payload and a negative zero must arrive unchanged.
struct FloatCase {
    const char* name;
    std::array<UINT, 4> parameters;
};

constexpr UINT float_pi = 0x40490fdbu;
constexpr FloatCase float_cases[] = {
    {"float aligned", {8, float_pi, 44, 0}},
    {"float NaN operand", {8, 0xffc00001u, 44, 0}},
    {"float negative zero operand", {8, 0x80000000u, 44, 0}},
    {"float misaligned base", {9, float_pi, 44, 0}},
    {"float window end", {0xfffffff4u, float_pi, 44, 0}},
    {"float base at the end of the window", {0xfffffffcu, float_pi, 44, 0}},
    {"float index addition wraps", {0xfffffffcu, float_pi, 44, 0xffffffffu}},
    {"float misaligned write", {8, float_pi, 45, 0}},
    {"float write beyond descriptor", {8, float_pi, 64, 0}},
    {"float write above the window", {8, float_pi, 0xfffffffcu, 0}},
    {"float write straddling the end of the descriptor", {8, float_pi, 60, 0}},
};

// A vec4 case: base low, operand bits, destination byte address, base high. The shader loads a four-float vector directly
// at the base (A) and another through an array index at base + 16 (B), builds M from the operand and lanes of both
// (operand, B.z, A.y, A.x), then stores A at the destination, B at destination + 16 and M at destination + 32. Each
// access is four bit-cast DWORDs under one guard: all four move or none does, so a lane-order error, a NaN payload
// and a negative zero are visible, and a vector that is misaligned or crosses the window end never moves partly.
struct Vec4Case {
    const char* name;
    std::array<UINT, 4> parameters;
};

constexpr UINT vec4_pi = 0x40490fdbu;
constexpr UINT vec4_nan = 0xffc00001u;
constexpr UINT vec4_negative_zero = 0x80000000u;
// The production window ends at the last DWORD below 0xfffffffc; the narrow window ends at byte 44, inside the
// 64-byte view and inside the vector at byte 32, so a guard that checked only a vector's first DWORD would show.
constexpr uint64_t vec4_window_end = 0xfffffffcull;
constexpr uint64_t vec4_narrow_window_end = 44;

constexpr Vec4Case vec4_cases[] = {
    {"vec4 aligned", {16, vec4_pi, 0, 0}},
    {"vec4 NaN operand", {16, vec4_nan, 0, 0}},
    {"vec4 negative zero operand", {16, vec4_negative_zero, 0, 0}},
    {"vec4 base misaligned by 4", {20, vec4_pi, 0, 0}},
    {"vec4 base misaligned by 8", {24, vec4_pi, 0, 0}},
    {"vec4 base misaligned by 12", {28, vec4_pi, 0, 0}},
    {"vec4 load in the last vector of the view", {32, vec4_pi, 0, 0}},
    {"vec4 load across the end of the descriptor", {48, vec4_pi, 0, 0}},
    {"vec4 last vector of the window", {0xffffffd0u, vec4_pi, 0, 0}},
    {"vec4 straddling the end of the window", {0xffffffe0u, vec4_pi, 0, 0}},
    {"vec4 starting at the end of the window", {0xfffffff0u, vec4_pi, 0, 0}},
    {"vec4 index addition wraps", {0xfffffff0u, vec4_pi, 0, 0xffffffffu}},
    {"vec4 misaligned write by 4", {16, vec4_pi, 4, 0}},
    {"vec4 misaligned write by 8", {16, vec4_pi, 8, 0}},
    {"vec4 misaligned write by 12", {16, vec4_pi, 12, 0}},
    {"vec4 write beyond descriptor", {16, vec4_pi, 64, 0}},
    {"vec4 write above the window", {16, vec4_pi, 0xfffffff0u, 0}},
    {"vec4 write across the end of the descriptor", {16, vec4_pi, 48, 0}},
};

constexpr Vec4Case vec4_narrow_cases[] = {
    {"narrow vec4 below the end", {0, vec4_pi, 0, 0}},
    {"narrow vec4 load across the end", {16, vec4_pi, 0, 0}},
    {"narrow vec4 load past the end", {32, vec4_pi, 0, 0}},
    {"narrow vec4 store across the end", {0, vec4_pi, 16, 0}},
    {"narrow vec4 store past the end", {0, vec4_pi, 48, 0}},
};

// A QWORD case: base low, source QWORD index, destination byte address, base high. The shader adds the index
// scaled by eight to the base, loads a QWORD there, adds one and stores it at the destination address.
struct QwordCase {
    const char* name;
    std::array<UINT, 4> parameters;
};

// The first QWORD that must not be accessed. The production window ends at the last QWORD below the final DWORD;
// the narrow window ends inside the 64-byte view so a stray access to an address past it would be visible.
constexpr uint64_t qword_window_end = 0xfffffff8u;
constexpr uint64_t narrow_window_end = 40;

constexpr QwordCase qword_cases[] = {
    {"qword baseline", {0, 1, 32, 0}},
    {"qword second source", {0, 3, 40, 0}},
    {"qword base offset", {8, 0, 32, 0}},
    {"qword load aligned to a DWORD only", {4, 1, 32, 0}},
    {"qword load above the window", {0xfffffff8u, 0, 32, 0}},
    {"qword load above 4 GiB", {0, 1, 32, 1}},
    {"qword index addition wraps", {0xfffffff8u, 3, 32, 0xffffffffu}},
    {"qword store aligned to a DWORD only", {0, 1, 36, 0}},
    {"qword store above the window", {0, 1, 0xfffffff8u, 0}},
    {"qword store beyond descriptor", {0, 1, 64, 0}},
    {"qword store in the last descriptor QWORD", {0, 1, 56, 0}},
};

constexpr QwordCase narrow_cases[] = {
    {"narrow load below the end", {0, 4, 16, 0}},
    {"narrow load at the end", {0, 5, 16, 0}},
    {"narrow load past the end", {0, 6, 16, 0}},
    {"narrow store below the end", {0, 1, 32, 0}},
    {"narrow store at the end", {0, 1, 40, 0}},
    {"narrow store past the end", {0, 1, 48, 0}},
};

void init_dword_arena(std::array<UINT, 16>& words)
{
    words.fill(arena_fill);
    words[3] = left;
    words[7] = right;
}

void apply_dword_case(const WindowCase& test, std::array<UINT, 16>& expected)
{
    UINT result = test.expected;
    if (test.atomic_word >= 0) {
        auto& word = expected[static_cast<size_t>(test.atomic_word)];
        result = word;
        word += test.parameters[1];
    }
    const UINT destination = test.parameters[2];
    if (destination % 4 == 0 && destination < sizeof(expected)) expected[destination / 4] = result;
}

// Negative words, so that a signed and an unsigned compare choose differently against a small positive operand.
void init_signed_arena(std::array<UINT, 16>& words)
{
    words.fill(arena_fill);
    words[2] = static_cast<UINT>(-3);
    words[3] = static_cast<UINT>(-7);
    words[4] = 0x80000000u;
    words[5] = static_cast<UINT>(-5);
}

// Independent CPU model of the signed accesses. An address is used only when it is a multiple of four, below the
// window end, and the base plus the offset does not wrap. The 64-byte view then returns zero for a read past it and
// discards a write past it. Every old value is stored, as zero when its address was not used. The fifth address runs
// the sequence, each step reading what the previous one left.
void apply_signed_case(const SignedCase& test, std::array<UINT, 16>& words)
{
    const uint64_t base = (static_cast<uint64_t>(test.parameters[3]) << 32) | test.parameters[0];
    const UINT bits = test.parameters[1];
    const int32_t operand = static_cast<int32_t>(bits);
    const UINT expected = ((bits & 0xffff0000u) | 0x0f0fu) ^ 0x0ff00ff0u;
    UINT old[10]{};
    // The DWORD at base + 4 * index, or null when the address is not used.
    auto target = [&](uint64_t index) -> UINT* {
        const uint64_t address = base + 4 * index;
        if (address < base || address % 4 != 0 || address >= 0xfffffffcull || address + 4 > sizeof(words)) return nullptr;
        return &words[address / 4];
    };
    for (uint64_t k = 0; k < 3; ++k) {
        UINT* word = target(k + 1);
        if (!word) continue;
        old[k] = *word;
        const int32_t value = static_cast<int32_t>(*word);
        if (k == 0) *word = static_cast<UINT>(operand < value ? operand : value);
        if (k == 1) *word = static_cast<UINT>(operand > value ? operand : value);
    }
    if (UINT* word = target(4)) {
        // Each step stores its old value and leaves the next step's input.
        old[3] = *word;
        *word += bits;
        old[4] = *word;
        *word = bits;
        old[5] = *word;
        *word &= 0xffff0000u;
        old[6] = *word;
        *word |= 0x0f0fu;
        old[7] = *word;
        *word ^= 0x0ff00ff0u;
        old[8] = *word;
        if (*word == (expected ^ 0x80000000u)) *word = 0x12345678u;
        old[9] = *word;
        if (*word == expected) *word = 0xfffe1dc0u;
    }
    for (uint64_t k = 0; k < 10; ++k) {
        const uint64_t address = static_cast<uint64_t>(test.parameters[2]) + 4 * k;
        if (address % 4 == 0 && address < 0xfffffffcull && address + 4 <= sizeof(words)) words[address / 4] = old[k];
    }
}

// A quiet NaN with a payload, a negative zero and 1.5 where the shader loads, over words that would be wrong if read.
void init_float_arena(std::array<UINT, 16>& words)
{
    words.fill(arena_fill);
    words[3] = 0x7fc01234u;
    words[4] = 0x80000000u;
    words[5] = 0x3fc00000u;
}

// Independent CPU model of the float accesses. An address is used only when it is a multiple of four, below the
// window end, and the base plus the offset does not wrap. The 64-byte view then returns zero for a read past it and
// discards a write past it. All loads happen before any store, and a load that was not used yields the bit pattern 0.
void apply_float_case(const FloatCase& test, std::array<UINT, 16>& words)
{
    const uint64_t base = (static_cast<uint64_t>(test.parameters[3]) << 32) | test.parameters[0];
    const std::array<UINT, 16> source = words;
    UINT value[4]{0, 0, 0, test.parameters[1]};
    for (uint64_t k = 0; k < 3; ++k) {
        const uint64_t address = base + 4 * (k + 1);
        if (address < base || address % 4 != 0 || address >= 0xfffffffcull || address + 4 > sizeof(source)) continue;
        value[k] = source[address / 4];
    }
    for (uint64_t k = 0; k < 4; ++k) {
        const uint64_t address = static_cast<uint64_t>(test.parameters[2]) + 4 * k;
        if (address % 4 == 0 && address < 0xfffffffcull && address + 4 <= sizeof(words)) words[address / 4] = value[k];
    }
}

// Four vectors of four distinct DWORDs. The second holds a quiet NaN with a payload, a negative zero, 1.5 and a negative
// NaN, the third an ordinary number, -1.0, +infinity and the largest finite float, so a swapped lane is visible. The
// first and fourth hold values that would be wrong to read.
void init_vec4_arena(std::array<UINT, 16>& words)
{
    words = {0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u, 0x7fc01234u, 0x80000000u, 0x3fc00000u, 0xffc0abcdu,
             0x3f99999au, 0xbf800000u, 0x7f800000u, 0x7f7fffffu, arena_fill, arena_fill, arena_fill, arena_fill};
}

// Independent CPU model of the four-float accesses. A vector is used only when its address is a multiple of 16 and
// all 16 bytes are below the window end, and for an indexed address the sum with the base does not wrap. The 64-byte
// view then returns zero for a read past it and discards a write past it; an aligned vector never crosses the view.
// All loads happen before any store, and a vector that was not used loads as four zero DWORDs.
void apply_vec4_case(const Vec4Case& test, std::array<UINT, 16>& words, uint64_t window_end)
{
    const uint64_t base = (static_cast<uint64_t>(test.parameters[3]) << 32) | test.parameters[0];
    const std::array<UINT, 16> source = words;
    const auto usable = [window_end](uint64_t address, uint64_t anchor) {
        return address >= anchor && address % 16 == 0 && address < window_end && window_end - address >= 16;
    };
    std::array<UINT, 4> value[3]{};
    const uint64_t load_address[2] = {base, base + 16};
    for (int k = 0; k < 2; ++k) {
        if (usable(load_address[k], base) && load_address[k] + 16 <= sizeof(source))
            std::memcpy(value[k].data(), reinterpret_cast<const BYTE*>(source.data()) + load_address[k], 16);
    }
    value[2] = {test.parameters[1], value[1][2], value[0][1], value[0][0]};
    const uint64_t destination = test.parameters[2];
    for (uint64_t k = 0; k < 3; ++k) {
        const uint64_t address = destination + 16 * k;
        if (usable(address, destination) && address + 16 <= sizeof(words))
            std::memcpy(reinterpret_cast<BYTE*>(words.data()) + address, value[k].data(), 16);
    }
}

// Two QWORDs whose halves differ, the first carrying from the low into the high half when one is added.
void init_qword_arena(std::array<UINT, 16>& words)
{
    words.fill(arena_fill);
    words[2] = 0xffffffffu;
    words[3] = 0x00000001u;
    words[6] = 0x89abcdefu;
    words[7] = 0x01234567u;
}

// Independent CPU model of the bounded 64-bit access. A QWORD is accessed only when its address is a multiple of
// eight, below the window end, and the base plus the scaled index does not wrap. The 64-byte view then returns zero
// for a read past it and discards a write past it; a QWORD never straddles the end of the view.
void apply_qword_case(const QwordCase& test, std::array<UINT, 16>& expected, uint64_t window_end)
{
    const uint64_t base = (static_cast<uint64_t>(test.parameters[3]) << 32) | test.parameters[0];
    const uint64_t source = base + static_cast<uint64_t>(test.parameters[1]) * 8;
    const bool source_ok = source >= base && source % 8 == 0 && source < window_end;
    uint64_t value = 0;
    if (source_ok && source + 8 <= sizeof(expected)) std::memcpy(&value, reinterpret_cast<BYTE*>(expected.data()) + source, 8);
    value += 1;
    const uint64_t destination = test.parameters[2];
    const bool destination_ok = destination % 8 == 0 && destination < window_end;
    if (destination_ok && destination + 8 <= sizeof(expected))
        std::memcpy(reinterpret_cast<BYTE*>(expected.data()) + destination, &value, 8);
}

template <typename Case, size_t Count, typename Init, typename Apply>
void check_window_cases(ID3D12Device* device, const char* shader_path, const Case (&cases)[Count], Init init, Apply apply)
{
    std::ifstream shader(shader_path, std::ios::binary);
    if (!shader) throw std::runtime_error("Cannot open address-window shader");
    const std::vector<char> bytes(std::istreambuf_iterator<char>(shader), {});
    D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2, 0}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    for (UINT i = 0; i < 2; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1, &ranges[i]};
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters};
    ComPtr<ID3DBlob> root_bytes, error;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_bytes, &error),
          "Serialize address-window root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_bytes->GetBufferPointer(), root_bytes->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "Create address-window root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = {bytes.data(), bytes.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "Create address-window pipeline");

    for (const auto& test : cases) {
        std::array<UINT, 16> expected;
        init(expected);
        auto upload = make_buffer(device, 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto arena = make_buffer(device, sizeof(expected), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = make_buffer(device, sizeof(expected), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RANGE empty{};
        void* mapped = nullptr;
        check(upload->Map(0, &empty, &mapped), "Map address-window upload");
        std::memcpy(mapped, test.parameters.data(), sizeof(test.parameters));
        std::memcpy(static_cast<BYTE*>(mapped) + 64, expected.data(), sizeof(expected));
        upload->Unmap(0, nullptr);
        apply(test, expected);

        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        ComPtr<ID3D12DescriptorHeap> heap;
        check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "Create address-window descriptors");
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        UINT stride = device->GetDescriptorHandleIncrementSize(desc.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(test.parameters.size());
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(upload.Get(), &srv, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(expected.size());
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(arena.Get(), nullptr, &uav, cpu);

        ComputeCommands commands(device, pipeline.Get());
        auto& list = commands.list;
        transition(list.Get(), arena.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(arena.Get(), 0, upload.Get(), 64, sizeof(expected));
        transition(list.Get(), arena.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += stride;
        list->SetComputeRootDescriptorTable(1, gpu);
        list->Dispatch(1, 1, 1);
        transition(list.Get(), arena.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, arena.Get(), 0, sizeof(expected));
        commands.submit_and_wait(device);
        D3D12_RANGE read{0, sizeof(expected)};
        check(readback->Map(0, &read, &mapped), "Map address-window readback");
        std::array<UINT, 16> actual;
        std::memcpy(actual.data(), mapped, sizeof(actual));
        readback->Unmap(0, &empty);
        for (size_t i = 0; i < expected.size(); ++i) {
            if (actual[i] != expected[i]) {
                std::fprintf(stderr, "%s word %zu expected %08x observed %08x\n", test.name, i, expected[i], actual[i]);
                throw std::runtime_error("Address-window readback mismatch");
            }
        }
        std::printf("%s: all %zu words matched\n", test.name, expected.size());
    }
}
}

void check_spirv_window(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, cases, init_dword_arena, apply_dword_case);
}

void check_spirv_window_atomic(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, atomic_cases, init_dword_arena, apply_dword_case);
}

void check_spirv_window_signed(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, signed_cases, init_signed_arena, apply_signed_case);
}

void check_spirv_window_float(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, float_cases, init_float_arena, apply_float_case);
}

void check_spirv_window_vec4(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, vec4_cases, init_vec4_arena,
                       [](const Vec4Case& test, std::array<UINT, 16>& words) {
                           apply_vec4_case(test, words, vec4_window_end);
                       });
}

void check_spirv_window_vec4_narrow(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, vec4_narrow_cases, init_vec4_arena,
                       [](const Vec4Case& test, std::array<UINT, 16>& words) {
                           apply_vec4_case(test, words, vec4_narrow_window_end);
                       });
}

void check_spirv_window_qword(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, qword_cases, init_qword_arena,
                       [](const QwordCase& test, std::array<UINT, 16>& words) {
                           apply_qword_case(test, words, qword_window_end);
                       });
}

void check_spirv_window_qword_narrow(ID3D12Device* device, const char* shader_path)
{
    check_window_cases(device, shader_path, narrow_cases, init_qword_arena,
                       [](const QwordCase& test, std::array<UINT, 16>& words) {
                           apply_qword_case(test, words, narrow_window_end);
                       });
}
