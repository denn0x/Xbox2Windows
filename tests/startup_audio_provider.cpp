// Voice-lifecycle control for the playback-timing slice of #1145 (slice 2).
//
// The client here is deliberately not the title: it drives one small provider interface through a
// steady and a bursty submission plan, and it is given either of two explicit providers:
//
//   diagnostic - records the voice lifecycle, buffer arguments and callbacks, and advances a virtual
//                clock as the plan is pumped; it plays no audio and touches no device;
//   system     - calls XAudio2Create and the real IXAudio2 directly. No import patching, no hooking:
//                the "diagnostics disabled" arm visibly calls the system provider.
//
// The precondition the slice starts from is that the diagnostic arm is a faithful window, so the
// fixture first checks that both arms observe the same lifecycle and the same buffer arguments for
// the same plan. If they diverge it fails there and prints the first divergence, because a timing
// number from an unfaithful window would be worthless.
//
// What is recorded (and nothing else): timestamps, frame counts, queue depth, drop counts, clock
// progress, callbacks observed, and buffer arguments. Audio bytes are never recorded or stored.
#include <xaudio2.h>

#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int skip_no_device = 77;

struct Format {
    unsigned rate = 48000, channels = 2, bits = 16;
};

struct VoiceState {
    unsigned long long queued_frames = 0;
    unsigned long long played_frames = 0;
};

// One entry per lifecycle or submission event, in order. Kept independent of wall-clock time so two
// arms running on different clocks can be compared event for event.
struct Event {
    const char* kind;  // "create", "submit", "start", "stop"
    unsigned long long arg = 0;
    bool operator==(const Event& other) const { return std::strcmp(kind, other.kind) == 0 && arg == other.arg; }
};

struct Trace {
    std::vector<Event> events;
    unsigned long long submitted_frames = 0;
    unsigned long long submitted_buffers = 0;
    unsigned long long callbacks = 0;
    unsigned long long drops = 0;
    unsigned long long queue_depth_max = 0;
    unsigned long long clock_frames = 0;
    bool started = false, stopped = false;
};

class Voice {
public:
    virtual ~Voice() = default;
    // frames of quiet PCM; the plan owns the bytes so the provider never has to store audio content.
    virtual bool submit(unsigned frames) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual VoiceState state() = 0;
    virtual Trace& trace() = 0;
    virtual bool callback_failed() const = 0;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual const char* name() const = 0;
    virtual std::string open() = 0;  // empty on success
    virtual Voice* create_voice(const Format&) = 0;
    virtual void close() = 0;
};

// ---------------------------------------------------------------------------------------------------
// Diagnostic provider: no device, virtual clock, records the lifecycle the client drives.
class DiagnosticVoice final : public Voice {
public:
    explicit DiagnosticVoice(Format format) : format_(format) {}
    bool submit(unsigned frames) override
    {
        if (queue_ + frames > capacity_frames) {
            ++trace_.drops;
            return false;
        }
        queue_ += frames;
        ++in_flight_;
        trace_.submitted_frames += frames;
        ++trace_.submitted_buffers;
        trace_.events.push_back({"submit", frames});
        drain();
        return true;
    }
    void start() override
    {
        trace_.started = true;
        trace_.events.push_back({"start", 0});
    }
    void stop() override
    {
        trace_.stopped = true;
        trace_.events.push_back({"stop", 0});
    }
    VoiceState state() override
    {
        VoiceState s;
        s.queued_frames = queue_;
        s.played_frames = played_;
        return s;
    }
    Trace& trace() override { return trace_; }
    bool callback_failed() const override { return false; }

