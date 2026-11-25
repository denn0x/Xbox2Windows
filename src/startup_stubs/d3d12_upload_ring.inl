// LOCAL memcut (out/gamerun/memcut_work/NOTES.md, board 234000): the upload ring for transient copies of title
// memory (fetch tables, vertices, constants, index buffers, texture uploads, indirect markers).
//
// The old ring was one 256 MiB upload buffer whose cursor wrapped with no fence check: inside one frame (runs
// 94-98: 381-602 MiB a frame; run 99: 6,169 MiB) later copies overwrote bytes that recorded draws had not read
// yet. Now (default, X2W_UPLOAD_RING=chain) the ring is a chain of upload chunks (X2W_UPLOAD_RING_CHUNK_MIB,
// default 64). A full chunk is reused only when
//   1. no command list whose current recording overlapped the chunk's use is still open, or closed and not yet
//      executed (Reset / Close / Execute are numbered on one sequence), and then
//   2. every queue's submit fence, snapshotted at that point (note_submits signals one after each execute), has
//      passed.
// Otherwise a new chunk is made, up to X2W_UPLOAD_RING_CAP_MIB (default 2048); past the cap the copy is refused
// (counted, logged) and the caller treats it as a failed copy. Never a silent wrap. Free chunks beyond 4 are
// released. A cached frame copy (frame_copy) is reused only while its chunk has not been recycled (generation).
// Known limit: a recording executed a second time without a Reset reads its chunk only until the chunk is
// recycled (counted: "re-executed recordings").
//
// X2W_UPLOAD_RING=legacy: today's ring (one 256 MiB buffer, wraps regardless), tracked as 4 segments with the same
// rules, so the report says how many bytes were written over data the GPU had not read yet. For one run.
// Fixtures only: X2W_UPLOAD_RING_TEST_KIB=<n> makes the legacy ring / a chain chunk n KiB.
namespace {
struct RingTag {
    UINT chunk = ~0u;
    u64 generation = 0;
};
struct RingChunk {
    ID3D12Resource* buffer = nullptr;
    unsigned char* mapped = nullptr;
    u64 base = 0;  // offset of this chunk inside buffer (legacy segments share one buffer)
    u64 bytes = 0, used = 0;
    u64 first_use = 0, last_use = 0;  // sequence numbers
    u64 generation = 1;
    enum State { free_, in_use, fenced } state = free_;
    bool unsafe = false;  // legacy: reused before it was free
    std::vector<std::pair<ID3D12Fence*, UINT64>> waits;
    LONG idle_since = 0;
};
struct RingList {
    u64 reset = 0, close = 0, executed = 0;
};
struct UploadRing {
    SRWLOCK lock = SRWLOCK_INIT;
    bool ready = false, legacy = false;
    u64 chunk_bytes = 0, cap_bytes = 0;
    std::vector<RingChunk> chunks;
    UINT current = ~0u;
    u64 seq = 0;
    std::unordered_map<Wrapper*, RingList> lists;
    // Counters (since the start; the report line also gives the change since the last line).
    u64 reserved = 0, refused_cap = 0, refused_cap_bytes = 0, refused_size = 0, overwritten = 0, overwritten_uses = 0,
        would_wrap = 0, reexecuted = 0, made = 0, released = 0, peak_in_use = 0, cache_stale = 0, draws_refused = 0, unfenced = 0;
    u64 last_reserved = 0;
    LONG last_report_frame = 0;
};
UploadRing& upload_ring_state()
{
    static UploadRing r;
    return r;
}
// Cap refusals on this thread: draw_translated compares it before and after its copies and does not issue a draw
// whose copy was refused.
thread_local u64 ring_cap_refusals;
u64 ring_env(const char* name, u64 fallback)
{
    char value[24]{};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n && n < sizeof(value) ? std::strtoull(value, nullptr, 0) : fallback;
}

// Size of the old ring: 256 MiB, or X2W_UPLOAD_RING_TEST_KIB in fixtures.
u64 ring_env_test_ring()
{
    static const u64 bytes = [] {
        const u64 kib = ring_env("X2W_UPLOAD_RING_TEST_KIB", 0);
        return kib ? kib << 10 : u64{256} << 20;
    }();
    return bytes;
}

// r.lock held.
bool ring_make_buffer(u64 bytes, ID3D12Resource** buffer, unsigned char** mapped)
{
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* created = nullptr;
    void* base = nullptr;
    if (FAILED(device_of(the_device)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                              nullptr, IID_PPV_ARGS(&created))))
        return false;
    if (FAILED(created->Map(0, nullptr, &base))) {
        created->Release();
        return false;
    }
    *buffer = created;
    *mapped = static_cast<unsigned char*>(base);
    return true;
}

