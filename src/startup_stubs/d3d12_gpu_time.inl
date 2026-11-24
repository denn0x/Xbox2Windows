// LOCAL: X2W_CX_GPU_TIME=1, GPU time of translated compute dispatches, per compute key.
//
// Each list that records a translated dispatch gets a timestamp query heap (2 queries per dispatch, up to
// gpu_time_cap dispatches per recording) and two readback buffers. EndQuery brackets each Dispatch. At Close the
// queries are resolved into the readback buffer of this recording (the two alternate, so a list resubmitted
// once before its result is read does not overwrite it). After ExecuteCommandLists, one Signal on a fence of
// that queue (one fence per queue, so completed values only grow) marks the submission. The results are read
// once the fence passes: at a later submit or list Reset, never with a CPU wait. Every 300 frames, the ten keys
// with the most GPU time are logged.
//
// What a value means: the time between the timestamp before and after the Dispatch on the GPU timeline. Each
// translated dispatch ends in a UAV barrier, so dispatches of one list run one after another. Work of other
// lists or queues can still overlap and is then counted too.

namespace gpu_time {
constexpr UINT cap = 2048;  // dispatches per recording
struct ListState {
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback[2] = {};
    std::vector<u64> keys;      // this recording, in query order
    int slot = 0;               // readback buffer of this recording
    bool resolved = false;      // resolved at Close, not yet submitted
    struct Pending {
        std::vector<u64> keys;
        ID3D12Fence* fence = nullptr;
        UINT64 value = 0, frequency = 0;
        int slot = 0;
    };
    Pending pending[2];         // per readback buffer
    bool has_pending[2] = {};
    bool failed = false;
};
struct QueueFence { ID3D12Fence* fence = nullptr; UINT64 value = 0; };
struct KeyTime { double total_ms = 0, max_ms = 0; LONG64 calls = 0; };

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<Wrapper*, ListState>& lists() { static std::unordered_map<Wrapper*, ListState> m; return m; }
std::unordered_map<ID3D12CommandQueue*, QueueFence>& fences() { static std::unordered_map<ID3D12CommandQueue*, QueueFence> m; return m; }
std::unordered_map<u64, KeyTime>& times() { static std::unordered_map<u64, KeyTime> m; return m; }
LONG timed, over_cap, overwritten, harvested, failures;

bool on()
{
    static const bool enabled = [] {
        const bool e = x2w_local::env_on("X2W_CX_GPU_TIME");
        if (e) log("x2w-cx X2W_CX_GPU_TIME: timestamps around translated dispatches, read back after the queue's fence passes");
        return e;
    }();
    return enabled;
}

bool ensure(ListState& s, ID3D12GraphicsCommandList* list)
{
    if (s.heap) return true;
    if (s.failed) return false;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    ID3D12Device* device = device_of(the_device);
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * cap, 0};
    HRESULT hr = (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
                     ? device->CreateQueryHeap(&qd, IID_PPV_ARGS(&s.heap)) : E_NOTIMPL;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = u64{2} * cap * 8;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&s.readback[i]));
    if (SUCCEEDED(hr)) return true;
    if (s.heap) s.heap->Release(), s.heap = nullptr;
    for (auto*& r : s.readback)
        if (r) r->Release(), r = nullptr;
    s.failed = true;
    InterlockedIncrement(&failures);
    static LONG logged;
    if (first_calls(logged, 4)) log("x2w-cx gpu time: query heap or readback for a list type %d failed 0x%08lx", static_cast<int>(type), static_cast<unsigned long>(hr));
    return false;
}

// Reads every pending result whose fence has passed (lock held).
void harvest_locked()
{
    for (auto& entry : lists()) {
        ListState& s = entry.second;
        for (int b = 0; b < 2; ++b) {
            if (!s.has_pending[b]) continue;
            ListState::Pending& p = s.pending[b];
            if (p.fence->GetCompletedValue() < p.value) continue;
            s.has_pending[b] = false;
            const std::size_t n = p.keys.size();
            const D3D12_RANGE range{0, n * 16};
            void* mapped = nullptr;
            if (n && p.frequency && SUCCEEDED(s.readback[b]->Map(0, &range, &mapped))) {
                const auto* ticks = static_cast<const UINT64*>(mapped);
                for (std::size_t i = 0; i < n; ++i) {
                    if (ticks[2 * i + 1] < ticks[2 * i]) continue;  // not written or wrapped
                    const double ms = static_cast<double>(ticks[2 * i + 1] - ticks[2 * i]) * 1000.0 / static_cast<double>(p.frequency);
                    KeyTime& t = times()[p.keys[i]];
                    t.total_ms += ms;
                    ++t.calls;
                    if (ms > t.max_ms) t.max_ms = ms;
                }
                const D3D12_RANGE none{0, 0};
                s.readback[b]->Unmap(0, &none);
                InterlockedIncrement(&harvested);
            }
        }
    }
}

