// LOCAL (translator work): the register blobs and requests of every slot 57 graphics pipeline, and the
// patches of every slot 55 pipeline derived from one, written at creation next to the pipeline requests
// in X2W_SHADER_CACHE. Included inside d3d12_xs_device.cpp's anonymous namespace, after d3d12_draw.inl
// (pipeline_key, cache_path).
//
// On whenever X2W_SHADER_CACHE is set (run.ps1 -Render sets it). X2W_REGBLOB_OFF=1 turns it off;
// X2W_REGBLOB_REQUESTS_OFF=1 keeps the blob files but writes no .a/.b/.root at creation.
// X2W_REGBLOB_MAX_MB caps the bytes queued per run (default 512). State and patch records also wait for
// the writer at most 16 MiB at a time; first sets of a key never wait-drop (counted apart if capped).
//
// Slot 57 record, 0x160 bytes: [0] root signature, [1]/[2] state block A (packed register values),
// [3]/[4] block B (0x50-byte header + the vertex input layout; the file name .psregs is historic), then 4-word stage slots from [5]: code pointer, code bytes, metadata pointer, metadata
// bytes ([5]/[6] code a, [7]/[8], [9]/[10] code b, [11]/[12], [13]/[14] ...). Layout of the blocks:
// out/gamerun/translator_work/REG_LAYOUT.md.
//
// In the shader cache directory (per key, a few files; the directory is listed by other tools):
//   req_<key>.vsregs  [1]/[2]   req_<key>.psregs  [3]/[4]   req_<key>.r78  [7]/[8]   (raw bytes)
//   req_<key>.rec     the 0x160-byte record, written after the blob files (marker: the set is complete)
//   req_<key>.b, .root, .a   the request, as cached_translation writes it (kept if present; .a last)
// In <cache>\regblob\:
//   <key>.desc        0x300 bytes from the record start (runs past the record; pointers only)
//   <key>.p<N>        every other readable pointer/length pair at odd word N except 1, 3, 5, 7, 9
//                     (.p11 = [11]/[12], .p13 ...); code a/b are req_<key>.a/.b
//   <key>.states      every distinct content of [1], [3], [7], [11] seen for the key (the first one
//                     too), appended: records {u32 'X2WS', u32 bytes after this word, u32 state hash,
//                     u32 count, count x {u32 word, u32 bytes, bytes}}
//   g57_<ck>.{vsregs,psregs,r78,p<N>,desc,root,rec}   a record without both code blobs (no request
//                     key; mesh pipelines: code at .p13, metadata .p15); code pairs are .p5 / .p9 / .p13;
//                     <ck> hashes the root blob (.root) and every pair
//   d55.txt           one line per distinct (base key, patch set): "<key or g57_ck> <n> <h1> ... <hn>\n",
//                     record hashes in patch order
//   d55_<h>.bin       one 0x14c-byte patch record {u32 kind; desktop desc} per distinct hash <h>:
//                     kind 0 depth-stencil, 2 blend (1 assumed rasterizer); <h> ignores padding garbage
// Appended files are cut back to their last complete record or line before the first append of a run.
// Every other file goes through <name>.<pid>.<tid>.tmp and a rename. One worker thread does all file IO.

