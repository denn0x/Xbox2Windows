// Playback-timing control: the same endpoint and the same sink as #1250, fed two ways.
//
// The stutter reported from the title is a pacing symptom, and an audible report cannot say whether
// the producer fed the endpoint in steady blocks or in bursts with gaps. This fixture feeds the sink
// the same number of blocks, and therefore the same total audio, twice:
//
//   steady  - one block every block_period, paced in real time
//   bursty  - a run of blocks back to back, then a withholding gap with no submission
//
// and prints, for each phase, the first submission, the block and frame counts, the endpoint clock,
// the drained interval (how long the queue sat empty while the producer withheld data) and the
// recovery time (from the producer resuming to consumption advancing again). The sink and the endpoint
// are not modified: this measures them.
//
// The sink's report is cumulative for the life of the sink, so every number below is a phase delta
// taken against a snapshot before the phase started; a raw report() would otherwise include the
// earlier phase and mislabel it as this one's.
//
// The check is the difference between the phases, not an absolute number: bursty input must show a
// drained interval the steady phase does not, otherwise the measurement cannot see a producer-side
// stutter at all and this fixture is not evidence either way.
#include "startup_stubs/audio_sink.h"

#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace x2w_audio;

namespace {
constexpr int skip_no_endpoint = 77;

// What one phase did, with every sink counter expressed as a delta over the phase.
struct Phase {
    const char* name = "";
    double first_submit_ms = -1;
    double largest_gap_ms = 0;
    unsigned long long blocks = 0;
    unsigned long long frames = 0;
    unsigned long long consumed = 0;
    unsigned long long clock = 0;
    double drained_ms = 0;
    double pacing_error_max_ms = 0;  // worst |actual submit time - ideal time| in the phase
    double pacing_error_rms_ms = 0;  // root-mean-square of the same deviation
    double pacing_error_p95_ms = 0;
    double recovery_ms = -1;
    unsigned rejected_full = 0;
    unsigned unsupported = 0;
};

void print_phase(const Phase& p)
{
    std::printf("%s: first_submission_at=%.1fms blocks=%llu frames=%llu consumed=%llu clock=%llu "
                "largest_submit_gap=%.1fms drained_ms=%.1f recovery_ms=%.1f pacing_error_max=%.1fms pacing_error_rms=%.1fms rejected_full=%u unsupported=%u\n",
                p.name, p.first_submit_ms, p.blocks, p.frames, p.consumed, p.clock, p.largest_gap_ms, p.drained_ms,
                p.recovery_ms, p.pacing_error_max_ms, p.pacing_error_rms_ms, p.rejected_full, p.unsupported);
}

// Nearest-rank percentile. The maximum is still reported for isolated scheduler stalls.
double pacing_percentile95(std::vector<double> errors)
{
    if (errors.empty()) return 0;
    const auto index = (errors.size() * 95 + 99) / 100 - 1;
    std::nth_element(errors.begin(), errors.begin() + index, errors.end());
    return errors[index];
}

// Frames of a quiet tone so blocks are valid audio rather than an empty submission.
std::vector<int16_t> tone(uint32_t rate, uint32_t frames, double hz)
{
    std::vector<int16_t> pcm(frames);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i] = static_cast<int16_t>(1200.0 * std::sin(2.0 * 3.14159265358979 * hz * i / rate));
    return pcm;
}

// A feed scheduled with Sleep(1) cannot be tighter than a few timer ticks, and the tick differs between
// hosts (#1448 measured the same Sleep overshoot on this one). The steady bound is derived from the
// measured tick rather than a fixed number, so the check says "the feed kept up with the host's timer"
// instead of encoding whatever this machine happened to do on the day.
double sleep_granularity_ms()
{
    constexpr int samples = 9;
    double ticks[samples];
    for (double& tick : ticks) {
        const double start = now_ms();
        Sleep(1);
        tick = now_ms() - start;
    }
    std::sort(ticks, ticks + samples);
    return ticks[samples / 2];
}

// The tick alone is not the bound: the steady phase takes the worst of 120 wake-ups, and a host with a
// raised timer resolution has a ~1 ms tick, so a three-tick bound would be a few milliseconds and one
// deschedule would fail the run. Calibrate the scheduling the fixture actually uses by running the same
// wait loop, the same number of times, with nothing submitted.
double calibrate_scheduler_ms(uint32_t block_frames, uint32_t rate, int blocks)
{
    double worst = 0;
    const double begin = now_ms();
    for (int i = 0; i < blocks; ++i) {
        const double want = begin + i * (1000.0 * block_frames / rate);
        while (now_ms() < want) Sleep(1);
        worst = (std::max)(worst, std::fabs(now_ms() - want));
    }
    return worst;
}

