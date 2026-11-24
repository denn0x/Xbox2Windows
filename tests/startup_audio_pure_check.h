// Platform-independent checks for src/startup_stubs/audio_sink.h; they also build with g++ on Linux.
#pragma once
#include "startup_stubs/audio_sink.h"

#include <cstdio>

namespace x2w_audio {

// Generated mono PCM16 sine block; amplitude 0 gives an all-zero (silent) block.
inline std::vector<int16_t> make_tone(uint32_t rate, uint32_t frames, double hz, double amplitude)
{
    std::vector<int16_t> pcm(frames);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i] = static_cast<int16_t>(std::lround(amplitude * 32767.0 * std::sin(2.0 * 3.14159265358979 * hz * i / rate)));
    return pcm;
}

inline int check_pure()
{
    const BlockFormat mono16{SampleKind::Pcm16, 1, 48000};
    const auto tone = make_tone(48000, 480, 440.0, 0.5);
    const auto silent = make_tone(48000, 480, 440.0, 0.0);
    if (block_peak(mono16, silent.data(), 480) != 0.0f || block_peak(mono16, tone.data(), 480) < 0.45f) return 10;

    std::vector<uint8_t> out;
    const BlockFormat stereo_f{SampleKind::Float32, 2, 48000};
    if (!convert_block(mono16, tone.data(), 480, stereo_f, out) || out.size() != 480 * 8) return 11;
    if (std::fabs(block_peak(stereo_f, out.data(), 480) - block_peak(mono16, tone.data(), 480)) > 1e-6f) return 12;
    if (!convert_block(mono16, silent.data(), 480, stereo_f, out) || block_peak(stereo_f, out.data(), 480) != 0.0f) return 13;
    const BlockFormat stereo16{SampleKind::Pcm16, 2, 48000};
    if (!convert_block(mono16, tone.data(), 480, stereo16, out) || out.size() != 480 * 4) return 14;
    std::vector<uint8_t> back;  // PCM16 -> float -> PCM16 must be lossless
    const BlockFormat mono_f{SampleKind::Float32, 1, 48000};
    if (!convert_block(mono16, tone.data(), 480, mono_f, out) || !convert_block(mono_f, out.data(), 480, mono16, back) ||
        std::memcmp(back.data(), tone.data(), 960) != 0) return 15;
    if (!convert_block(mono16, tone.data(), 480, {SampleKind::Pcm16, 1, 44100}, out) || out.size() != 441 * 2) return 16;
    if (convert_block(stereo16, out.data(), 120, {SampleKind::Pcm16, 6, 48000}, back)) return 17;   // unsupported upmix

    // Keep the existing equal-channel and mono copy paths on quad endpoints.
    const BlockFormat quad_f{SampleKind::Float32, 4, 48000};
    const float quad[4] = {0.125f, -0.25f, 0.375f, -0.5f};
    if (!convert_block(quad_f, quad, 1, quad_f, out) || out.size() != sizeof(quad) ||
        std::memcmp(out.data(), quad, sizeof(quad)) != 0) return 24;
    if (!convert_block(mono_f, quad, 1, quad_f, out) || out.size() != sizeof(quad)) return 25;
    for (size_t channel = 0; channel < 4; ++channel)
        if (read_sample(quad_f, out.data(), channel) != quad[0]) return 26;

    // Stereo -> mono fold-down.
    {
        const float st_data[4] = {0.8f, -0.2f, 0.4f, 0.4f};
        const BlockFormat st_f{SampleKind::Float32, 2, 48000};
        if (!convert_block(st_f, st_data, 2, mono_f, out) || out.size() != 8) return 30;
        if (std::fabs(read_sample(mono_f, out.data(), 0) - 0.3f) > 1e-6f) return 31;
        if (std::fabs(read_sample(mono_f, out.data(), 1) - 0.4f) > 1e-6f) return 32;
    }

    // 5.1 (6ch) -> stereo ITU-R BS.775-1 downmix accuracy, LFE omission, and clamping.
    {
        const BlockFormat ch6_f{SampleKind::Float32, 6, 48000};
        // Frame 0: FL=0.5, FR=0.25, FC=0.4, LFE=0.9 (ignored), BL=0.2, BR=-0.2
        // Frame 1: FL=1.0, FR=-1.0, FC=1.0, LFE=0.0, BL=1.0, BR=-1.0 -> clamped to [+1.0, -1.0]
        const float src6[12] = {
            0.5f, 0.25f, 0.4f, 0.9f, 0.2f, -0.2f,
            1.0f, -1.0f, 1.0f, 0.0f, 1.0f, -1.0f,
        };
        if (!convert_block(ch6_f, src6, 2, stereo_f, out) || out.size() != 16) return 33;
        const float exp_l0 = 0.5f + kItuSurroundCoeff * 0.4f + kItuSurroundCoeff * 0.2f;
        const float exp_r0 = 0.25f + kItuSurroundCoeff * 0.4f + kItuSurroundCoeff * (-0.2f);
        if (std::fabs(read_sample(stereo_f, out.data(), 0) - exp_l0) > 1e-5f) return 34;
        if (std::fabs(read_sample(stereo_f, out.data(), 1) - exp_r0) > 1e-5f) return 35;
        if (read_sample(stereo_f, out.data(), 2) != 1.0f) return 36;
        if (read_sample(stereo_f, out.data(), 3) != -1.0f) return 37;
    }

    // 7.1 (8ch) -> stereo ITU/SMPTE downmix accuracy & side/back weighting.
    {
        const BlockFormat ch8_f{SampleKind::Float32, 8, 48000};
        // FL=0.2, FR=0.1, FC=0.3, LFE=0.8, BL=0.1, BR=0.2, SL=0.4, SR=-0.1
        const float src8[8] = {0.2f, 0.1f, 0.3f, 0.8f, 0.1f, 0.2f, 0.4f, -0.1f};
        if (!convert_block(ch8_f, src8, 1, stereo_f, out) || out.size() != 8) return 38;
        const float exp_l = 0.2f + kItuSurroundCoeff * 0.3f + kItuSurroundCoeff * (0.1f + 0.4f);
        const float exp_r = 0.1f + kItuSurroundCoeff * 0.3f + kItuSurroundCoeff * (0.2f - 0.1f);
        if (std::fabs(read_sample(stereo_f, out.data(), 0) - exp_l) > 1e-5f) return 39;
        if (std::fabs(read_sample(stereo_f, out.data(), 1) - exp_r) > 1e-5f) return 40;
    }

    // 24 kHz -> 48 kHz and 44.1 kHz -> 48 kHz linear resampling frame counts & signal preservation.
    {
        const BlockFormat mono24{SampleKind::Pcm16, 1, 24000};
        const auto tone24 = make_tone(24000, 240, 440.0, 0.5);
        if (!convert_block(mono24, tone24.data(), 240, mono_f, out) || out.size() != 480 * 4) return 41;
        if (block_peak(mono_f, out.data(), 480) < 0.45f) return 42;

        // Constant DC preservation across 44.1 kHz -> 48 kHz.
        const BlockFormat mono44_f{SampleKind::Float32, 1, 44100};
        const std::vector<float> dc44(441, 0.625f);
        if (!convert_block(mono44_f, dc44.data(), 441, mono_f, out) || out.size() != 480 * 4) return 43;
        for (uint32_t i = 0; i < 480; ++i)
            if (std::fabs(read_sample(mono_f, out.data(), i) - 0.625f) > 1e-6f) return 44;

        // 44.1 kHz -> 48 kHz sine tone peak preservation (40 ms: 1764 -> 1920 frames).
        const BlockFormat mono44{SampleKind::Pcm16, 1, 44100};
        const auto tone44 = make_tone(44100, 1764, 440.0, 0.5);
        if (!convert_block(mono44, tone44.data(), 1764, mono_f, out) || out.size() != 1920 * 4) return 45;
        const float pk = block_peak(mono_f, out.data(), 1920);
        if (pk < 0.48f || pk > 0.51f) return 46;
    }

    // Combined 5.1 44.1 kHz -> stereo 48 kHz conversion.
    {
        const BlockFormat ch6_44{SampleKind::Float32, 6, 44100};
        std::vector<float> src6_44(441 * 6, 0.0f);
        for (uint32_t i = 0; i < 441; ++i) {
            src6_44[i * 6 + 0] = 0.25f;
            src6_44[i * 6 + 2] = 0.25f;
        }
        if (!convert_block(ch6_44, src6_44.data(), 441, stereo_f, out) || out.size() != 480 * 8) return 47;
        const float exp_l = 0.25f + kItuSurroundCoeff * 0.25f;
        const float exp_r = kItuSurroundCoeff * 0.25f;
        if (std::fabs(read_sample(stereo_f, out.data(), 0) - exp_l) > 1e-5f) return 48;
        if (std::fabs(read_sample(stereo_f, out.data(), 1) - exp_r) > 1e-5f) return 49;
    }

    // Mono fold-down clamps only the final value: FL=1, FR=-1, FC=1 -> sqrt(0.5), not 0.3536.
    {
        const BlockFormat ch6_f{SampleKind::Float32, 6, 48000};
        const float fold[6] = {1.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        if (!convert_block(ch6_f, fold, 1, mono_f, out) || out.size() != 4) return 60;
        if (std::fabs(read_sample(mono_f, out.data(), 0) - 0.5f * kItuSurroundCoeff * 2.0f) > 1e-5f) return 61;
        // Stereo targets still clamp each channel (FL = 1 + 0.7071 clips to 1).
        if (!convert_block(ch6_f, fold, 1, stereo_f, out) || out.size() != 8) return 62;
        if (read_sample(stereo_f, out.data(), 0) != 1.0f) return 63;
    }

    // Streaming resampler: output must not depend on how the input is split into blocks.
    {
        const BlockFormat in44{SampleKind::Float32, 1, 44100};
        std::vector<float> ramp(44100);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 50000.0f;
        std::vector<uint8_t> whole, piece;
        ResampleState one, many;
        if (!convert_block(in44, ramp.data(), 44100, mono_f, whole, &one)) return 64;
        std::vector<uint8_t> joined;
        for (size_t at = 0; at < ramp.size(); at += 128) {
            const uint32_t n = static_cast<uint32_t>((std::min)(size_t{128}, ramp.size() - at));
            if (!convert_block(in44, ramp.data() + at, n, mono_f, piece, &many)) return 65;
            joined.insert(joined.end(), piece.begin(), piece.end());
        }
        if (joined.size() != whole.size()) return 66;
        for (size_t i = 0; i < whole.size() / 4; ++i)
            if (std::fabs(read_sample(mono_f, joined.data(), i) - read_sample(mono_f, whole.data(), i)) > 1e-5f) return 67;
        if (joined.size() / 4 < 48000 - 2 || joined.size() / 4 > 48000) return 68;  // no per-block rounding drift

        // 24 -> 48 kHz ramp split at a boundary keeps the interpolation sample between the blocks.
        const BlockFormat in24{SampleKind::Float32, 1, 24000};
        const float r8[8] = {0, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
        ResampleState st;
        std::vector<uint8_t> a, b;
        if (!convert_block(in24, r8, 4, mono_f, a, &st) || !convert_block(in24, r8 + 4, 4, mono_f, b, &st)) return 69;
        a.insert(a.end(), b.begin(), b.end());
        if (a.size() != 14 * 4) return 70;
        for (size_t i = 0; i < 14; ++i)
            if (std::fabs(read_sample(mono_f, a.data(), i) - 0.05f * static_cast<float>(i)) > 1e-5f) return 71;

        // A format change drops the carried state: the new stream starts at its own first frame.
        const BlockFormat in16{SampleKind::Float32, 1, 16000};
        const float flat[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        if (!convert_block(in16, flat, 4, mono_f, a, &st)) return 72;
        if (a.empty() || std::fabs(read_sample(mono_f, a.data(), 0) - 0.5f) > 1e-6f) return 73;
    }

    // Negative controls: 0 rate, out-of-range rate, 0 channels, unsupported channel count, empty block, null ptr.
    if (convert_block({SampleKind::Pcm16, 1, 0}, tone.data(), 480, mono_f, out)) return 50;
    if (convert_block(mono16, tone.data(), 480, {SampleKind::Float32, 1, 0}, out)) return 51;
    if (convert_block({SampleKind::Pcm16, 1, 999}, tone.data(), 480, mono_f, out)) return 52;
    if (convert_block({SampleKind::Pcm16, 1, 500000}, tone.data(), 480, mono_f, out)) return 53;
    if (convert_block({SampleKind::Pcm16, 0, 48000}, tone.data(), 480, mono_f, out)) return 54;
    if (convert_block(mono16, tone.data(), 480, {SampleKind::Float32, 0, 48000}, out)) return 55;
    if (convert_block({SampleKind::Pcm16, 3, 48000}, tone.data(), 120, stereo_f, out)) return 56;
    if (convert_block({SampleKind::Pcm16, 9, 48000}, tone.data(), 40, stereo_f, out)) return 57;
    if (convert_block(mono16, tone.data(), 0, mono_f, out)) return 58;
    if (convert_block(mono16, nullptr, 480, mono_f, out)) return 59;

    StarvationTimer t;  // queue empties at 100 ms, producer resumes at 400 ms, data moves at 430 ms
    t.sample_queue(50, 10, true);
    if (t.drained_start >= 0) return 20;                    // padding > 0: not drained
    t.sample_queue(100, 0, false);
    if (t.drained_start >= 0) return 21;                    // producer not withholding: not starvation
    t.sample_queue(100, 0, true);
    t.sample_queue(250, 0, true);
    t.resume(400, 960);
    t.sample_consumed(410, 960);
    t.sample_consumed(430, 1000);
    t.sample_consumed(500, 2000);
    if (t.drained_ms != 300 || t.recovery_ms != 30) return 22;
    std::puts("pure checks: conversion, silence detection and drained/recovery arithmetic ok");
    return 0;
}

}  // namespace x2w_audio