namespace regblob {
constexpr std::size_t record_bytes = 0x160, desc_bytes = 0x300;
constexpr int record_words = static_cast<int>(record_bytes / 8);
constexpr std::size_t blob_cap = 1u << 20;
constexpr std::uint32_t states_magic = 0x53573258;  // "X2WS"
constexpr std::size_t table_cap = 1u << 18;

struct Pair {
    int word;
    const unsigned char* data;
    std::size_t bytes;
};
struct Blob {
    int word;
    std::vector<unsigned char> bytes;
};
enum class Kind { set, generic_set, states, derived };
struct Job {
    Kind kind;
    u64 key;
    std::uint32_t hash = 0;                 // states: state hash
    std::vector<Blob> blobs;                // set: every pair except code; generic: every pair; states: 1, 3, 7, 11
    std::vector<unsigned char> record, desc, a, b, root;  // sets; derived: record = raw patches
    std::vector<std::uint32_t> patch_hashes;  // derived
    bool generic_base = false;              // derived: the base key is a g57 key
    std::size_t bytes() const
    {
        std::size_t n = record.size() + desc.size() + a.size() + b.size() + root.size();
        for (const Blob& blob : blobs) n += blob.bytes.size();
        return n;
    }
};

SRWLOCK queue_lock = SRWLOCK_INIT;
CONDITION_VARIABLE queue_ready = CONDITION_VARIABLE_INIT;
std::vector<Job>& queue() { static std::vector<Job> q; return q; }
LONG64 bytes_queued, bytes_pending;
LONG sets_written, generic_written, requests_written, states_appended, derived_lines, patch_files, failures;
LONG dropped_backlog, dropped_total, sets_dropped, table_full, queued_jobs;

// A pointer/length pair at odd word i: pointer 4-aligned in the user range, length 4..cap, readable.
bool pair_at(const u64* w, int i, Pair& out)
{
    const u64 p = w[i], bytes = w[i + 1];
    if (p < 0x10000 || p > 0x00007FFFFFFEFFFFull || (p & 3) || bytes < 4 || bytes > blob_cap) return false;
    if (readable_window(p, static_cast<std::size_t>(bytes)) < bytes) return false;
    out = {i, reinterpret_cast<const unsigned char*>(p), static_cast<std::size_t>(bytes)};
    return true;
}

std::vector<unsigned char> bytes_of(const void* data, std::size_t bytes)
{
    const auto* p = static_cast<const unsigned char*>(data);
    return std::vector<unsigned char>(p, p + bytes);
}

// <cache>\regblob\<name>
bool sub_path(wchar_t* out, const wchar_t* name)
{
    wchar_t dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"X2W_SHADER_CACHE", dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 64) return false;
    return swprintf(out, MAX_PATH, L"%s\\regblob\\%s", dir, name) > 0;
}

bool exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

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

// An appended file: read it whole (for its existing entries), cut it back to valid_end(bytes) once, then
// append. Only the worker thread calls these.
std::vector<unsigned char> read_all(const wchar_t* path)
{
    std::vector<unsigned char> out;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (LONGLONG{64} << 20)) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) || got != out.size()) out.clear();
    }
    CloseHandle(file);
    return out;
}
void cut_to(const wchar_t* path, std::size_t bytes)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(bytes);
    if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) SetEndOfFile(file);
    CloseHandle(file);
}
bool append(const wchar_t* path, const void* data, std::size_t bytes)
{
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, data, static_cast<DWORD>(bytes), &written, nullptr) && written == bytes;
    CloseHandle(file);
    return complete;
}

void put_u32(std::vector<unsigned char>& out, std::uint32_t v)
{
    out.insert(out.end(), reinterpret_cast<const unsigned char*>(&v), reinterpret_cast<const unsigned char*>(&v) + 4);
}

const wchar_t* blob_name(int word, wchar_t* out, std::size_t size)
{
    if (word == 1) return L"vsregs";
    if (word == 3) return L"psregs";
    if (word == 7) return L"r78";
    swprintf(out, size, L"p%d", word);
    return out;
}

void write_set(const Job& job)
{
    wchar_t path[MAX_PATH], name[64], part[16];
    bool ok = true;
    const bool generic = job.kind == Kind::generic_set;
    if (generic) {
        swprintf(name, std::size(name), L"g57_%016llx.rec", job.key);
        if (!sub_path(path, name) || exists(path)) return;
    } else if (!cache_path(path, L"req_", job.key, L".rec")) {
        return;
    }
    const bool blobs = generic || !exists(path);
    if (blobs) {
        swprintf(name, std::size(name), L"%ls%016llx.desc", generic ? L"g57_" : L"", job.key);
        ok = sub_path(path, name) && write_whole(path, job.desc.data(), job.desc.size()) && ok;
        for (const Blob& blob : job.blobs) {
            const wchar_t* ext = blob_name(blob.word, part, std::size(part));
            const bool in_cache = !generic && (blob.word == 1 || blob.word == 3 || blob.word == 7);
            if (in_cache) {
                swprintf(name, std::size(name), L".%ls", ext);
                ok = cache_path(path, L"req_", job.key, name) && write_whole(path, blob.bytes.data(), blob.bytes.size()) && ok;
            } else {
                swprintf(name, std::size(name), L"%ls%016llx.%ls", generic ? L"g57_" : L"", job.key, ext);
                ok = sub_path(path, name) && write_whole(path, blob.bytes.data(), blob.bytes.size()) && ok;
            }
        }
        if (generic) {
            // The root blob hashed into ck: mesh pipelines (code at [13]) need it to be translated.
            swprintf(name, std::size(name), L"g57_%016llx.root", job.key);
            ok = sub_path(path, name) && write_whole(path, job.root.data(), job.root.size()) && ok;
            swprintf(name, std::size(name), L"g57_%016llx.rec", job.key);
            ok = ok && sub_path(path, name) && write_whole(path, job.record.data(), job.record.size());
        } else {
            ok = ok && cache_path(path, L"req_", job.key, L".rec") && write_whole(path, job.record.data(), job.record.size());
        }
        if (ok) InterlockedIncrement(generic ? &generic_written : &sets_written);
    }
    // The request, in cached_translation's order (.a last); never replaced. Not after a failed set.
    if (!generic && ok && !job.a.empty()) {
        const struct { const wchar_t* ext; const std::vector<unsigned char>* bytes; } parts[] = {
            {L".b", &job.b}, {L".root", &job.root}, {L".a", &job.a}};
        for (const auto& p : parts) {
            if (!cache_path(path, L"req_", job.key, p.ext)) { ok = false; break; }
            if (exists(path)) continue;
            if (!write_whole(path, p.bytes->data(), p.bytes->size())) { ok = false; break; }
            if (p.ext[1] == L'a') InterlockedIncrement(&requests_written);
        }
    }
    if (!ok) InterlockedIncrement(&failures);
    static LONG logged;
    if (blobs && ok && first_calls(logged, 8))
        log("x2w-regblob wrote %ls%016llx (%zu blobs)", generic ? L"regblob\\g57_" : L"req_", job.key, job.blobs.size());
}