Submit submit_with_wait(AudioSink& sink, const BlockFormat& format, const std::vector<int16_t>& pcm)
{
    const double end = now_ms() + 500;
    for (;;) {
        const Submit r = sink.submit(format, static_cast<uint32_t>(pcm.size()), pcm.data());
        if (r != Submit::Full || now_ms() > end) return r;
        Sleep(2);
        sink.observe(false);
    }
}

Phase delta(const Report& before, const Report& after, const char* name)
{
    Phase p;
    p.name = name;
    p.blocks = after.frames.size() - before.frames.size();
    p.frames = after.submitted_frames - before.submitted_frames;
    p.consumed = after.consumed_frames - before.consumed_frames;
    p.clock = after.clock_frames - before.clock_frames;
    p.drained_ms = after.drained_ms - before.drained_ms;
    p.recovery_ms = after.recovery_ms;
    p.rejected_full = after.rejected_full - before.rejected_full;
    p.unsupported = after.unsupported - before.unsupported;
    return p;
}

// Records when a submission happened, the largest gap between consecutive submissions, and -- the
// metric a stutter report actually needs -- how far each submission drifted from the ideal cadence.
// A feed that is late but evenly spaced shows gaps without drift; a stuttering feed shows drift.
struct Cadence {
    double begin = 0;
    double first = -1;
    double previous = -1;
    double largest = 0;
    double worst_error = 0;
    double squared = 0;
    unsigned long long marks = 0;
    std::vector<double> absolute_errors;
    void mark(double ideal_at)
    {
        const double at = now_ms() - begin;
        if (first < 0) first = at;
        if (previous >= 0) largest = (std::max)(largest, at - previous);
        previous = at;
        const double error = at - (ideal_at - begin);
        absolute_errors.push_back(std::fabs(error));
        if (std::fabs(error) > worst_error) worst_error = std::fabs(error);
        squared += error * error;
        ++marks;
    }
    double rms() const { return marks ? std::sqrt(squared / static_cast<double>(marks)) : 0.0; }
};

bool run_steady(AudioSink& sink, const BlockFormat& source, uint32_t block_frames, int blocks, Phase& phase)
{
    const auto pcm = tone(source.rate, block_frames, 220.0);
    const Report before = sink.report();
    Cadence cadence;
    cadence.begin = now_ms();
    for (int i = 0; i < blocks; ++i) {
        const double want = cadence.begin + i * (1000.0 * block_frames / source.rate);
        while (now_ms() < want) Sleep(1);
        if (submit_with_wait(sink, source, pcm) != Submit::Accepted) return false;
        cadence.mark(want);
        sink.observe(false);
    }
    sink.observe(false);
    const Report after = sink.report();
    phase = delta(before, after, "steady");
    phase.first_submit_ms = cadence.first;
    phase.pacing_error_max_ms = cadence.worst_error;
    phase.pacing_error_rms_ms = cadence.rms();
    phase.pacing_error_p95_ms = pacing_percentile95(cadence.absolute_errors);
    phase.largest_gap_ms = cadence.largest;
    return true;
}

bool run_bursty(AudioSink& sink, const BlockFormat& source, uint32_t block_frames, int bursts, int per_burst, int gap_ms,
                Phase& phase)
{
    const auto pcm = tone(source.rate, block_frames, 220.0);
    const Report before = sink.report();
    Cadence cadence;
    cadence.begin = now_ms();
    for (int b = 0; b < bursts; ++b) {
        for (int i = 0; i < per_burst; ++i) {
            if (submit_with_wait(sink, source, pcm) != Submit::Accepted) return false;
            cadence.mark(cadence.begin + (b * per_burst + i) * (1000.0 * block_frames / source.rate));
            sink.observe(false);
        }
        const double until = now_ms() + gap_ms;
        while (now_ms() < until) {
            sink.observe(true);
            Sleep(5);
        }
    }
    sink.observe(false);
    const Report after = sink.report();
    phase = delta(before, after, "bursty");
    phase.first_submit_ms = cadence.first;
    phase.pacing_error_max_ms = cadence.worst_error;
    phase.pacing_error_rms_ms = cadence.rms();
    phase.pacing_error_p95_ms = pacing_percentile95(cadence.absolute_errors);
    phase.largest_gap_ms = cadence.largest;
    return true;
}
}  // namespace

