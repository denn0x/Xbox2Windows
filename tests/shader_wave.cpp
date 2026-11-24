#include "bvh_fixture.h"
#include "ShaderWave.h"
#include "ShaderWave32.h"
#include "ShaderWave64.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>

using namespace bvh_fixture;

namespace {
constexpr Word threads = 64;
constexpr Word fields = 16;
constexpr Word guard_low = 0x5a5a5a5au;
constexpr Word guard_high = 0xa5a5a5a5u;

struct WaveRun {
    Word width = 0;
    std::vector<Word> words;
};

void print_windows_build()
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get_version = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    if (get_version && get_version(&info) == 0) {
        std::printf("windows_build=%lu.%lu.%lu\n", info.dwMajorVersion, info.dwMinorVersion,
                    info.dwBuildNumber);
    } else {
        std::puts("windows_build=unavailable");
    }
}

// Older runtimes reject shader-model enums above what they recognize with E_INVALIDARG.
D3D_SHADER_MODEL highest_shader_model(ID3D12Device* device)
{
    for (D3D_SHADER_MODEL requested : {
             D3D_SHADER_MODEL_6_6,
             D3D_SHADER_MODEL_6_5,
             D3D_SHADER_MODEL_6_4,
             D3D_SHADER_MODEL_6_3,
             D3D_SHADER_MODEL_6_2,
             D3D_SHADER_MODEL_6_1,
             D3D_SHADER_MODEL_6_0,
             D3D_SHADER_MODEL_5_1,
         }) {
        D3D12_FEATURE_DATA_SHADER_MODEL model{requested};
        const HRESULT hr = check_feature(device, D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
        if (hr == E_INVALIDARG) continue;
        check(hr, "Query wave shader model");
        return model.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_5_1;
}

WaveRun run_wave(ID3D12Device* device, D3D12_SHADER_BYTECODE code)
{
    constexpr UINT bytes = threads * fields * sizeof(Word);
    Kernel shader(device, code);
    auto out = make_buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto back = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ComputeCommands cmd(device, shader.pipeline.Get());
    auto* list = cmd.list.Get();
    transition(list, out.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(shader.root.Get());
    list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list, out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, out.Get(), 0, bytes);
    cmd.submit_and_wait(device);
    WaveRun result;
    result.words = read(back.Get(), threads * fields);
    result.width = result.words[1];
    return result;
}

// The CPU model uses only the width and lanes the shader reported. It never assumes wave32 or
// wave64, and it accepts a final partially populated wave when the width exceeds the 64 threads.
void check_records(const WaveRun& run, const char* label, Word forced)
{
    const Word width = run.width;
    if (!width || width > 128) throw std::runtime_error("Unexpected wave width");
    if (forced && width != forced) throw std::runtime_error("Forced wave width was not applied");
    std::map<Word, std::vector<Word>> waves;
    for (Word t = 0; t < threads; ++t) {
        const Word* r = run.words.data() + t * fields;
        if (r[1] != width) throw std::runtime_error("Wave width varies");
        if (r[0] >= width) throw std::runtime_error("Lane index is outside the wave");
        waves[r[2]].push_back(t);
    }
    if (waves.size() != (threads + width - 1) / width) throw std::runtime_error("Unexpected wave count");
    Word first_active = 0, first_sum = 0;
    std::array<Word, 4> first_ballot{};
    bool have_first = false;
    for (const auto& [first, wave_threads] : waves) {
        if (std::count(wave_threads.begin(), wave_threads.end(), first) != 1)
            throw std::runtime_error("Wave identity does not name one of its threads");
        const Word active = static_cast<Word>(wave_threads.size());
        if (!active || active > width) throw std::runtime_error("Invalid wave population");
        std::vector<Word> lanes;
        lanes.reserve(active);
        for (Word t : wave_threads) lanes.push_back(run.words[t * fields]);
        std::sort(lanes.begin(), lanes.end());
        std::array<Word, 4> ballot{};
        Word sum_lanes = 0;
        for (Word lane : lanes) {
            if (lane >= width) throw std::runtime_error("Wave lane exceeds the reported width");
            ballot[lane / 32] |= 1u << (lane % 32);
            sum_lanes += lane;
        }
        for (Word i = 1; i < active; ++i) {
            if (lanes[i] == lanes[i - 1]) throw std::runtime_error("Duplicate wave lane");
        }
        if (active == width) {
            for (Word lane = 0; lane < width; ++lane) {
                if (lanes[lane] != lane) throw std::runtime_error("Full wave is missing a lane");
            }
        }
        for (Word t : wave_threads) {
            const Word* r = run.words.data() + t * fields;
            equal(r[3], active, t, 3);
            for (Word word = 0; word < 4; ++word) equal(r[4 + word], ballot[word], t, 4 + word);
            equal(r[8], 1, t, 8);
            equal(r[9], lanes.front(), t, 9);
            equal(r[10], lanes.back(), t, 10);
            if (active == width) equal(r[11], 0, t, 11);
            else if (!std::binary_search(lanes.begin(), lanes.end(), r[11]))
                throw std::runtime_error("WaveReadLaneFirst returned a lane outside the wave");
            equal(r[12], active, t, 12);
            equal(r[13], sum_lanes, t, 13);
            equal(r[14], guard_low, t, 14);
            equal(r[15], guard_high, t, 15);
        }
        if (!have_first) {
            first_active = active;
            first_ballot = ballot;
            first_sum = sum_lanes;
            have_first = true;
        }
    }
    std::printf("shader_wave %s width=%u waves=%zu sum_ones=%u ballot=%08x/%08x/%08x/%08x "
                "sum_lanes=%u mismatches=0\n",
                label, width, waves.size(), first_active, first_ballot[0], first_ballot[1],
                first_ballot[2], first_ballot[3], first_sum);
}
}

bool check_shader_wave(ID3D12Device* device, Word forced)
{
    if (forced != 0 && forced != 32 && forced != 64)
        throw std::runtime_error("Unsupported forced wave width");
    const D3D_SHADER_MODEL needed = forced ? D3D_SHADER_MODEL_6_6 : D3D_SHADER_MODEL_6_0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 caps{};
    check(check_feature(device, D3D12_FEATURE_D3D12_OPTIONS1, &caps, sizeof(caps)), "Query wave range");
    const D3D_SHADER_MODEL model = highest_shader_model(device);
    std::printf("native_lane_range=%u..%u wave_ops=%u shader_model_capped_at_6_6=0x%x\n",
                caps.WaveLaneCountMin, caps.WaveLaneCountMax, caps.WaveOps ? 1u : 0u,
                static_cast<unsigned>((std::min)(model, D3D_SHADER_MODEL_6_6)));
    print_windows_build();
    if (!caps.WaveOps || model < needed) return false;
    if (forced && (caps.WaveLaneCountMin > forced || caps.WaveLaneCountMax < forced)) return false;
    check_records(run_wave(device, {ShaderWave, sizeof(ShaderWave)}), "native", 0);
    if (!forced) return true;
    const D3D12_SHADER_BYTECODE forced_code = forced == 32
        ? D3D12_SHADER_BYTECODE{ShaderWave32, sizeof(ShaderWave32)}
        : D3D12_SHADER_BYTECODE{ShaderWave64, sizeof(ShaderWave64)};
    check_records(run_wave(device, forced_code), forced == 32 ? "forced32" : "forced64", forced);
    return true;
}