// Valid prefix of a .states file; collects the state hashes in it.
std::size_t states_valid(const std::vector<unsigned char>& f, std::unordered_set<std::uint32_t>& seen)
{
    std::size_t at = 0;
    while (at + 16 <= f.size()) {
        std::uint32_t magic, bytes, hash;
        std::memcpy(&magic, &f[at], 4);
        std::memcpy(&bytes, &f[at + 4], 4);
        std::memcpy(&hash, &f[at + 8], 4);
        if (magic != states_magic || bytes < 8 || bytes > f.size() - at - 8) break;
        seen.insert(hash);
        at += 8 + bytes;
    }
    return at;
}

void append_states(const Job& job)
{
    static std::unordered_map<u64, std::unordered_set<std::uint32_t>> known;  // worker thread only
    wchar_t path[MAX_PATH], name[64];
    swprintf(name, std::size(name), L"%016llx.states", job.key);
    if (!sub_path(path, name)) return;
    auto found = known.find(job.key);
    if (found == known.end()) {
        found = known.emplace(job.key, std::unordered_set<std::uint32_t>{}).first;
        const std::vector<unsigned char> f = read_all(path);
        const std::size_t valid = states_valid(f, found->second);
        if (valid < f.size()) cut_to(path, valid);
    }
    if (!found->second.insert(job.hash).second) return;
    std::vector<unsigned char> out;
    put_u32(out, states_magic);
    put_u32(out, 0);
    put_u32(out, job.hash);
    put_u32(out, static_cast<std::uint32_t>(job.blobs.size()));
    for (const Blob& blob : job.blobs) {
        put_u32(out, static_cast<std::uint32_t>(blob.word));
        put_u32(out, static_cast<std::uint32_t>(blob.bytes.size()));
        out.insert(out.end(), blob.bytes.begin(), blob.bytes.end());
    }
    const std::uint32_t after = static_cast<std::uint32_t>(out.size() - 8);
    std::memcpy(&out[4], &after, 4);
    if (append(path, out.data(), out.size())) InterlockedIncrement(&states_appended);
    else InterlockedIncrement(&failures);
}

void append_derived(const Job& job)
{
    static bool loaded = false;  // worker thread only
    static std::unordered_set<std::string> lines;
    static std::unordered_set<std::uint32_t> records;
    wchar_t path[MAX_PATH], name[64];
    if (!sub_path(path, L"d55.txt")) return;
    if (!loaded) {
        loaded = true;
        const std::vector<unsigned char> f = read_all(path);
        std::size_t start = 0, valid = 0;
        for (std::size_t i = 0; i < f.size(); ++i)
            if (f[i] == '\n') {
                lines.emplace(reinterpret_cast<const char*>(f.data()) + start, i - start);
                start = valid = i + 1;
            }
        if (valid < f.size()) cut_to(path, valid);
    }
    char line[64 + 9 * 64];
    int n = std::snprintf(line, sizeof(line), "%s%016llx %zu", job.generic_base ? "g57_" : "", job.key, job.patch_hashes.size());
    for (std::size_t i = 0; i < job.patch_hashes.size() && n < static_cast<int>(sizeof(line)) - 12; ++i)
        n += std::snprintf(line + n, sizeof(line) - n, " %08x", job.patch_hashes[i]);
    if (!lines.emplace(line, static_cast<std::size_t>(n)).second) return;
    constexpr std::size_t record = x2w_derived_blend::record_size;
    bool ok = true;
    for (std::size_t i = 0; i < job.patch_hashes.size() && (i + 1) * record <= job.record.size(); ++i) {
        if (records.count(job.patch_hashes[i])) continue;
        swprintf(name, std::size(name), L"d55_%08x.bin", job.patch_hashes[i]);
        wchar_t file[MAX_PATH];
        if (!sub_path(file, name)) { ok = false; break; }
        if (!exists(file)) {
            if (!write_whole(file, job.record.data() + i * record, record)) { ok = false; break; }
            InterlockedIncrement(&patch_files);
        }
        records.insert(job.patch_hashes[i]);
    }
    line[n++] = '\n';
    if (ok && append(path, line, static_cast<std::size_t>(n))) InterlockedIncrement(&derived_lines);
    else InterlockedIncrement(&failures);
}

