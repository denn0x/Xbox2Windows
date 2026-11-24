// LOCAL: X2W_CX_GPU_TIME=1, GPU time of translated compute dispatches, per compute key.
//
// Each list that records a timed item gets a timestamp query heap (2 queries per item, up to cap() items per
// recording) and two readback buffers. EndQuery brackets each item. At Close the queries are resolved into the
// readback buffer of this recording (the two alternate, so a list resubmitted once before its result is read
// does not overwrite it). After ExecuteCommandLists, one Signal on a fence of that queue (one fence per queue,
// so completed values only grow) marks the submission. The results are read once the fence passes: at a later
// submit or list Reset, never with a CPU wait. Every 300 frames, the ten keys with the most GPU time are logged.
//
// LOCAL stall: X2W_GPU_TIME_CLASSES=1 (implies X2W_CX_GPU_TIME) also times translated draws (by pipeline key),
// slot-62 desktop compute dispatches (by compute key), GPU buffer copies (X2W_D3D12_COPY_BUFFER=gpu) and each
// whole recording of a direct or compute list (from Reset to Close, keyed by list type). Per recording, the
// list span minus the items inside it is "untimed": barriers, state changes and work of the title's own
// desktop pipelines that is not bracketed. Extra lines "x2w-gpu time ..." per 300 frames; the X2W_CX_GPU_TIME
// lines keep their format.
//
// What a value means: the time between the timestamp before and after the item on the GPU timeline. Each
// translated dispatch ends in a UAV barrier, so dispatches of one list run one after another. Draws are not
// separated by barriers: their intervals overlap and their sum can exceed the list span. Work of other lists
// or queues can still overlap any item and is then counted too, so a per-key total is an upper bound.

namespace gpu_time {
// enum Class (c_cx, c_draw, c_c62, c_copy, c_list, c_count) is declared with the forward declarations in d3d12_xs_device.cpp
const char* const class_names[c_count] = {"cx", "draw", "c62", "copy", "list"};
struct Item { u64 key; int cls; };
struct ListState {
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback[2] = {};
    std::vector<Item> items;    // this recording, in query order (item i: queries 2i, 2i + 1)
    int open = -1;              // the item whose end is still missing (cx, draw, c62, copy)
    int open_list = -1;         // the recording span whose end is still missing
    int slot = 0;               // readback buffer of this recording
    int type = 0;               // desktop list type (direct 0, compute 2)
    bool resolved = false;      // resolved at Close, not yet submitted
    struct Pending {
        std::vector<Item> items;
        ID3D12Fence* fence = nullptr;
        UINT64 value = 0, frequency = 0;
        int slot = 0;
        int type = 0;
    };
    Pending pending[2];         // per readback buffer
    bool has_pending[2] = {};
    bool failed = false;
};
struct QueueFence { ID3D12Fence* fence = nullptr; UINT64 value = 0; };
struct KeyTime { double total_ms = 0, max_ms = 0; LONG64 calls = 0, over_1ms = 0; };

SRWLOCK lock = SRWLOCK_INIT;
std::unordered_map<Wrapper*, ListState>& lists() { static std::unordered_map<Wrapper*, ListState> m; return m; }
std::unordered_map<ID3D12CommandQueue*, QueueFence>& fences() { static std::unordered_map<ID3D12CommandQueue*, QueueFence> m; return m; }
std::unordered_map<u64, KeyTime>& times(int cls) { static std::unordered_map<u64, KeyTime> m[c_count]; return m[cls]; }
double untimed_ms[4], span_ms[4], cx_ms[4];  // by list type (direct 0, bundle 1, compute 2, copy 3)
LONG64 spans[4], cx_calls[4];
LONG timed, over_cap, overwritten, harvested, failures;

bool classes()
{
    static const bool enabled = [] {
        const bool e = x2w_local::env_on("X2W_GPU_TIME_CLASSES");
        if (e) log("x2w-gpu time X2W_GPU_TIME_CLASSES: also timing translated draws, slot-62 dispatches, GPU copies and list spans");
        return e;
    }();
    return enabled;
}
bool on()
{
    static const bool enabled = [] {
        const bool e = x2w_local::env_on("X2W_CX_GPU_TIME") || classes();
        if (e) log("x2w-cx X2W_CX_GPU_TIME: timestamps around translated dispatches, read back after the queue's fence passes");
        return e;
    }();
    return enabled;
}
UINT cap() { return classes() ? 4096 : 2048; }  // items per recording

bool ensure(ListState& s, ID3D12GraphicsCommandList* list)
{
    if (s.heap) return true;
    if (s.failed) return false;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    ID3D12Device* device = device_of(the_device);
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * cap(), 0};
    HRESULT hr = (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
                     ? device->CreateQueryHeap(&qd, IID_PPV_ARGS(&s.heap)) : E_NOTIMPL;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = u64{2} * cap() * 8;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&s.readback[i]));
    if (SUCCEEDED(hr)) {
        s.type = static_cast<int>(type) & 3;
        return true;
    }
    if (s.heap) s.heap->Release(), s.heap = nullptr;
    for (auto*& r : s.readback)
        if (r) r->Release(), r = nullptr;
    s.failed = true;
    InterlockedIncrement(&failures);
    static LONG logged;
    if (first_calls(logged, 4)) log("x2w-cx gpu time: query heap or readback for a list type %d failed 0x%08lx", static_cast<int>(type), static_cast<unsigned long>(hr));
    return false;
}