// Around the Dispatch of execute_translated (d3d12_compute.inl).
void dispatch(Wrapper* self, ID3D12GraphicsCommandList* list, u64 key, bool after)
{
    if (!on()) return;
    AcquireSRWLockExclusive(&lock);
    ListState& s = lists()[self];
    if (!after) {
        if (s.keys.size() >= cap) {
            InterlockedIncrement(&over_cap);
        } else if (ensure(s, list)) {
            list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * s.keys.size()));
            s.keys.push_back(key);
            s.keys.push_back(~0ull);  // the "after" half is still missing
        }
    } else if (s.keys.size() >= 2 && s.keys.back() == ~0ull) {
        s.keys.pop_back();
        list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * s.keys.size() - 1));
        InterlockedIncrement(&timed);
    }
    ReleaseSRWLockExclusive(&lock);
}

// list_close, before the desktop Close.
void close(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    if (!on()) return;
    AcquireSRWLockExclusive(&lock);
    const auto found = lists().find(self);
    if (found != lists().end() && found->second.heap) {
        ListState& s = found->second;
        if (!s.keys.empty() && s.keys.back() == ~0ull) s.keys.pop_back(), s.keys.pop_back();  // a begin without its end
        if (!s.keys.empty()) {
            s.slot = s.has_pending[0] && !s.has_pending[1] ? 1 : 0;
            list->ResolveQueryData(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, static_cast<UINT>(2 * s.keys.size()), s.readback[s.slot], 0);
            s.resolved = true;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

// queue_execute, after the desktop ExecuteCommandLists.
void submitted(ID3D12CommandQueue* queue, const std::vector<Wrapper*>& wrappers)
{
    if (!on()) return;
    AcquireSRWLockExclusive(&lock);
    std::vector<ListState*> marked;
    for (Wrapper* w : wrappers) {
        const auto found = lists().find(w);
        if (found != lists().end() && found->second.resolved && !found->second.keys.empty()) marked.push_back(&found->second);
    }
    if (!marked.empty()) {
        QueueFence& f = fences()[queue];
        if (!f.fence && FAILED(device_of(the_device)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence)))) f.fence = nullptr;
        UINT64 frequency = 0;
        if (f.fence && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && SUCCEEDED(queue->Signal(f.fence, ++f.value))) {
            for (ListState* s : marked) {
                const int b = s->slot;
                if (s->has_pending[b]) InterlockedIncrement(&overwritten);
                s->pending[b] = {s->keys, f.fence, f.value, frequency, b};
                s->has_pending[b] = true;
            }
        } else {
            InterlockedIncrement(&failures);
        }
    }
    harvest_locked();
    ReleaseSRWLockExclusive(&lock);
}

// list_reset: a new recording.
void reset(Wrapper* self)
{
    if (!on()) return;
    AcquireSRWLockExclusive(&lock);
    harvest_locked();
    const auto found = lists().find(self);
    if (found != lists().end()) {
        found->second.keys.clear();
        found->second.resolved = false;
    }
    ReleaseSRWLockExclusive(&lock);
}

// Every 300 frames.
void report()
{
    if (!on()) return;
    std::vector<std::pair<u64, KeyTime>> all;
    AcquireSRWLockExclusive(&lock);
    harvest_locked();
    for (const auto& entry : times()) all.push_back(entry);
    times().clear();
    ReleaseSRWLockExclusive(&lock);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.total_ms > b.second.total_ms; });
    double total = 0;
    LONG64 calls = 0;
    for (const auto& entry : all) total += entry.second.total_ms, calls += entry.second.calls;
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "x2w-cx gpu time (300 frames): %.1f ms in %lld timed dispatches of %zu keys; top:", total, calls,
                          all.size());
    for (std::size_t i = 0; i < all.size() && i < 10; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %016llx %.2f ms/%lld (max %.3f)", all[i].first, all[i].second.total_ms,
                           all[i].second.calls, all[i].second.max_ms);
    log("%s", line);
    log("x2w-cx gpu time counters: timed %ld, over %u per list %ld, results read %ld, overwritten before read %ld, failures %ld",
        InterlockedExchange(&timed, 0), cap, InterlockedExchange(&over_cap, 0), InterlockedExchange(&harvested, 0),
        InterlockedExchange(&overwritten, 0), InterlockedExchange(&failures, 0));
}
}  // namespace gpu_time