DWORD __stdcall worker(void*)
{
    wchar_t dir[MAX_PATH];
    if (sub_path(dir, L"")) CreateDirectoryW(dir, nullptr);
    std::vector<Job> batch;
    for (;;) {
        AcquireSRWLockExclusive(&queue_lock);
        while (queue().empty()) SleepConditionVariableSRW(&queue_ready, &queue_lock, INFINITE, 0);
        batch.swap(queue());
        ReleaseSRWLockExclusive(&queue_lock);
        for (const Job& job : batch) {
            switch (job.kind) {
            case Kind::set: case Kind::generic_set: write_set(job); break;
            case Kind::states: append_states(job); break;
            case Kind::derived: append_derived(job); break;
            }
            InterlockedAdd64(&bytes_pending, -static_cast<LONG64>(job.bytes()));
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
    const bool set = job.kind == Kind::set || job.kind == Kind::generic_set;
    const LONG64 bytes = static_cast<LONG64>(job.bytes());
    // Only state and patch records drop while the writer is behind; a key's first set waits its turn.
    if (InterlockedAdd64(&bytes_pending, bytes) > (LONG64{16} << 20) && !set) {
        InterlockedAdd64(&bytes_pending, -bytes);
        InterlockedIncrement(&dropped_backlog);
        return;
    }
    if (InterlockedAdd64(&bytes_queued, bytes) > cap) {
        InterlockedAdd64(&bytes_queued, -bytes);
        InterlockedAdd64(&bytes_pending, -bytes);
        InterlockedIncrement(&dropped_total);
        if (set) InterlockedIncrement(&sets_dropped);
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

SRWLOCK key_lock = SRWLOCK_INIT;
// Slot-57 pipeline wrapper -> its key and whether it is a g57 key, for the derived pipelines built on it.
std::unordered_map<const Wrapper*, std::pair<u64, bool>>& keys_of() { static std::unordered_map<const Wrapper*, std::pair<u64, bool>> m; return m; }

// Device slot 57, after note_pipeline. Returns the key (request key, or the g57 content key).
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
    bool variant = false;
    if (keyed && states.size() < table_cap) variant = states.insert(state_id).second;
    else if (keyed && !states.count(state_id)) InterlockedIncrement(&table_full);
    ReleaseSRWLockExclusive(&lock);
    AcquireSRWLockExclusive(&key_lock);
    keys_of()[self] = {key, !keyed};
    ReleaseSRWLockExclusive(&key_lock);
    if (plain) {
        Job job{keyed ? Kind::set : Kind::generic_set, key};
        job.record = bytes_of(desc, record_bytes);
        job.desc = bytes_of(desc, readable_window(reinterpret_cast<u64>(desc), desc_bytes));
        for (int i = 1; i + 1 < record_words; i += 2) {
            Pair p;
            if ((keyed && (i == 5 || i == 9)) || !pair_at(w, i, p)) continue;
            job.blobs.push_back({i, bytes_of(p.data, p.bytes)});
        }
        static const bool requests = !x2w_local::env_on("X2W_REGBLOB_REQUESTS_OFF");
        if (keyed && requests) {
            job.a = bytes_of(a, a_bytes);
            job.b = bytes_of(b, b_bytes);
            job.root = root_blob;
        }
        if (!keyed) job.root = root_blob;  // regblob\g57_<ck>.root
        push(std::move(job));
    }
    if (variant) {
        Job job{Kind::states, key};
        job.hash = static_cast<std::uint32_t>(state ^ (state >> 32));
        for (int i : {1, 3, 7, 11}) {
            Pair p;
            if (pair_at(w, i, p)) job.blobs.push_back({i, bytes_of(p.data, p.bytes)});
        }
        push(std::move(job));
    }
    return key;
}

// Hash of one patch record without the bytes the title leaves as garbage: the padding after each blend
// target's write mask, and everything after the desc of a depth-stencil or rasterizer record.
std::uint32_t patch_hash(const unsigned char* record)
{
    constexpr std::size_t size = x2w_derived_blend::record_size;
    unsigned char canonical[size];
    std::memcpy(canonical, record, size);
    std::uint32_t kind;
    std::memcpy(&kind, canonical, sizeof(kind));
    unsigned char* body = canonical + sizeof(kind);
    if (kind == 2) {
        for (int t = 0; t < 8; ++t)
            std::memset(body + offsetof(D3D12_BLEND_DESC, RenderTarget) + t * sizeof(D3D12_RENDER_TARGET_BLEND_DESC) +
                        offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) + 1, 0, 3);
    } else if (kind == 0 || kind == 1) {
        const std::size_t used = kind == 0 ? sizeof(D3D12_DEPTH_STENCIL_DESC) : sizeof(D3D12_RASTERIZER_DESC);
        std::memset(body + used, 0, size - sizeof(kind) - used);
        if (kind == 0) std::memset(body + offsetof(D3D12_DEPTH_STENCIL_DESC, StencilWriteMask) + 1, 0, 2);
    }
    static_assert(sizeof(D3D12_RENDER_TARGET_BLEND_DESC) == offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) + 4);
    static_assert(offsetof(D3D12_DEPTH_STENCIL_DESC, FrontFace) == offsetof(D3D12_DEPTH_STENCIL_DESC, StencilWriteMask) + 3);
    static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC) + sizeof(std::uint32_t) <= size);
    static_assert(sizeof(D3D12_RASTERIZER_DESC) + sizeof(std::uint32_t) <= size);
    const u64 h = fnv_bytes(canonical, size);
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