// r.lock held.
void ring_setup(UploadRing& r)
{
    r.ready = true;
    char mode[16]{};
    GetEnvironmentVariableA("X2W_UPLOAD_RING", mode, sizeof(mode));
    r.legacy = !std::strcmp(mode, "legacy");
    const u64 test_kib = ring_env("X2W_UPLOAD_RING_TEST_KIB", 0);
    if (r.legacy) {
        const u64 ring = test_kib ? test_kib << 10 : u64{256} << 20;
        ID3D12Resource* buffer = nullptr;
        unsigned char* mapped = nullptr;
        if (ring_make_buffer(ring, &buffer, &mapped)) {
            for (UINT i = 0; i < 4; ++i) {
                RingChunk c;
                c.buffer = buffer;
                c.mapped = mapped + i * (ring / 4);
                c.base = i * (ring / 4);
                c.bytes = ring / 4;
                r.chunks.push_back(c);
            }
            r.chunk_bytes = ring / 4;
        }
        r.cap_bytes = ring;
    } else {
        r.chunk_bytes = test_kib ? test_kib << 10 : ring_env("X2W_UPLOAD_RING_CHUNK_MIB", 64) << 20;
        r.cap_bytes = ring_env("X2W_UPLOAD_RING_CAP_MIB", 2048) << 20;
        if (const u64 cap_chunks = ring_env("X2W_UPLOAD_RING_TEST_CAP_CHUNKS", 0)) r.cap_bytes = cap_chunks * r.chunk_bytes;
        if (r.cap_bytes < r.chunk_bytes) r.cap_bytes = r.chunk_bytes;
    }
    log("x2w-upload ring: %s, %llu KiB chunks, cap %llu MiB%s", r.legacy ? "legacy (one ring, wraps without a fence check; tracked as 4 segments)"
                                                                         : "chain (a chunk is reused only after its lists executed and their fences passed)",
        r.chunk_bytes >> 10, r.cap_bytes >> 20, r.legacy && r.chunks.empty() ? ": ring buffer FAILED" : "");
}

// r.lock held. Does a live list's current recording still hold chunk c (overlapped its use, not executed yet)?
bool ring_held_by_lists(const UploadRing& r, const RingChunk& c)
{
    for (const auto& [list, l] : r.lists) {
        if (!l.reset || l.reset > c.last_use) continue;      // never recorded, or re-recorded after the last use
        if (l.close && l.close < c.first_use) continue;       // closed before the chunk was first used
        if (l.close && l.executed > l.close) continue;        // that recording was executed
        return true;
    }
    return false;
}