void add(int cls, u64 key, double ms)
{
    KeyTime& t = times(cls)[key];
    t.total_ms += ms;
    ++t.calls;
    if (ms > 1.0) ++t.over_1ms;
    if (ms > t.max_ms) t.max_ms = ms;
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
            const std::size_t n = p.items.size();
            const D3D12_RANGE range{0, n * 16};
            void* mapped = nullptr;
            if (n && p.frequency && SUCCEEDED(s.readback[b]->Map(0, &range, &mapped))) {
                const auto* ticks = static_cast<const UINT64*>(mapped);
                double inside = 0, span = -1;
                int type = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    if (ticks[2 * i + 1] < ticks[2 * i]) continue;  // not written or wrapped
                    const double ms = static_cast<double>(ticks[2 * i + 1] - ticks[2 * i]) * 1000.0 / static_cast<double>(p.frequency);
                    add(p.items[i].cls, p.items[i].key, ms);
                    if (p.items[i].cls == c_list) span = ms, type = static_cast<int>(p.items[i].key & 3);
                    else inside += ms;
                    if (p.items[i].cls == c_cx) cx_ms[p.type] += ms, ++cx_calls[p.type];
                }
                if (span >= 0) {
                    span_ms[type] += span;
                    untimed_ms[type] += span > inside ? span - inside : 0;
                    ++spans[type];
                }
                const D3D12_RANGE none{0, 0};
                s.readback[b]->Unmap(0, &none);
                InterlockedIncrement(&harvested);
            }
        }
    }
}

// Opens (after == false) or closes the item of class `cls`; `drop` on the close forgets the item (nothing was
// recorded between the two marks).
void mark(Wrapper* self, ID3D12GraphicsCommandList* list, int cls, u64 key, bool after, bool drop)
{
    if (!on() || (cls != c_cx && !classes()) || !list) return;
    AcquireSRWLockExclusive(&lock);
    ListState& s = lists()[self];
    if (!after) {
        if (s.open >= 0) {
            // an item without its end (should not happen): forget it
            if (s.open == static_cast<int>(s.items.size()) - 1) s.items.pop_back();
            s.open = -1;
        }
        if (s.items.size() >= cap()) {
            InterlockedIncrement(&over_cap);
        } else if (ensure(s, list)) {
            list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * s.items.size()));
            s.open = static_cast<int>(s.items.size());
            s.items.push_back({key, cls});
        }
    } else if (s.open >= 0 && s.items[s.open].cls == cls) {
        if (drop && s.open == static_cast<int>(s.items.size()) - 1) {
            s.items.pop_back();  // its begin query index is reused by the next item
        } else {
            list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * s.open + 1));
            InterlockedIncrement(&timed);
        }
        s.open = -1;
    }
    ReleaseSRWLockExclusive(&lock);
}

