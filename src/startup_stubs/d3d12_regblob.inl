// LOCAL (translator work): the register blobs and requests of every slot 57 graphics pipeline, written
// at creation as raw bytes next to the pipeline requests in X2W_SHADER_CACHE. Included inside
// d3d12_xs_device.cpp's anonymous namespace, after d3d12_draw.inl (pipeline_key, cache_path).
//
// On whenever X2W_SHADER_CACHE is set (run.ps1 -Render sets it). X2W_REGBLOB_OFF=1 turns it off;
// X2W_REGBLOB_REQUESTS_OFF=1 keeps the blob files but writes no .a/.b/.root at creation.
// X2W_REGBLOB_MAX_MB caps the bytes queued per run (default 512); at most 64 MiB wait unwritten at a
// time. A set over either cap is dropped (counted) and tried again next run.
//
// Slot 57 record, 0x160 bytes: [0] root signature, [1]/[2] state block A, [3]/[4] state block B
// (0xbc bytes, the pixel-stage blob), then pointer/length pairs at odd words: [5]/[6] code a,
// [7]/[8], [9]/[10] code b, [11]/[12], ... (seen in X2W_D3D12_DUMP_PSO logs).
//
// Files per key (raw bytes, no header; a pair that is absent or unreadable gets no file):
//   req_<key>.vsregs  [1]/[2]      req_<key>.psregs  [3]/[4]      req_<key>.r78  [7]/[8]
//   req_<key>.p<N>    every other readable pair at odd word N (decimal, no padding), code included:
//                     .p5 = .a and .p9 = .b for a keyed record, .p11 [11]/[12], .p13 ... (a third stage)
//   req_<key>.regs    [1]/[2] then [3]/[4], each as u32 length + bytes (the req_c67_*.regs convention)
//   req_<key>.meta    [7]/[8] then [11]/[12], each as u32 length + bytes
//   req_<key>.desc    0x300 bytes from the record (what note_pipeline copies; may run past the record)
//   req_<key>.rec     the 0x160-byte record (pointers as the title passed them); last of the blob files
//   req_<key>.b, .root, .a   the pipeline request, as cached_translation writes it (kept if present)
//   req_<key>.d55_<8 hex>    slot 55 derived from this key: its raw patch records, once per distinct set
//                            (hash ignores the blend write-mask padding and the rasterizer record's tail)
// <key> is the request key (pipeline_key over [5]/[6], [9]/[10] and the root blob), unchanged, so these
// pipelines get requests also when they are only ever bound at ExecuteIndirect (list slot 61).
// Every distinct content of the blobs [1], [3], [7], [11] of a key (the first one too) is also written
// as req_<key>.<state 8 hex>.{vsregs,psregs,r78,p<N>,rec} (no code): pipelines sharing shaders but not state.
// A record without both code blobs has no request key (other stage layouts, mesh candidates): it is
// written as req_g57_<content key>.* with the same names, the key hashes
// the root blob and every pair's word index and bytes.
//
// The creating thread hashes, copies the new blobs and queues them; one worker thread checks for the
// .rec (once per key and per key and state per run) and writes each file through
// <name>.<pid>.<tid>.tmp and a rename. A killed run leaves no partial file under a final name.