// r.lock held. Moves chunks along in_use -> fenced -> free.
void ring_poll(UploadRing& r)
{
    std::vector<std::pair<ID3D12Fence*, UINT64>> now;
    bool now_taken = false, unfenced = false;
    for (UINT i = 0; i < r.chunks.size(); ++i) {
        RingChunk& c = r.chunks[i];
        if (c.state == RingChunk::in_use && i != r.current && !ring_held_by_lists(r, c)) {
            if (!now_taken) {
                AcquireSRWLockShared(&allocator_lock);
                for (const auto& [queue, submit] : queue_submits) {
                    if (submit.first) now.push_back(submit);
                    else unfenced = true;  // note_submits could not make this queue's fence: nothing to wait on
                }
                ReleaseSRWLockShared(&allocator_lock);
                now_taken = true;
            }
            if (unfenced) {
                ++r.unfenced;  // kept in use: a queue without a submit fence cannot prove the GPU is done
                continue;
            }
            c.waits = now;
            c.state = RingChunk::fenced;
        }
        if (c.state == RingChunk::fenced) {
            bool passed = true;
            for (const auto& [fence, value] : c.waits) passed &= fence->GetCompletedValue() >= value;
            if (passed) {
                c.state = RingChunk::free_;
                ++c.generation;  // cached frame copies in it are gone
                c.idle_since = frame_number;
            }
        }
    }
    // Chain: keep at most 4 free chunks (256 MiB at the default size); release the rest after 300 idle frames.
    if (!r.legacy) {
        UINT free_count = 0;
        for (UINT i = 0; i < r.chunks.size(); ++i) {
            RingChunk& c = r.chunks[i];
            if (c.state != RingChunk::free_ || !c.buffer) continue;
            if (++free_count > 4 && frame_number - c.idle_since > 300) {
                c.buffer->Release();
                c.buffer = nullptr;
                c.mapped = nullptr;
                ++r.released;
            }
        }
    }
}

// r.lock held. Bytes in chunks that are not free (in use or waiting for fences).
u64 ring_in_use(const UploadRing& r)
{
    u64 bytes = 0;
    for (const RingChunk& c : r.chunks)
        if (c.buffer && c.state != RingChunk::free_) bytes += c.bytes;
    return bytes;
}

// r.lock held. A chunk to write into next, or ~0u (chain at its cap).
UINT ring_next_chunk(UploadRing& r)
{
    if (r.legacy) {
        if (r.chunks.empty()) return ~0u;
        const UINT next = r.current == ~0u ? 0 : (r.current + 1) % 4;
        RingChunk& c = r.chunks[next];
        if (c.state != RingChunk::free_) ring_poll(r);
        c.unsafe = c.state != RingChunk::free_;  // the old ring reuses it anyway: count what it overwrites
        ++c.generation;
        return next;
    }
    ring_poll(r);
    for (UINT i = 0; i < r.chunks.size(); ++i)
        if (r.chunks[i].state == RingChunk::free_ && r.chunks[i].buffer && i != r.current) return i;
    u64 live = 0;
    for (const RingChunk& c : r.chunks) live += c.buffer ? c.bytes : 0;
    if (live + r.chunk_bytes > r.cap_bytes) return ~0u;
    RingChunk c;
    c.bytes = r.chunk_bytes;
    if (!ring_make_buffer(c.bytes, &c.buffer, &c.mapped)) return ~0u;
    ++r.made;
    for (UINT i = 0; i < r.chunks.size(); ++i)
        if (!r.chunks[i].buffer) {  // reuse a released slot (keeps its generation counting up)
            c.generation = r.chunks[i].generation + 1;
            r.chunks[i] = c;
            return i;
        }
    r.chunks.push_back(c);
    return static_cast<UINT>(r.chunks.size() - 1);
}
}  // namespace