// Device slot 55 (CreateDerivedPipelineStateX), after note_pipeline: one d55.txt line per distinct
// (base key, patch set). The base is found through the slot-55 base chain (depth 8), as translation_for does.
void dump55(const void* base, const void* patches, std::size_t bytes)
{
    static const bool on = !x2w_local::env_on("X2W_REGBLOB_OFF");
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::unordered_set<u64> seen;
    constexpr std::size_t record = x2w_derived_blend::record_size;
    wchar_t probe[MAX_PATH];
    if (!on || !patches || !bytes || bytes > 64 * record || bytes % record || !cache_path(probe, L"req_", 0, L"")) return;
    const Wrapper* w = as_wrapper(base);
    std::pair<u64, bool> found{0, false};
    bool have = false;
    for (int depth = 0; w && depth < 8 && !have; ++depth) {
        AcquireSRWLockShared(&key_lock);
        const auto it = keys_of().find(w);
        if (it != keys_of().end()) found = it->second, have = true;
        ReleaseSRWLockShared(&key_lock);
        if (have) break;
        const PipelineInfo* info = pipeline_info(w);
        w = info && info->path == 55 ? info->base : nullptr;
    }
    if (!have) return;
    Job job{Kind::derived, found.first};
    job.generic_base = found.second;
    const auto* p = static_cast<const unsigned char*>(patches);
    u64 set = fnv_bytes(&found, sizeof(found.first));
    for (std::size_t at = 0; at < bytes; at += record) {
        job.patch_hashes.push_back(patch_hash(p + at));
        set = fnv_bytes(&job.patch_hashes.back(), 4, set);
    }
    AcquireSRWLockExclusive(&lock);
    bool fresh = false;
    if (seen.size() < table_cap) fresh = seen.insert(set).second;
    else if (!seen.count(set)) InterlockedIncrement(&table_full);
    ReleaseSRWLockExclusive(&lock);
    if (!fresh) return;
    job.record = bytes_of(patches, bytes);
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
    log("x2w-regblob totals: queued %ld (%lld KiB), pending %zu (%lld KiB), sets %ld, unclassified %ld, requests %ld, "
        "states %ld, derived lines %ld (patch files %ld), failed %ld; dropped: writer behind %ld, over %s cap %ld "
        "(first sets %ld), table full %ld", queued_jobs, bytes_queued >> 10, pending, bytes_pending >> 10, sets_written,
        generic_written, requests_written, states_appended, derived_lines, patch_files, failures, dropped_backlog,
        "X2W_REGBLOB_MAX_MB", dropped_total, sets_dropped, table_full);
}
}  // namespace regblob