namespace {
// Kept apart from main so the sink is destroyed before CoUninitialize runs.
int run()
{
    std::vector<double> timing_control(120, 1.0);
    timing_control.front() = 75.0;
    if (pacing_percentile95(timing_control) != 1.0) {
        std::puts("FAIL: one isolated timing outlier changed p95");
        return 1;
    }
    std::fill_n(timing_control.begin(), 7, 75.0);
    if (pacing_percentile95(timing_control) != 75.0) {
        std::puts("FAIL: sustained timing errors did not change p95");
        return 1;
    }

    AudioSink sink;
    const std::string error = sink.open();
    if (!error.empty()) {
        std::printf("skip: no usable endpoint: %s\n", error.c_str());
        return skip_no_endpoint;
    }
    const BlockFormat mix = sink.mix_format();
    std::printf("endpoint: %s, %s %u ch %u Hz, buffer %u frames\n", sink.endpoint().c_str(),
                mix.kind == SampleKind::Float32 ? "float32" : "pcm16", mix.channels, mix.rate, sink.buffer_frames());

    const BlockFormat source{SampleKind::Pcm16, 1, mix.rate};
    const uint32_t block_frames = mix.rate / 100;  // 10 ms
    if (block_frames == 0) return 2;

    // The same block count in both phases, so the only difference is pacing.
    constexpr int total_blocks = 120;
    constexpr int bursts = 20, per_burst = total_blocks / bursts;  // 6 x 20 = 120

    Phase steady{}, bursty{};
    if (!run_steady(sink, source, block_frames, total_blocks, steady)) return 3;
    if (!run_bursty(sink, source, block_frames, bursts, per_burst, 150, bursty)) return 4;

    print_phase(steady);
    print_phase(bursty);
    std::printf("pacing error: steady max=%.1f ms rms=%.1f ms; bursty max=%.1f ms rms=%.1f ms\n",
                steady.pacing_error_max_ms, steady.pacing_error_rms_ms, bursty.pacing_error_max_ms,
                bursty.pacing_error_rms_ms);
    std::printf("comparison: blocks %llu/%llu frames %llu/%llu drained steady=%.1f ms bursty=%.1f ms\n", steady.blocks,
                bursty.blocks, steady.frames, bursty.frames, steady.drained_ms, bursty.drained_ms);

    if (!steady.blocks || !bursty.blocks) {
        std::printf("FAIL: no accepted blocks in a phase\n");
        return 1;
    }
    if (steady.frames != bursty.frames) {
        std::printf("FAIL: phases did not submit the same total audio (%llu vs %llu frames)\n", steady.frames, bursty.frames);
        return 1;
    }
    if (bursty.drained_ms <= steady.drained_ms + 20.0) {
        std::printf("FAIL: bursty input did not register a longer drained interval than steady input "
                    "(%.1f vs %.1f ms); the measurement cannot see producer stutter\n", bursty.drained_ms,
                    steady.drained_ms);
        return 1;
    }
    if (bursty.recovery_ms < 0) {
        std::printf("FAIL: bursty phase never recovered (recovery not measured)\n");
        return 1;
    }
    // Sensitivity first: if the metric cannot separate an evenly paced feed from one that withholds
    // blocks, it cannot speak about the title's stutter either. The steady bound is a few measured timer
    // ticks, because the feed is scheduled with Sleep(1) and cannot beat the host's tick.
    const double tick_ms = sleep_granularity_ms();
    const double jitter_ms = calibrate_scheduler_ms(block_frames, source.rate, total_blocks);
    const double steady_limit_ms = (std::max)(3.0 * tick_ms, 2.0 * jitter_ms + 5.0);
    std::printf("timer: Sleep(1) median %.1f ms, scheduler worst %.1f ms over %d wake-ups, steady bound %.1f ms\n",
                tick_ms, jitter_ms, total_blocks, steady_limit_ms);
    std::printf("pacing p95: steady %.1f ms, bursty %.1f ms\n", steady.pacing_error_p95_ms, bursty.pacing_error_p95_ms);
    if (steady.pacing_error_p95_ms > steady_limit_ms) {
        std::printf("FAIL: steady p95 drifted %.1f ms from the ideal cadence, beyond the scheduler bound (%.1f ms)\n",
                    steady.pacing_error_p95_ms, steady_limit_ms);
        return 1;
    }
    if (bursty.pacing_error_p95_ms <= steady.pacing_error_p95_ms + 5.0 * tick_ms) {
        std::printf("FAIL: bursty feed did not drift further than steady (%.1f vs %.1f ms); the metric "
                    "cannot see producer stutter\n",
                    bursty.pacing_error_p95_ms, steady.pacing_error_p95_ms);
        return 1;
    }
    std::printf("PASS: same total audio, bursty feed shows a drained interval steady feed does not, recovery %.1f ms\n",
                bursty.recovery_ms);
    return 0;
}
}  // namespace

int main()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::printf("skip: COM initialisation failed\n");
        return skip_no_endpoint;
    }
    const int result = run();
    CoUninitialize();
    return result;
}