    // The plan calls this once per submission period, so the virtual clock advances in the same steps
    // as the real one would and the callbacks line up with the same submissions.
    void advance(unsigned frames)
    {
        const unsigned consumed = (std::min)(frames, static_cast<unsigned>(queue_));
        queue_ -= consumed;
        played_ += consumed;
        trace_.clock_frames += consumed;
        while (in_flight_ > 0 && played_ >= next_callback_at_) {
            --in_flight_;
            ++trace_.callbacks;
            next_callback_at_ += block_frames_;
        }
        trace_.queue_depth_max = (std::max)(trace_.queue_depth_max, queue_);
    }

private:
    void drain() { trace_.queue_depth_max = (std::max)(trace_.queue_depth_max, queue_); }
    Format format_;
    unsigned capacity_frames = 4800;  // 100 ms
    unsigned block_frames_ = 480;     // 10 ms
    unsigned long long queue_ = 0, played_ = 0, next_callback_at_ = 480;
    int in_flight_ = 0;
    Trace trace_;
};

class DiagnosticProvider final : public Provider {
public:
    ~DiagnosticProvider() override { close(); }
    const char* name() const override { return "diagnostic"; }
    std::string open() override { return {}; }
    Voice* create_voice(const Format& format) override
    {
        auto* voice = new DiagnosticVoice(format);
        voice->trace().events.push_back({"create", format.rate});
        voices_.push_back(voice);
        return voice;
    }
    void close() override
    {
        for (Voice* voice : voices_) delete voice;
        voices_.clear();
    }
    void advance(unsigned frames)
    {
        for (Voice* voice : voices_)
            if (auto* diagnostic = dynamic_cast<DiagnosticVoice*>(voice)) diagnostic->advance(frames);
    }

private:
    std::vector<Voice*> voices_;
};

// ---------------------------------------------------------------------------------------------------
// System provider: the real thing, called directly.
class SystemVoice final : public Voice, public IXAudio2VoiceCallback {
public:
    SystemVoice(IXAudio2* engine, Format format) : engine_(engine), format_(format) {}
    ~SystemVoice() override
    {
        if (voice_) voice_->DestroyVoice();
        if (buffers_) delete[] buffers_;
    }
    std::string create()
    {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = static_cast<WORD>(format_.channels);
        wf.nSamplesPerSec = format_.rate;
        wf.wBitsPerSample = static_cast<WORD>(format_.bits);
        wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        const HRESULT hr = engine_->CreateSourceVoice(&voice_, &wf, 0, 1.0f, this);
        if (FAILED(hr)) return "CreateSourceVoice failed";
        trace_.events.push_back({"create", format_.rate});
        return {};
    }
    bool submit(unsigned frames) override
    {
        if (submit_index_ >= max_blocks) return false;
        XAUDIO2_BUFFER& buffer = buffers_[submit_index_];
        buffer = {};
        const size_t bytes = static_cast<size_t>(frames) * (format_.bits / 8) * format_.channels;
        if (bytes > pcm_.size() || bytes > UINT32_MAX) return false;
        buffer.AudioBytes = static_cast<UINT32>(bytes);
        buffer.pAudioData = pcm_.data();  // one shared quiet block; content is never recorded
        buffer.pContext = &buffer;
        const HRESULT hr = voice_->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            ++trace_.drops;
            return false;
        }
        ++submit_index_;
        trace_.submitted_frames += frames;
        ++trace_.submitted_buffers;
        trace_.events.push_back({"submit", frames});
        return true;
    }
    void start() override
    {
        if (SUCCEEDED(voice_->Start(0))) {
            trace_.started = true;
            trace_.events.push_back({"start", 0});
        }
    }
    void stop() override
    {
        if (SUCCEEDED(voice_->Stop(0))) {
            trace_.stopped = true;
            trace_.events.push_back({"stop", 0});
        }
    }
    VoiceState state() override
    {
        XAUDIO2_VOICE_STATE s{};
        voice_->GetState(&s, 0);
        VoiceState out;
        out.queued_frames = s.BuffersQueued * block_frames_;
        out.played_frames = s.SamplesPlayed;
        trace_.queue_depth_max = (std::max)(trace_.queue_depth_max, out.queued_frames);
        trace_.clock_frames = out.played_frames;
        return out;
    }
    Trace& trace() override
    {
        trace_.callbacks = completed_.load();
        return trace_;
    }
    bool callback_failed() const override { return callback_error_.load(); }
    void prepare(unsigned block_frames)
    {
        block_frames_ = block_frames;
        pcm_.assign(static_cast<size_t>(block_frames) * format_.channels * (format_.bits / 8), 0);
        buffers_ = new XAUDIO2_BUFFER[max_blocks]{};
    }
    // Voice callbacks update only atomics; the driving thread owns the trace.
    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void*) override {}
    void __stdcall OnBufferEnd(void* context) override
    {
        const unsigned index = completed_.load();
        if (index >= max_blocks || context != &buffers_[index]) callback_error_.store(true);
        completed_.fetch_add(1);
    }
    void __stdcall OnLoopEnd(void*) override {}
    void __stdcall OnVoiceError(void*, HRESULT) override { callback_error_.store(true); }