// Around the Dispatch of execute_translated (d3d12_compute.inl).
void dispatch(Wrapper* self, ID3D12GraphicsCommandList* list, u64 key, bool after) { mark(self, list, c_cx, key, after, false); }

// list_reset, after the desktop Reset: the recording span begins (X2W_GPU_TIME_CLASSES).
void list_begin(Wrapper* self, ID3D12GraphicsCommandList* list)
{
    if (!on() || !classes() || !list) return;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    AcquireSRWLockExclusive(&lock);
    ListState& s = lists()[self];
    if (s.items.empty() && ensure(s, list)) {
        list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        s.open_list = 0;
        s.items.push_back({static_cast<u64>(type), c_list});
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
        if (s.open >= 0) {  // a begin without its end
            if (s.open == static_cast<int>(s.items.size()) - 1) s.items.pop_back();
            s.open = -1;
        }
        if (s.open_list >= 0) {
            list->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * s.open_list + 1));
            s.open_list = -1;
        }
        if (!s.items.empty()) {
            s.slot = s.has_pending[0] && !s.has_pending[1] ? 1 : 0;
            list->ResolveQueryData(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, static_cast<UINT>(2 * s.items.size()), s.readback[s.slot], 0);
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
        if (found != lists().end() && found->second.resolved && !found->second.items.empty()) marked.push_back(&found->second);
    }
    if (!marked.empty()) {
        QueueFence& f = fences()[queue];
        if (!f.fence && FAILED(device_of(the_device)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence)))) f.fence = nullptr;
        UINT64 frequency = 0;
        if (f.fence && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && SUCCEEDED(queue->Signal(f.fence, ++f.value))) {
            for (ListState* s : marked) {
                const int b = s->slot;
                if (s->has_pending[b]) InterlockedIncrement(&overwritten);
                s->pending[b] = {s->items, f.fence, f.value, frequency, b, s->type};
                s->has_pending[b] = true;
            }
        } else {
            InterlockedIncrement(&failures);
        }
    }
    harvest_locked();
    ReleaseSRWLockExclusive(&lock);
    // Test knob (off by default): X2W_GPU_TIME_REPORT_SUBMITS=<n> also writes the report every n submissions, for
    // fixtures that never present. It first waits (at most 5 s) for this queue's timing fence so the report holds
    // this submission's results; that wait is why the knob is for fixtures only.
    static const LONG every = [] {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_GPU_TIME_REPORT_SUBMITS", value, sizeof(value));
        return n && n < sizeof(value) ? static_cast<LONG>(std::atoi(value)) : 0L;
    }();
    static LONG submits;
    if (every > 0 && InterlockedIncrement(&submits) % every == 0) {
        AcquireSRWLockExclusive(&lock);
        const auto found = fences().find(queue);
        ID3D12Fence* fence = found != fences().end() ? found->second.fence : nullptr;
        const UINT64 value = fence ? found->second.value : 0;
        ReleaseSRWLockExclusive(&lock);
        if (fence && fence->GetCompletedValue() < value) {
            if (HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr)) {
                if (SUCCEEDED(fence->SetEventOnCompletion(value, done))) WaitForSingleObject(done, 5000);
                CloseHandle(done);
            }
        }
        report();
    }
}

// list_reset: a new recording.
void reset(Wrapper* self)
{
    if (!on()) return;
    AcquireSRWLockExclusive(&lock);
    harvest_locked();
    const auto found = lists().find(self);
    if (found != lists().end()) {
        found->second.items.clear();
        found->second.open = found->second.open_list = -1;
        found->second.resolved = false;
    }
    ReleaseSRWLockExclusive(&lock);
}