namespace regblob {
constexpr std::size_t record_bytes = 0x160, desc_bytes = 0x300;
constexpr int record_words = static_cast<int>(record_bytes / 8);
constexpr std::size_t blob_cap = 1u << 20;

struct Pair {
    int word;
    const unsigned char* data;
    std::size_t bytes;
};
struct File {
    std::wstring ext;
    std::vector<unsigned char> bytes;
    bool keep_existing = false;  // request files: never replaced
};
struct Job {
    const wchar_t* prefix;
    u64 key;
    std::wstring marker;      // skip the files before it when it exists
    std::vector<File> files;  // marker last of the blob files, then request files
};

SRWLOCK queue_lock = SRWLOCK_INIT;
CONDITION_VARIABLE queue_ready = CONDITION_VARIABLE_INIT;
std::vector<Job>& queue() { static std::vector<Job> q; return q; }
LONG64 bytes_queued, bytes_pending;  // queued this run (capped), and not yet written (capped at 64 MiB)
LONG sets_written, variants_written, derived_written, generic_written, requests_written, failures, dropped, queued_jobs;

// A pointer/length pair at odd word i: pointer 4-aligned in the user range, length 4..cap, readable.
bool pair_at(const u64* w, int i, Pair& out)
{
    const u64 p = w[i], bytes = w[i + 1];
    if (p < 0x10000 || p > 0x00007FFFFFFEFFFFull || (p & 3) || bytes < 4 || bytes > blob_cap) return false;
    if (readable_window(p, static_cast<std::size_t>(bytes)) < bytes) return false;
    out = {i, reinterpret_cast<const unsigned char*>(p), static_cast<std::size_t>(bytes)};
    return true;
}

bool write_whole(const wchar_t* path, const void* data, std::size_t bytes)
{
    wchar_t temporary[MAX_PATH];
    if (swprintf(temporary, MAX_PATH, L"%s.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId()) < 0) return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, data, static_cast<DWORD>(bytes), &written, nullptr) && written == bytes;
    CloseHandle(file);
    if (complete && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temporary);
    return false;
}

void run_job(const Job& job)
{
    wchar_t path[MAX_PATH];
    const bool have_marker = cache_path(path, job.prefix, job.key, job.marker.c_str()) &&
                             GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    bool blobs = !have_marker, ok = true, request = false;
    for (const File& f : job.files) {
        if (!f.keep_existing && !blobs) continue;
        if (!cache_path(path, job.prefix, job.key, f.ext.c_str())) { ok = false; break; }
        if (f.keep_existing && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
        if (!ok && (f.ext == job.marker || f.keep_existing)) break;  // keep the order: no marker or .a after a failed write
        const bool done = write_whole(path, f.bytes.data(), f.bytes.size());
        ok = ok && done;
        if (f.keep_existing && done && f.ext == L".a") request = true;
    }
    if (!ok) InterlockedIncrement(&failures);
    if (request) InterlockedIncrement(&requests_written);
    const std::size_t m = job.marker.size();
    const bool set = m >= 4 && job.marker.compare(m - 4, 4, L".rec") == 0;
    if (blobs && ok && !set) InterlockedIncrement(&derived_written);
    if (blobs && ok && set) {
        const bool variant = m > 4;  // ".<8 hex>.rec"
        InterlockedIncrement(variant ? &variants_written : job.prefix[4] == L'g' ? &generic_written : &sets_written);
        static LONG logged;
        if (!variant && first_calls(logged, 8))
            log("x2w-regblob wrote %ls%016llx (%zu files)", job.prefix, job.key, job.files.size());
    }
}

DWORD __stdcall worker(void*)
{
    std::vector<Job> batch;
    for (;;) {
        AcquireSRWLockExclusive(&queue_lock);
        while (queue().empty()) SleepConditionVariableSRW(&queue_ready, &queue_lock, INFINITE, 0);
        batch.swap(queue());
        ReleaseSRWLockExclusive(&queue_lock);
        for (const Job& job : batch) {
            run_job(job);
            LONG64 bytes = 0;
            for (const File& f : job.files) bytes += static_cast<LONG64>(f.bytes.size());
            InterlockedAdd64(&bytes_pending, -bytes);
        }
        batch.clear();
    }
}

void push(Job&& job)
{
    static const LONG64 cap = [] {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_REGBLOB_MAX_MB", value, sizeof(value));
        const long mb = n && n < sizeof(value) ? std::strtol(value, nullptr, 10) : 512;
        return LONG64{mb > 0 ? mb : 512} << 20;
    }();
    LONG64 bytes = 0;
    for (const File& f : job.files) bytes += static_cast<LONG64>(f.bytes.size());
    if (InterlockedAdd64(&bytes_pending, bytes) > (LONG64{64} << 20)) {
        InterlockedAdd64(&bytes_pending, -bytes);
        InterlockedIncrement(&dropped);
        return;
    }
    if (InterlockedAdd64(&bytes_queued, bytes) > cap) {
        InterlockedAdd64(&bytes_queued, -bytes);
        InterlockedAdd64(&bytes_pending, -bytes);
        InterlockedIncrement(&dropped);
        return;
    }
    static const bool started = [] {
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        return thread != nullptr;
    }();
    if (!started) {
        InterlockedAdd64(&bytes_pending, -bytes);
        return;
    }
    InterlockedIncrement(&queued_jobs);
    AcquireSRWLockExclusive(&queue_lock);
    queue().push_back(std::move(job));
    ReleaseSRWLockExclusive(&queue_lock);
    WakeConditionVariable(&queue_ready);
}

std::vector<unsigned char> bytes_of(const void* data, std::size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(data);
    return std::vector<unsigned char>(p, p + bytes);
}

// u32 length + bytes for each of the pairs at words i and j (length 0 when absent).
std::vector<unsigned char> length_prefixed(const u64* w, int i, int j)
{
    std::vector<unsigned char> out;
    for (int k : {i, j}) {
        Pair p{};
        const std::uint32_t n = pair_at(w, k, p) && p.bytes <= 0x10000 ? static_cast<std::uint32_t>(p.bytes) : 0u;
        out.insert(out.end(), reinterpret_cast<const unsigned char*>(&n), reinterpret_cast<const unsigned char*>(&n) + 4);
        if (n) out.insert(out.end(), p.data, p.data + n);
    }
    return out;
}

// The blob files of one set, .rec last. code: also the code pairs [5] and [9] (as .p5, .p9).
void add_blobs(std::vector<File>& files, const std::wstring& infix, const u64* w, const void* record, bool code, bool whole)
{
    for (int i = 1; i + 1 < record_words; i += 2) {
        Pair p;
        if ((!code && (i == 5 || i == 9)) || !pair_at(w, i, p)) continue;
        wchar_t ext[32];
        if (i == 1) swprintf(ext, std::size(ext), L"%ls.vsregs", infix.c_str());
        else if (i == 3) swprintf(ext, std::size(ext), L"%ls.psregs", infix.c_str());
        else if (i == 7) swprintf(ext, std::size(ext), L"%ls.r78", infix.c_str());
        else swprintf(ext, std::size(ext), L"%ls.p%d", infix.c_str(), i);
        files.push_back({ext, bytes_of(p.data, p.bytes)});
    }
    if (whole) {
        files.push_back({L".regs", length_prefixed(w, 1, 3)});
        files.push_back({L".meta", length_prefixed(w, 7, 11)});
        const std::size_t have = readable_window(reinterpret_cast<u64>(record), desc_bytes);
        files.push_back({L".desc", bytes_of(record, have)});
    }
    files.push_back({infix + L".rec", bytes_of(record, record_bytes)});
}

SRWLOCK key_lock = SRWLOCK_INIT;
// Slot-57 pipeline wrapper -> its key and prefix, for the derived pipelines built on it.
std::unordered_map<const Wrapper*, std::pair<u64, const wchar_t*>>& keys_of() { static std::unordered_map<const Wrapper*, std::pair<u64, const wchar_t*>> m; return m; }

// Device slot 57, after note_pipeline. Returns the key (request key, or the req_g57_ content key).
u64 dump57(const void* desc, const Wrapper* self)
{
    static const bool on = !x2w_local::env_on("X2W_REGBLOB_OFF");
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_set<u64> keys, states;  // handled this run
    wchar_t probe[MAX_PATH];
    if (!on || !desc || !cache_path(probe, L"req_", 0, L"") ||
        readable_window(reinterpret_cast<u64>(desc), record_bytes) < record_bytes) return 0;
    u64 w[record_words];
    std::memcpy(w, desc, sizeof(w));
    std::vector<unsigned char> root_blob;
    if (Wrapper* root = as_wrapper(reinterpret_cast<const void*>(w[0]))) {
        AcquireSRWLockShared(&note_lock);
        const auto found = root_blobs().find(root);
        if (found != root_blobs().end()) root_blob = found->second;
        ReleaseSRWLockShared(&note_lock);
    }
    // The same conditions as translation_for, so the key is the request key.
    const auto* a = reinterpret_cast<const void*>(w[5]);
    const auto* b = reinterpret_cast<const void*>(w[9]);
    const std::size_t a_bytes = static_cast<std::size_t>(w[6]), b_bytes = static_cast<std::size_t>(w[10]);
    const bool keyed = !root_blob.empty() && a_bytes && b_bytes && a_bytes <= (1u << 20) && b_bytes <= (1u << 20) &&
                       x2w_local::readable(a, a_bytes) && x2w_local::readable(b, b_bytes);
    u64 key = 0, state = 1469598103934665603ull;
    if (keyed) {
        key = pipeline_key(a, a_bytes, b, b_bytes, root_blob);
        for (int i : {1, 3, 7, 11}) {
            Pair p;
            if (!pair_at(w, i, p)) continue;
            state = fnv_bytes(&i, sizeof(i), state);
            state = fnv_bytes(p.data, p.bytes, state);
        }
    } else {
        constexpr char seed[] = "x2w-regblob-g57-1";
        key = fnv_bytes(root_blob.data(), root_blob.size(), fnv_bytes(seed, sizeof(seed) - 1));
        int pairs = 0;
        for (int i = 1; i + 1 < record_words; i += 2) {
            Pair p;
            if (!pair_at(w, i, p)) continue;
            ++pairs;
            key = fnv_bytes(&i, sizeof(i), key);
            key = fnv_bytes(p.data, p.bytes, key);
        }
        if (!pairs) return 0;
    }
    const u64 state_id = fnv_bytes(&state, sizeof(state), key);
    AcquireSRWLockExclusive(&lock);
    const bool plain = keys.insert(keyed ? key : ~key).second;
    const bool variant = keyed && states.size() < 16384 && states.insert(state_id).second;
    ReleaseSRWLockExclusive(&lock);
    const wchar_t* prefix = keyed ? L"req_" : L"req_g57_";
    AcquireSRWLockExclusive(&key_lock);
    keys_of()[self] = {key, prefix};
    ReleaseSRWLockExclusive(&key_lock);
    if (plain) {
        Job job{prefix, key, L".rec", {}};
        add_blobs(job.files, L"", w, desc, true, true);
        static const bool requests = !x2w_local::env_on("X2W_REGBLOB_REQUESTS_OFF");
        if (keyed && requests) {  // the request, in cached_translation's order (.a last)
            job.files.push_back({L".b", bytes_of(b, b_bytes), true});
            job.files.push_back({L".root", root_blob, true});
            job.files.push_back({L".a", bytes_of(a, a_bytes), true});
        }
        push(std::move(job));
    }
    if (variant) {
        wchar_t infix[16];
        swprintf(infix, std::size(infix), L".%08x", static_cast<unsigned>(state ^ (state >> 32)));
        Job job{prefix, key, std::wstring(infix) + L".rec", {}};
        add_blobs(job.files, infix, w, desc, false, false);
        push(std::move(job));
    }
    return key;
}

// Device slot 55 (CreateDerivedPipelineStateX), after note_pipeline: the patch records ({u32 kind;
// desktop desc}, x2w_derived_blend::record_size each; 1 rasterizer, 2 blend) as
// <prefix><base key>.d55_<8 hex hash of the patch bytes>, once per distinct patch set and base key.
// The base is found through the slot-55 base chain (depth 8), as translation_for does.
void dump55(const void* base, const void* patches, std::size_t bytes)
{
    static const bool on = !x2w_local::env_on("X2W_REGBLOB_OFF");
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_set<u64> seen;
    wchar_t probe[MAX_PATH];
    if (!on || !patches || !bytes || bytes > 0x10000 || !cache_path(probe, L"req_", 0, L"")) return;
    const Wrapper* w = as_wrapper(base);
    std::pair<u64, const wchar_t*> found{0, nullptr};
    for (int depth = 0; w && depth < 8; ++depth) {
        AcquireSRWLockShared(&key_lock);
        const auto it = keys_of().find(w);
        if (it != keys_of().end()) found = it->second;
        ReleaseSRWLockShared(&key_lock);
        if (found.second) break;
        const PipelineInfo* info = pipeline_info(w);
        w = info && info->path == 55 ? info->base : nullptr;
    }
    if (!found.second) return;
    // Hash a canonical copy: the title leaves garbage in the padding after each write mask (run84), and a
    // rasterizer record uses only sizeof(D3D12_RASTERIZER_DESC) of its 0x148 bytes. The raw bytes are written.
    std::vector<unsigned char> canonical = bytes_of(patches, bytes);
    constexpr std::size_t record = x2w_derived_blend::record_size;
    for (std::size_t at = 0; at + record <= canonical.size(); at += record) {
        std::uint32_t kind;
        std::memcpy(&kind, canonical.data() + at, sizeof(kind));
        unsigned char* body = canonical.data() + at + sizeof(kind);
        if (kind == 2) {
            for (int t = 0; t < 8; ++t)
                std::memset(body + offsetof(D3D12_BLEND_DESC, RenderTarget) + t * sizeof(D3D12_RENDER_TARGET_BLEND_DESC) +
                            offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) + 1, 0, 3);
        } else if (kind == 1) {
            std::memset(body + sizeof(D3D12_RASTERIZER_DESC), 0, record - sizeof(kind) - sizeof(D3D12_RASTERIZER_DESC));
        }
    }
    static_assert(sizeof(D3D12_RENDER_TARGET_BLEND_DESC) == offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) + 4);
    static_assert(sizeof(D3D12_RASTERIZER_DESC) + sizeof(std::uint32_t) <= x2w_derived_blend::record_size);
    const u64 hash = fnv_bytes(canonical.data(), canonical.size());
    AcquireSRWLockExclusive(&lock);
    const bool fresh = seen.size() < 16384 && seen.insert(fnv_bytes(&hash, sizeof(hash), found.first)).second;
    ReleaseSRWLockExclusive(&lock);
    if (!fresh) return;
    wchar_t ext[32];
    swprintf(ext, std::size(ext), L".d55_%08x", static_cast<unsigned>(hash ^ (hash >> 32)));
    Job job{found.second, found.first, ext, {}};
    job.files.push_back({ext, bytes_of(patches, bytes)});
    push(std::move(job));
}

// Every 300 frames in render mode (frame heartbeat).
void report()
{
    static const bool on = !x2w_local::env_on("X2W_REGBLOB_OFF");
    if (!on || !queued_jobs) return;
    AcquireSRWLockShared(&queue_lock);
    const std::size_t pending = queue().size();
    ReleaseSRWLockShared(&queue_lock);
    log("x2w-regblob totals: queued %ld (%lld KiB), pending %zu, sets %ld, variants %ld, without key %ld, requests %ld, "
        "derived patch sets %ld, failed %ld, dropped over cap %ld", queued_jobs, bytes_queued >> 10, pending, sets_written,
        variants_written, generic_written, requests_written, derived_written, failures, dropped);
}
}  // namespace regblob