// A slice of `bytes` for copies the GPU reads from a list recorded now. Returns its CPU address and sets the
// buffer and offset to bind (and the tag frame_copy keeps), or nullptr when refused.
unsigned char* reserve_upload_ring(std::size_t bytes, u64* offset, ID3D12Resource** buffer, RingTag* tag)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    if (!r.ready) ring_setup(r);
    const u64 rounded = (bytes + 511) & ~u64{511};  // 512: texture footprints are placed in the ring too
    if (!bytes || rounded > r.chunk_bytes || (r.legacy && r.chunks.empty())) {
        ++r.refused_size;
        ReleaseSRWLockExclusive(&r.lock);
        x2w_memcut::note_ring_refused();
        return nullptr;
    }
    if (r.current == ~0u || r.chunks[r.current].used + rounded > r.chunks[r.current].bytes) {
        const UINT next = ring_next_chunk(r);
        if (next == ~0u) {
            ++ring_cap_refusals;
            ++r.refused_cap;
            r.refused_cap_bytes += rounded;
            const u64 refused = r.refused_cap;
            ReleaseSRWLockExclusive(&r.lock);
            x2w_memcut::note_ring_refused();
            if (refused <= 8 || refused % 4096 == 0)
                log("x2w-upload ring: refused %zu bytes at frame %ld: the chain is at its cap (X2W_UPLOAD_RING_CAP_MIB) and no chunk "
                    "is free yet; the copy fails (refusal %llu)", bytes, frame_number, refused);
            return nullptr;
        }
        r.current = next;
        RingChunk& c = r.chunks[next];
        c.state = RingChunk::in_use;
        c.used = 0;
        c.first_use = ++r.seq;
        c.waits.clear();
    }
    RingChunk& c = r.chunks[r.current];
    const u64 at = c.used;
    c.used += rounded;
    c.last_use = ++r.seq;
    r.reserved += rounded;
    const bool first_overwrite = c.unsafe && !r.overwritten_uses;
    if (c.unsafe) r.overwritten += rounded, ++r.overwritten_uses;
    const u64 in_use = ring_in_use(r);
    if (in_use > r.peak_in_use) r.peak_in_use = in_use;
    // The old ring (256 MiB, or the test size) would have overwritten data the GPU had not read.
    const u64 old_ring = ring_env_test_ring();
    const bool first_wrap = !r.legacy && in_use > old_ring && !r.would_wrap;
    if (!r.legacy && in_use > old_ring) r.would_wrap += rounded;
    *offset = c.base + at;
    *buffer = c.buffer;
    if (tag) *tag = RingTag{r.current, c.generation};
    unsigned char* cpu = c.mapped + at;
    ReleaseSRWLockExclusive(&r.lock);
    x2w_memcut::note_ring(frame_number, rounded);  // LOCAL memcut
    if (first_overwrite)
        log("x2w-upload ring (legacy): first copy written over ring data the GPU had not read yet (frame %ld, %zu bytes)", frame_number, bytes);
    if (first_wrap)
        log("x2w-upload ring (chain): first copy past the old ring's size while all of it is still unread (frame %ld): the old ring "
            "would have overwritten unread data here", frame_number);
    return cpu;
}

// frame_copy cache hit: the slice may be bound again only while its chunk was not recycled. Marks the use.
bool ring_touch(const RingTag& tag)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    RingChunk* c = tag.chunk < r.chunks.size() ? &r.chunks[tag.chunk] : nullptr;
    // Same generation = not recycled since the copy was made; a recycled chunk is free or in use again.
    const bool same = c && c->buffer && c->generation == tag.generation && c->state != RingChunk::free_;
    if (!same) ++r.cache_stale;
    if (same) c->last_use = ++r.seq;
    ReleaseSRWLockExclusive(&r.lock);
    return same || r.legacy;  // legacy: as before, the cache trusts its offset (stale hits are only counted)
}

// draw_translated: a draw not issued because one of its copies was refused at the cap.
void ring_draw_refused(u64 key)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    const u64 n = ++r.draws_refused;
    ReleaseSRWLockExclusive(&r.lock);
    if (n <= 8 || n % 4096 == 0)
        log("x2w-upload ring: draw %016llx not issued at frame %ld: a copy it needs was refused (chain at its cap); refused draws %llu",
            key, frame_number, n);
}