private:
    static constexpr int max_blocks = 256;
    IXAudio2* engine_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    Format format_{};
    std::vector<unsigned char> pcm_;
    XAUDIO2_BUFFER* buffers_ = nullptr;
    int submit_index_ = 0;
    unsigned block_frames_ = 480;
    Trace trace_;
    std::atomic<unsigned> completed_{0};
    std::atomic<bool> callback_error_{false};
};

class SystemProvider final : public Provider {
public:
    ~SystemProvider() override { close(); }
    const char* name() const override { return "system"; }
    std::string open() override
    {
        const HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return "XAudio2Create failed";
        const HRESULT master = engine_->CreateMasteringVoice(&master_);
        if (FAILED(master)) return "CreateMasteringVoice failed (no endpoint)";
        return {};
    }
    Voice* create_voice(const Format& format) override
    {
        auto* voice = new SystemVoice(engine_, format);
        if (!voice->create().empty()) {
            delete voice;
            return nullptr;
        }
        voices_.push_back(voice);
        return voice;
    }
    void close() override
    {
        for (SystemVoice* voice : voices_) delete voice;
        voices_.clear();
        if (master_) {
            master_->DestroyVoice();
            master_ = nullptr;
        }
        if (engine_) {
            engine_->Release();
            engine_ = nullptr;
        }
    }

private:
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    std::vector<SystemVoice*> voices_;
};

// ---------------------------------------------------------------------------------------------------
struct Plan {
    const char* name;
    bool bursty;
    int blocks;
    int block_frames;
    int gap_ms;
};

// Drives one plan through a provider. Returns false when a submission was refused.
bool run_plan(Provider& provider, const Plan& plan, Trace& trace, unsigned long long& elapsed_ms)
{
    const Format format{48000, 2, 16};
    Voice* voice = provider.create_voice(format);
    if (!voice) return false;
    auto* system_voice = dynamic_cast<SystemVoice*>(voice);
    if (system_voice) system_voice->prepare(plan.block_frames);
    voice->start();
    if (!voice->trace().started) return false;

    const ULONGLONG begin = GetTickCount64();
    int submitted = 0;
    while (submitted < plan.blocks) {
        const int batch = plan.bursty ? 6 : 1;
        for (int i = 0; i < batch && submitted < plan.blocks; ++i) {
            if (voice->callback_failed() || !voice->submit(plan.block_frames)) return false;
            ++submitted;
            // The clock is advanced only while waiting, not between back-to-back submits: playback
            // consumes during the wait, which is what makes the callback order match the system arm.
        }
        // Sample after the batch lands and before playback drains it, so queue depth is a real depth.
        voice->state();
        const int wait_ms = plan.bursty ? plan.gap_ms : 10;
        for (int waited = 0; waited < wait_ms; waited += 10) {
            Sleep(10);
            // Time passes whether or not the producer submits: the window must model that, or the
            // bursty arm is unfaithful by construction.
            if (auto* diagnostic = dynamic_cast<DiagnosticVoice*>(voice)) diagnostic->advance(plan.block_frames);
        }
        voice->state();
    }
    // Drain: let the system arm play out, and advance the diagnostic clock the same distance.
    const ULONGLONG deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline) {
        if (auto* diagnostic = dynamic_cast<DiagnosticVoice*>(voice)) diagnostic->advance(plan.block_frames);
        if (voice->trace().callbacks >= static_cast<unsigned long long>(submitted)) break;
        Sleep(5);
        voice->state();
    }
    voice->stop();
    trace = voice->trace();
    elapsed_ms = GetTickCount64() - begin;
    const bool complete = trace.stopped && trace.callbacks == trace.submitted_buffers && !voice->callback_failed();
    provider.close();
    return complete;
}