std::vector<std::pair<u64, KeyTime>> sorted(std::unordered_map<u64, KeyTime>& m)
{
    std::vector<std::pair<u64, KeyTime>> all(m.begin(), m.end());
    m.clear();
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.total_ms > b.second.total_ms; });
    return all;
}

// Every 300 frames.
void report()
{
    if (!on()) return;
    std::vector<std::pair<u64, KeyTime>> all[c_count];
    double untimed[4], span[4], cx_by_type[4];
    LONG64 span_count[4], cx_count_by_type[4];
    AcquireSRWLockExclusive(&lock);
    harvest_locked();
    for (int c = 0; c < c_count; ++c) all[c] = sorted(times(c));
    for (int t = 0; t < 4; ++t) {
        untimed[t] = untimed_ms[t], span[t] = span_ms[t], span_count[t] = spans[t], untimed_ms[t] = span_ms[t] = 0, spans[t] = 0;
        cx_by_type[t] = cx_ms[t], cx_count_by_type[t] = cx_calls[t], cx_ms[t] = 0, cx_calls[t] = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    double total = 0;
    LONG64 calls = 0;
    for (const auto& entry : all[c_cx]) total += entry.second.total_ms, calls += entry.second.calls;
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "x2w-cx gpu time (300 frames): %.1f ms in %lld timed dispatches of %zu keys; top:", total, calls,
                          all[c_cx].size());
    for (std::size_t i = 0; i < all[c_cx].size() && i < 10; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %016llx %.2f ms/%lld (max %.3f)", all[c_cx][i].first, all[c_cx][i].second.total_ms,
                           all[c_cx][i].second.calls, all[c_cx][i].second.max_ms);
    log("%s", line);
    log("x2w-cx gpu time counters: timed %ld, over %u per list %ld, results read %ld, overwritten before read %ld, failures %ld",
        InterlockedExchange(&timed, 0), cap(), InterlockedExchange(&over_cap, 0), InterlockedExchange(&harvested, 0),
        InterlockedExchange(&overwritten, 0), InterlockedExchange(&failures, 0));
    // LOCAL stall: which queue type the timed dispatches ran on (a compute-queue dispatch shares the GPU with the
    // direct queue's work, which its interval can absorb)
    log("x2w-cx gpu time by list type: direct %.1f ms/%lld, compute %.1f ms/%lld", cx_by_type[0], cx_count_by_type[0], cx_by_type[2],
        cx_count_by_type[2]);
    if (!classes()) return;
    n = std::snprintf(line, sizeof(line), "x2w-gpu time (300 frames) by class:");
    for (int c = 0; c < c_list; ++c) {
        double t = 0, over = 0;
        LONG64 k = 0;
        for (const auto& entry : all[c]) t += entry.second.total_ms, k += entry.second.calls, over += static_cast<double>(entry.second.over_1ms);
        n += std::snprintf(line + n, sizeof(line) - n, " %s %.1f ms/%lld (%.0f over 1 ms, %zu keys);", class_names[c], t, k, over, all[c].size());
    }
    static const char* const type_names[4] = {"direct", "bundle", "compute", "copy"};
    for (int t = 0; t < 4; ++t)
        if (span_count[t])
            n += std::snprintf(line + n, sizeof(line) - n, " %s lists %.1f ms/%lld spans, untimed inside %.1f ms;", type_names[t], span[t], span_count[t],
                               untimed[t]);
    log("%s", line);
    for (int c = 0; c < c_list; ++c) {
        if (all[c].empty()) continue;
        n = std::snprintf(line, sizeof(line), "x2w-gpu time %s top (ms/calls, max, over 1 ms):", class_names[c]);
        for (std::size_t i = 0; i < all[c].size() && i < 10; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, " %016llx %.2f/%lld (max %.3f, %lld)", all[c][i].first, all[c][i].second.total_ms,
                               all[c][i].second.calls, all[c][i].second.max_ms, all[c][i].second.over_1ms);
        log("%s", line);
    }
}
}  // namespace gpu_time