// Hooks: list Reset / Close succeeded, lists executed (draw_lock held, after note_submits), list destroyed.
void ring_list_reset(Wrapper* list)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    RingList& l = r.lists[list];
    l.reset = ++r.seq;
    l.close = 0;
    ReleaseSRWLockExclusive(&r.lock);
}
void ring_list_close(Wrapper* list)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    if (const auto l = r.lists.find(list); l != r.lists.end()) l->second.close = ++r.seq;
    ReleaseSRWLockExclusive(&r.lock);
}
void ring_list_gone(Wrapper* list)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    r.lists.erase(list);
    ReleaseSRWLockExclusive(&r.lock);
}
void ring_submitted(const std::vector<Wrapper*>& lists)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockExclusive(&r.lock);
    for (Wrapper* w : lists)
        if (const auto l = r.lists.find(w); l != r.lists.end()) {
            if (l->second.executed > l->second.close && l->second.close) ++r.reexecuted;  // the same recording again
            l->second.executed = ++r.seq;
        }
    if (r.ready) ring_poll(r);
    const LONG frame = frame_number;
    const bool report = r.ready && frame - r.last_report_frame >= 300;
    char line[1024];
    if (report) {
        r.last_report_frame = frame;
        UINT total = 0, in_use = 0, fenced = 0, free_count = 0;
        u64 held = 0;
        for (const RingChunk& c : r.chunks) {
            if (!c.buffer) continue;
            ++total;
            in_use += c.state == RingChunk::in_use;
            fenced += c.state == RingChunk::fenced;
            free_count += c.state == RingChunk::free_;
            held += c.state == RingChunk::in_use && ring_held_by_lists(r, c);
        }
        const u64 total_mib = r.legacy ? (r.chunks.empty() ? 0 : 4 * r.chunk_bytes) >> 20 : total * r.chunk_bytes >> 20;
        std::snprintf(line, sizeof(line),
                      "x2w-upload ring (frame %ld, %s): chunks %u (%llu MiB: in use %u, %llu of them held by unexecuted lists, waiting for "
                      "fences %u, free %u), peak in use %llu MiB, reserved %llu MiB since the last line (%llu MiB total), refused at "
                      "the cap %llu (%llu MiB, draws not issued %llu), refused over the chunk size %llu, written over data the GPU had not read %llu MiB in %llu "
                      "copies, would have wrapped in the old ring %llu MiB, re-executed recordings %llu, stale cache hits %llu, chunks "
                      "made %llu released %llu, lists %zu, kept for a queue without a fence %llu",
                      frame, r.legacy ? "legacy" : "chain", total, total_mib, in_use, held, fenced, free_count, r.peak_in_use >> 20,
                      (r.reserved - r.last_reserved) >> 20, r.reserved >> 20, r.refused_cap, r.refused_cap_bytes >> 20, r.draws_refused, r.refused_size,
                      r.overwritten >> 20, r.overwritten_uses, r.would_wrap >> 20, r.reexecuted, r.cache_stale, r.made, r.released,
                      r.lists.size(), r.unfenced);
        r.last_reserved = r.reserved;
    }
    ReleaseSRWLockExclusive(&r.lock);
    static LONG reports;
    if (report && first_calls(reports, 400)) log("%s", line);
}

// Fixtures (export x2w_upload_ring_test, after the file's namespace): counters by index (0 reserved, 1 refused at
// the cap, 2 overwritten bytes, 3 would-wrap bytes, 4 chunks with a buffer, 5 stale cache hits, 6 legacy).
unsigned long long upload_ring_counter(int what)
{
    UploadRing& r = upload_ring_state();
    AcquireSRWLockShared(&r.lock);
    u64 v = 0;
    switch (what) {
    case 0: v = r.reserved; break;
    case 1: v = r.refused_cap; break;
    case 2: v = r.overwritten; break;
    case 3: v = r.would_wrap; break;
    case 4: for (const RingChunk& c : r.chunks) v += c.buffer != nullptr; break;
    case 5: v = r.cache_stale; break;
    case 6: v = r.legacy; break;
    case 7: v = r.draws_refused; break;
    default: break;
    }
    ReleaseSRWLockShared(&r.lock);
    return v;
}