bool same_lifecycle(const Trace& a, const Trace& b, std::string& why)
{
    if (a.events.size() != b.events.size()) {
        why = "event counts differ (" + std::to_string(a.events.size()) + " vs " + std::to_string(b.events.size()) + ")";
        return false;
    }
    for (size_t i = 0; i < a.events.size(); ++i) {
        if (!(a.events[i] == b.events[i])) {
            why = std::string("first divergence at event ") + std::to_string(i) + ": " + a.events[i].kind + "/" +
                  std::to_string(a.events[i].arg) + " vs " + b.events[i].kind + "/" + std::to_string(b.events[i].arg);
            return false;
        }
    }
    return true;
}
int run()
{
    bool ok = true;
    for (const Plan& plan : {Plan{"steady", false, 120, 480, 0}, Plan{"bursty", true, 120, 480, 150}}) {
        // Both arms run the same plan; the diagnostic arm is the window being validated.
        DiagnosticProvider diagnostic;
        if (!diagnostic.open().empty()) return 2;
        Trace dtrace;
        unsigned long long ds = 0;
        if (!run_plan(diagnostic, plan, dtrace, ds)) return 3;

        SystemProvider system;
        const std::string error = system.open();
        if (!error.empty()) {
            std::printf("skip: system provider unavailable: %s\n", error.c_str());
            return skip_no_device;
        }
        Trace strace;
        unsigned long long ss = 0;
        if (!run_plan(system, plan, strace, ss)) return 4;

        std::string why;
        const bool faithful = same_lifecycle(dtrace, strace, why);
        std::printf("%s: faithful=%s%s%s\n", plan.name, faithful ? "yes" : "no", faithful ? "" : " reason=",
                    faithful ? "" : why.c_str());
        std::printf("  diagnostic: events=%zu frames=%llu callbacks=%llu drops=%llu depth_max=%llu clock=%llu "
                    "elapsed=%llums\n",
                    dtrace.events.size(), dtrace.submitted_frames, dtrace.callbacks, dtrace.drops,
                    dtrace.queue_depth_max, dtrace.clock_frames, ds);
        std::printf("  system    : events=%zu frames=%llu callbacks=%llu drops=%llu depth_max=%llu clock=%llu "
                    "elapsed=%llums\n",
                    strace.events.size(), strace.submitted_frames, strace.callbacks, strace.drops, strace.queue_depth_max,
                    strace.clock_frames, ss);
        if (!faithful) ok = false;
        if (strace.callbacks != strace.submitted_buffers) {
            std::printf("  WARN: system arm observed %llu callbacks for %llu buffers\n", strace.callbacks,
                        strace.submitted_buffers);
        }
    }

    if (!ok) {
        std::printf("FAIL: the diagnostic arm did not observe the same lifecycle as the system arm\n");
        return 1;
    }
    std::printf("PASS: diagnostic and system providers observed the same lifecycle and buffer arguments\n");
    return 0;
}
}  // namespace

int main()
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::printf("COM initialisation failed\n");
        return 2;
    }
    const int result = run();
    CoUninitialize();
    return result;
}
