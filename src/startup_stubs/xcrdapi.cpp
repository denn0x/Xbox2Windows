// Desktop XCRDAPI.dll stand-in for startup experiments (docs/startup-runtime.md).
//
// DStorageGetFactory returns a recording COM stand-in. Methods whose PC DirectStorage meaning matches
// the observed call shape are implemented on Win32 and logged as "x2w-dstorage"; every other vtable
// slot of every object logs "x2w-trap XCRDAPI.dll!<Object>::slot<N>" with its argument registers and
// terminates the process, so each run names the next method that needs an implementation.
// Exit code for an object slot: 0xE2570400 | (object base + slot), see Kind.
#define X2W_TRAP_MODULE "XCRDAPI.dll"
#define X2W_TRAP_MODULE_ID 4
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"
#include "startup_stubs/inflate.h"
#include "startup_stubs/dstorage_file_read.h"
#include "startup_stubs/bcpack_decoder.h"
#include "startup_stubs/bcpack_bc6h.h"
#include "startup_stubs/dstorage_status_stats.h"
#include <algorithm>
#include <memory>
#include <map>

#include <psapi.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

using u64 = std::uint64_t;
constexpr int slots = 32;

enum Kind : unsigned { factory = 0x20, file = 0x40, status = 0x60, queue = 0x80 };
const char* kind_name(unsigned kind)
{
    switch (kind) {
    case factory: return "Factory";
    case file: return "File";
    case status: return "StatusArray";
    case queue: return "Queue";
    default: return "Object";
    }
}

struct Object {
    void** vtable;
    unsigned kind;
    LONG refs;
    HANDLE handle;      // File
    unsigned capacity;  // StatusArray, Queue
    LONG* results;      // StatusArray: one HRESULT per index, E_PENDING until a status is enqueued
    LONG pending;       // Queue: worst HRESULT of the requests since the last status
    struct QueueWork* work = nullptr;  // Queue: entries waiting for Submit and the worker running them
    struct Unrecorded* unrecorded_head = nullptr;  // Queue: entries no queue storage could hold, see push()
    struct Unrecorded* unrecorded_tail = nullptr;
    x2w_dstorage_stats::StatusStats* stats = nullptr;  // StatusArray: LOCAL wait diagnostics (dstorage_status_stats.h)
};

// A queue entry as enqueued: a request (its 0x48 bytes copied, and a snapshot of its destination page
// tokens), a status entry or a fence signal. Entries run in order on the queue's worker after Submit,
// like DirectStorage; a destination page token that was 0 at enqueue is read again at the entry's turn
// (titles fill those in after EnqueueRequest).
//
// An Entry is default-constructed, moved and move-assigned on the Enqueue/Submit path, where an
// allocation may fail and nothing may escape. Those operations are implicitly noexcept, so no member
// may allocate in them: std::vector does in MSVC's debug iterator mode (its default and move
// constructors allocate a container proxy), and a bad_alloc there is std::terminate. The page tokens
// are therefore a plain array, allocated once with new (std::nothrow) at EnqueueRequest.
struct Entry {
    enum Type { request, status_entry, signal } type;
    unsigned char bytes[0x48];
    std::unique_ptr<ULONG_PTR[]> tokens;
    std::size_t token_count = 0;
    Object* array;
    unsigned index;
    void* fence;
    u64 value;
    u64 id = 0;
    u64 submission_id = 0;
    u64 after = 0;  // failing: entries the worker must have finished before this one completes
    LONG status_gen = 0;  // status entry: its EnqueueStatus generation (dstorage_status_stats.h)
};
struct CachedStream {
    u64 source = 0;
    u64 base_offset = 0;
    unsigned compressed_size = 0;
    unsigned chunk_size = 0;
    std::vector<u64> continuation_ids;
    std::vector<unsigned char> data;
};

struct QueueWork {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> waiting, submitted;
    std::deque<Entry> failing;   // snapshot of waiting entries a Submit could not move, completed in order
    struct Unrecorded* deferred_head = nullptr;  // snapshot of unrecorded entries, in order; guarded by mutex
    struct Unrecorded* deferred_tail = nullptr;
    bool completing = false;     // a thread owns completion of snapshot entries, so they stay in order; guarded by mutex
    std::condition_variable drained;  // signaled when the worker finishes an entry
    u64 submitted_total = 0;          // entries ever moved to `submitted`, guarded by mutex
    u64 finished_total = 0;           // entries the worker has finished running, guarded by mutex
    bool worker_started = false;  // guarded by the setup mutex
    u64 next_entry_id = 0;
    u64 next_submission_id = 0;
    static constexpr std::size_t max_cached_streams = 16;
    std::deque<CachedStream> cached_streams;
    unsigned char last_served[0x48] = {};  // LOCAL: the latest request this queue served (worker only), for FAIL lines
    bool served_any = false;

    void put_stream(u64 source, u64 base_offset, unsigned compressed_size, unsigned chunk_size,
                    std::vector<u64> continuation_ids, std::vector<unsigned char> data)
    {
        for (auto it = cached_streams.begin(); it != cached_streams.end(); ++it) {
            if (it->source == source && it->base_offset == base_offset && it->compressed_size == compressed_size) {
                cached_streams.erase(it);
                break;
            }
        }
        if (cached_streams.size() >= max_cached_streams) {
            cached_streams.pop_front();
        }
        cached_streams.push_back({source, base_offset, compressed_size, chunk_size,
                                  std::move(continuation_ids), std::move(data)});
    }
};

static bool has_zlib_header(const unsigned char* bytes, std::size_t size)
{
    if (!bytes || size < 2) return false;
    const unsigned char cmf = bytes[0];
    const unsigned char flg = bytes[1];
    if ((cmf & 0x0F) != 8) return false;
    if ((cmf >> 4) > 7) return false;
    if ((static_cast<unsigned>(cmf) * 256 + flg) % 31 != 0) return false;
    return true;
}

// True when every byte of [destination, destination + bytes) is committed and writable, with no guard or
// no-access page in between. Walks the regions one VirtualQuery at a time, so a destination that spans
// regions of different protection is judged page by page. Nothing is written or touched.
static bool destination_writable(const void* destination, std::size_t bytes)
{
    if (bytes == 0) return true;
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(destination);
    if (!destination || bytes > UINTPTR_MAX - cursor) return false;
    const std::uintptr_t end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        const DWORD protect = info.Protect & 0xFF;
        if (protect != PAGE_READWRITE && protect != PAGE_WRITECOPY && protect != PAGE_EXECUTE_READWRITE &&
            protect != PAGE_EXECUTE_WRITECOPY)
            return false;
        const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= cursor) return false;
        cursor = region_end;
    }
    return true;
}

// Return the exact queued requests that continue this stream in this submission, in queue order.
// Status entries are skipped, and any unrelated request, submission boundary, or non-matching
// source/offset ends the continuation chain.
static std::vector<u64> following_chunk_ids(Object* self, u64 current_submission_id, u64 source, u64 offset,
                                            unsigned source_size, unsigned destination_size, std::size_t total_size)
{
    std::vector<u64> ids;
    if (!self || !self->work) return ids;
    QueueWork& work = *self->work;
    const std::lock_guard guard(work.mutex);
    const unsigned step = destination_size ? destination_size : 0x10000;
    u64 expected = offset + step;
    if (expected < offset) return ids;

    for (const auto& e : work.submitted) {
        if (e.submission_id != current_submission_id) {
            break;
        }
        if (e.type != Entry::request) continue;
        const u64 e_options = *reinterpret_cast<const u64*>(e.bytes);
        if (!(e_options & 1) || (e_options & 0x400000)) {
            break;
        }
        const u64 e_source = *reinterpret_cast<const u64*>(e.bytes + 0x20);
        const u64 e_offset = *reinterpret_cast<const u64*>(e.bytes + 0x28);
        const unsigned e_source_size = *reinterpret_cast<const unsigned*>(e.bytes + 0x30);
        if (e_source != source || e_source_size != source_size || e_offset != expected) {
            break;
        }
        const u64 delta = expected - offset;
        if (delta >= total_size) {
            break;
        }
        ids.push_back(e.id);
        if (expected > UINT64_MAX - step) {
            break;
        }
        expected += step;
    }
    return ids;
}

void log(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 3) n = sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    x2w_trap::write_all(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n));
    x2w_trap::append_trap_log(line, static_cast<DWORD>(n));  // one open handle per module (trap.h)
}

void guid_text(const void* guid, char* out, std::size_t size)
{
    if (!guid) {
        std::snprintf(out, size, "null");
        return;
    }
    const auto* g = static_cast<const GUID*>(guid);
    std::snprintf(out, size, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3,
                  g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
                  g->Data4[7]);
}

template <int N>
__declspec(noinline) u64 trap_slot(Object* self, u64 rdx, u64 r8, u64 r9)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%s::slot%d", kind_name(self->kind), N);
    x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, (self->kind + N) & 0xFF,
                   reinterpret_cast<u64>(self), rdx, r8, r9, _ReturnAddress());
}

template <std::size_t... I>
void fill_traps(void** table, std::index_sequence<I...>)
{
    ((table[I] = reinterpret_cast<void*>(&trap_slot<static_cast<int>(I)>)), ...);
}

// IUnknown, shared by every object.
long __stdcall query_interface(Object* self, const GUID* riid, void** out)
{
    char text[48];
    guid_text(riid, text, sizeof(text));
    log("x2w-dstorage %s::QueryInterface riid=%s -> self", kind_name(self->kind), text);
    if (!out) return E_POINTER;
    InterlockedIncrement(&self->refs);
    *out = self;
    return S_OK;
}
unsigned long __stdcall add_ref(Object* self) { return static_cast<unsigned long>(InterlockedIncrement(&self->refs)); }
unsigned long __stdcall release(Object* self) { return static_cast<unsigned long>(InterlockedDecrement(&self->refs)); }

void** make_table(unsigned kind);

Object* make(unsigned kind)
{
    auto* object = new Object{};
    object->vtable = make_table(kind);
    object->kind = kind;
    object->refs = 1;
    object->handle = INVALID_HANDLE_VALUE;
    return object;
}

// Factory slot 4, PC IDStorageFactory::OpenFile(PCWSTR path, REFIID riid, void** ppv).
long __stdcall factory_open_file(Object*, const wchar_t* path, const GUID* riid, void** out)
{
    char id[48];
    guid_text(riid, id, sizeof(id));
    HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    log("x2w-dstorage Factory::OpenFile path=\"%ls\" riid=%s -> %s (err %lu)", path ? path : L"(null)", id,
        handle == INVALID_HANDLE_VALUE ? "fail" : "ok", error);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(error);
    Object* object = make(file);
    object->handle = handle;
    *out = object;
    return S_OK;
}

// Factory slot 5, PC IDStorageFactory::CreateStatusArray(UINT32 capacity, PCSTR name, REFIID, void**).
long __stdcall factory_create_status_array(Object*, unsigned capacity, const char* name, const GUID* riid, void** out)
{
    char id[48];
    guid_text(riid, id, sizeof(id));
    log("x2w-dstorage Factory::CreateStatusArray capacity=%u name=\"%s\" riid=%s", capacity, name ? name : "", id);
    if (!out) return E_POINTER;
    Object* object = make(status);
    object->capacity = capacity;
    object->results = new LONG[capacity ? capacity : 1];
    for (unsigned i = 0; i < capacity; ++i) object->results[i] = static_cast<LONG>(0x8000000A);  // E_PENDING
    object->stats = x2w_dstorage_stats::make(capacity, object->results);
    *out = object;
    return S_OK;
}

// Factory slot 8: called once at startup with 32 MiB (staging buffer size in PC terms).
long __stdcall factory_slot8(Object*, unsigned value)
{
    log("x2w-dstorage Factory::slot8 value=0x%x -> S_OK", value);
    return S_OK;
}

// Factory slot 10: (descriptor*, riid, ppv); creates a queue (the PC CreateQueue shape, wider descriptor).
long __stdcall factory_create_queue(Object*, const unsigned char* desc, const GUID* riid, void** out)
{
    char id[48];
    guid_text(riid, id, sizeof(id));
    char bytes[3 * 40 + 1] = "";
    if (desc)
        for (int i = 0; i < 40; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", desc[i]);
    const char* name = desc ? *reinterpret_cast<const char* const*>(desc + 0x10) : nullptr;
    log("x2w-dstorage Factory::CreateQueue desc=[%s] name=\"%s\" riid=%s", bytes,
        name && x2w_local::readable(name, 1) ? name : "?", id);
    if (!out) return E_POINTER;
    Object* object = make(queue);
    object->capacity = desc ? *reinterpret_cast<const std::uint16_t*>(desc + 8) : 0;
    *out = object;
    return S_OK;
}

// File slot 3, PC IDStorageFile::Close().
void __stdcall file_close(Object* self)
{
    log("x2w-dstorage File::Close handle=%p", self->handle);
    if (self->handle != INVALID_HANDLE_VALUE) CloseHandle(self->handle);
    self->handle = INVALID_HANDLE_VALUE;
}

// File slot 4, PC IDStorageFile::GetFileInformation(BY_HANDLE_FILE_INFORMATION*).
long __stdcall file_information(Object* self, BY_HANDLE_FILE_INFORMATION* info)
{
    const BOOL ok = info && GetFileInformationByHandle(self->handle, info);
    const DWORD error = ok ? 0 : GetLastError();  // before log(), whose Win32 calls overwrite it
    log("x2w-dstorage File::GetFileInformation -> %s size=0x%llx", ok ? "ok" : "fail",
        ok ? (static_cast<unsigned long long>(info->nFileSizeHigh) << 32) | info->nFileSizeLow : 0ull);
    return ok ? S_OK : HRESULT_FROM_WIN32(error);
}

// Queue slot 3, EnqueueRequest(request*) by the PC numbering. Request layout from the title's builder
// (a hypothesis from its two code paths): +0x00 options (0x400000: memory source; low byte: compression
// format, 1 seen), +0x08 destination pointer, +0x18 destination size (u32), +0x20 source (File object
// or memory pointer), +0x28 source offset, +0x30 source size (u32). Uncompressed requests are executed
// synchronously; compressed ones are logged and left undone. The first requests are logged in full.
LONG requests_done, requests_compressed, requests_failed, requests_bcpack_blank;

// LOCAL: BC6H (BCPack format 6) is zero-filled unless X2W_BC6H_FALLBACK=1. With it, streams go through
// bcpack_bc6h.h: blocks free of the format's dictionary (unknown) are rebuilt, the rest get the
// endpoints of the nearest known block or a constant (approximate texels, never exact); =2 uses the
// constant for every unknown-endpoint block (no copying). X2W_BC6H_FILL_BASE (decimal,
// mode-13 base, default 16913 = UF16 2^-7) sets the constant; 0 gives zero blocks.
char bc6h_fallback_mode() {
    static const char mode = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("X2W_BC6H_FALLBACK", value, sizeof(value)) ? value[0] : '0';
    }();
    return mode;
}

bool bc6h_fallback_enabled() { return bc6h_fallback_mode() == '1' || bc6h_fallback_mode() == '2'; }

volatile LONG64 bc6h_blocks, bc6h_exact, bc6h_endpoints_only, bc6h_copied, bc6h_constant;
LONG bc6h_streams;

bool decode_bc6h_fallback(const unsigned char* input, std::size_t size, unsigned char* out, std::size_t out_size,
                          std::size_t* produced) {
    static const uint16_t fill = [] {
        char value[16] = {};
        if (!GetEnvironmentVariableA("X2W_BC6H_FILL_BASE", value, sizeof(value))) return x2w_bcpack::bc6h::DEFAULT_FILL_BASE;
        return static_cast<uint16_t>(std::strtoul(value, nullptr, 10));
    }();
    x2w_bcpack::bc6h::Stats st;
    if (!x2w_bcpack::bc6h::decode_stream(input, size, out, out_size, produced, &st, nullptr, fill,
                                             bc6h_fallback_mode() != '2')) return false;
    InterlockedAdd64(&bc6h_blocks, static_cast<LONG64>(st.blocks));
    InterlockedAdd64(&bc6h_exact, static_cast<LONG64>(st.exact));
    InterlockedAdd64(&bc6h_endpoints_only, static_cast<LONG64>(st.endpoints_only));
    InterlockedAdd64(&bc6h_copied, static_cast<LONG64>(st.copied));
    InterlockedAdd64(&bc6h_constant, static_cast<LONG64>(st.constant));
    const LONG n = InterlockedIncrement(&bc6h_streams);
    if (x2w_local::first_of(n, 16) || (n & 63) == 0)
        log("x2w-dstorage BC6H fallback: stream %ld blocks %zu exact %zu endpoints-only %zu copied %zu constant %zu "
            "(totals: blocks %lld exact %lld endpoints-only %lld copied %lld constant %lld)",
            n, st.blocks, st.exact, st.endpoints_only, st.copied, st.constant, bc6h_blocks, bc6h_exact,
            bc6h_endpoints_only, bc6h_copied, bc6h_constant);
    return true;
}
// LOCAL: failed requests by outcome text (outcomes are string literals), for the heartbeat.
struct FailureTally { const char* outcome; LONG count; };
FailureTally failure_tally[16];
SRWLOCK failure_lock = SRWLOCK_INIT;
void tally_failure(const char* outcome)
{
    AcquireSRWLockExclusive(&failure_lock);
    for (auto& t : failure_tally) {
        if (!t.outcome || t.outcome == outcome || std::strcmp(t.outcome, outcome) == 0) {
            t.outcome = outcome;
            ++t.count;
            break;
        }
    }
    ReleaseSRWLockExclusive(&failure_lock);
}
LONG zero_tokens_at_enqueue, zero_tokens_at_run, tokens_replaced_after_enqueue;
// Archives of one kind (an RDS5 boundary table at file offset 0) are addressed by the executable 14 x 64 KiB
// after where their data sits on disk (measured on one executable: every shifted zlib stream starts exactly
// at a table boundary 0xE0000 before the requested offset, and pieces in the last 0xE0000 bytes point past
// the end of the file). Why the executable adds the shift is not known. A request is read from the shifted
// offset when the table has a boundary there and none at the requested offset, or when only the shifted
// range fits in the file. LOCAL SHORTCUT until the offset arithmetic is understood.
struct Rds5Boundary { u64 stored, logical; unsigned codec, padding; };
const std::vector<Rds5Boundary>* rds5_table(HANDLE handle)
{
    static std::mutex lock;
    static std::map<HANDLE, std::unique_ptr<std::vector<Rds5Boundary>>> tables;
    const std::lock_guard guard(lock);
    auto& slot = tables[handle];
    if (slot) return slot->empty() ? nullptr : slot.get();
    slot = std::make_unique<std::vector<Rds5Boundary>>();
    const auto read_at = [handle](u64 at, void* out, DWORD size) {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(at);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        return ReadFile(handle, out, size, &read, &overlapped) && read == size;
    };
    unsigned char header[16];
    if (!read_at(0, header, 16) || std::memcmp(header, "RDS5", 4) != 0) return nullptr;
    std::uint32_t count = 0;
    std::memcpy(&count, header + 4, 4);
    if (count < 2 || count > (1u << 20)) return nullptr;
    std::vector<std::uint32_t> raw(std::size_t{count} * 2);
    if (!read_at(16, raw.data(), count * 8)) return nullptr;
    slot->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t low = raw[2 * i], high = raw[2 * i + 1];
        slot->push_back({u64{low & 0x03ffffffu} * 256, (u64{high & 0xfffffu} * 4 + (low >> 30)) * 4096, (low >> 26) & 15, high >> 20});
    }
    return slot.get();
}
LONG requests_archive_shifted, requests_archive_past_end;
// Off unless X2W_DSTORAGE_ARCHIVE_SHIFT=1: it served no request in runs 85-87 (heartbeat archive-shifted=0
// past-end=0; the RDS5 interval slices took over), and it serves a request past the end of the file that
// must fail (startup_storage_directory_batch, 'rejected at EOF').
u64 archive_read_offset(HANDLE handle, u64 offset, unsigned size)
{
    static const bool enabled = x2w_local::env_on("X2W_DSTORAGE_ARCHIVE_SHIFT");
    constexpr u64 shift = 14 * 0x10000;
    if (!enabled || offset < shift) return offset;
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(handle, &file_size)) return offset;
    const u64 end = static_cast<u64>(file_size.QuadPart);
    const bool fits = offset + size <= end, shifted_fits = offset - shift + size <= end;
    if (!fits && shifted_fits) {
        InterlockedIncrement(&requests_archive_past_end);
        return offset - shift;
    }
    const auto* table = rds5_table(handle);
    if (!table || !shifted_fits) return offset;
    const auto boundary = [&](u64 at) {
        const auto it = std::lower_bound(table->begin(), table->end(), at, [](const Rds5Boundary& b, u64 v) { return b.stored < v; });
        return it != table->end() && it->stored == at;
    };
    if (boundary(offset - shift) && !boundary(offset)) {
        InterlockedIncrement(&requests_archive_shifted);
        return offset - shift;
    }
    return offset;
}

// LOCAL: a compressed request into an RDS5 interval (measured offline on one executable's archives,
// out/gamerun/storage_work/repro.py). Every zlib interval inflates to exactly its logical size and every
// BCPack interval (table codec = the request's format number, 1 or 7 checked) decodes to exactly its
// logical size; the request names the interval's compressed size (stored span minus the padding in
// the table's high 12 bits) but a destination smaller than that, and its offset is the interval's
// stored start plus the request's offset in the decoded data (k * destination size for the pieces of
// one stream). The interval is the last one at or before the offset with that compressed size whose
// decoded data holds the whole destination; a request for the whole interval, or one that would run
// past the interval's end, is left to the paths below (the 0xE0000-shifted requests above name the
// whole interval as destination).
struct Rds5Slice { u64 stored; std::size_t decoded_size, at; };
LONG requests_interval_slices;
bool rds5_slice(HANDLE handle, u64 offset, unsigned compressed, unsigned destination_size, unsigned codec, Rds5Slice& slice)
{
    const auto* table = rds5_table(handle);
    if (!table || !compressed || !destination_size) return false;
    auto it = std::upper_bound(table->begin(), table->end(), offset, [](u64 v, const Rds5Boundary& b) { return v < b.stored; });
    for (int steps = 0; it != table->begin() && steps < 0x10000; ++steps) {
        --it;
        const u64 at = offset - it->stored;
        if (at >= (64u << 20)) break;
        if (it + 1 == table->end()) continue;
        const u64 span = (it + 1)->stored - it->stored, decoded = (it + 1)->logical - it->logical;
        if (it->codec != codec || span <= it->padding || span - it->padding != compressed) continue;
        // A request for the whole interval already works below (and must not evict cached streams).
        if (at + destination_size > decoded || (!at && destination_size == decoded)) return false;
        slice ={it->stored, static_cast<std::size_t>(decoded), static_cast<std::size_t>(at)};
        return true;
    }
    return false;
}

// LOCAL: multi-piece zlib streams across queues and submissions (measured on one executable). One
// stream that inflates to several destination-sized pieces is enqueued as one request per piece;
// every piece names the same compressed size and piece k's offset is the stream's offset plus k
// times the destination size. The continuation cache above only covers pieces queued in the same
// submission on the same queue. Pieces that miss it are served from a shared cache of recently
// inflated streams, or by finding the stream start on disk (offset - k * piece, a zlib header, and a
// full Adler-checked inflate past piece k).
LONG requests_stream_pieces, requests_pieces_found_on_disk;
// Off unless X2W_DSTORAGE_STREAM_PIECES=1. Upstream's controls (startup_stub_check: a separate submission at
// a cached stream's continuation offset, an undersized whole-stream request) fail these requests, and
// these paths serve them: they cannot tell such a request from a real later piece or piece 0. Since the
// RDS5 interval slices (rds5_slice) these paths served no request in runs 85-87 (heartbeat
// stream-pieces=0 found-on-disk=0), so they stay as an opt-in.
bool stream_pieces_enabled()
{
    static const bool on = x2w_local::env_on("X2W_DSTORAGE_STREAM_PIECES");
    return on;
}
struct InflatedStream { u64 source, base; unsigned compressed; std::shared_ptr<const std::vector<unsigned char>> data; };
std::mutex streams_lock;
std::deque<InflatedStream> streams;
void remember_stream(u64 source, u64 base, unsigned compressed, std::shared_ptr<const std::vector<unsigned char>> data)
{
    const std::lock_guard guard(streams_lock);
    streams.push_front({source, base, compressed, std::move(data)});
    if (streams.size() > 16) streams.pop_back();
}
std::shared_ptr<const std::vector<unsigned char>> remembered_stream(u64 source, u64 base, unsigned compressed)
{
    const std::lock_guard guard(streams_lock);
    for (const InflatedStream& s : streams)
        if (s.source == source && s.base == base && s.compressed == compressed) return s.data;
    return nullptr;
}
bool find_stream(u64 source, u64 offset, unsigned compressed, unsigned piece, std::shared_ptr<const std::vector<unsigned char>>& data, u64& base)
{
    const std::lock_guard guard(streams_lock);
    for (const InflatedStream& s : streams) {
        if (s.source != source || s.compressed != compressed || offset <= s.base || !piece) continue;
        const u64 at = offset - s.base;
        if (at % piece || at >= s.data->size()) continue;
        data = s.data;
        base = s.base;
        return true;
    }
    return false;
}
std::shared_ptr<const std::vector<unsigned char>> stream_on_disk(HANDLE handle, u64 offset, unsigned compressed, unsigned piece, u64& base)
{
    if (!piece || compressed > (64u << 20)) return nullptr;
    const auto read_at = [handle](u64 at, unsigned char* out, unsigned size) {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(at);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        return ReadFile(handle, out, size, &read, &overlapped) && read == size;
    };
    std::vector<unsigned char> packed;
    for (u64 k = 1; k <= 64 && k * piece <= offset; ++k) {
        const u64 at = offset - k * piece;
        unsigned char head[2];
        if (!read_at(at, head, 2) || head[0] != 0x78 || ((head[0] << 8) | head[1]) % 31) continue;
        packed.resize(compressed);
        if (!read_at(at, packed.data(), compressed)) continue;
        std::vector<unsigned char> big(std::min<std::size_t>(std::size_t{32} << 20, std::max<std::size_t>(std::size_t{4} << 20, std::size_t{piece} * (k + 64))));
        std::size_t produced = 0;
        if (!x2w_inflate::inflate_zlib(packed.data(), compressed, big.data(), big.size(), &produced) || produced <= k * piece) continue;
        base = at;
        return std::make_shared<const std::vector<unsigned char>>(big.begin(), big.begin() + produced);
    }
    return nullptr;
}

LONG bcpack_requests[16], bcpack_blank[16];

const char* format_counts(char* out, std::size_t size, LONG* counts)
{
    out[0] = 0;
    std::size_t used = 0;
    for (unsigned format = 1; format < 16; ++format) {
        const LONG count = InterlockedCompareExchange(&counts[format], 0, 0);
        if (!count) continue;
        if (used >= size) break;
        const int n = std::snprintf(out + used, size - used, "%s%u:%ld", used ? " " : "", format, count);
        if (n < 0 || static_cast<std::size_t>(n) >= size - used) {
            out[size - 1] = 0;
            break;
        }
        used += static_cast<std::size_t>(n);
    }
    return used ? out : "none";
}

void log_bcpack_summary()
{
    LONG total = 0;
    for (unsigned format = 1; format < 16; ++format)
        total += InterlockedCompareExchange(&bcpack_requests[format], 0, 0);
    if (!total) return;

    char requests[256], blank[256];
    log("x2w-dstorage BCPack summary: requests formats=%s; zero-filled formats=%s",
        format_counts(requests, sizeof(requests), bcpack_requests),
        format_counts(blank, sizeof(blank), bcpack_blank));
}

void execute_request(Object* self, const unsigned char* request, const ULONG_PTR* tokens, u64 entry_id, u64 submission_id)
{
    if (!request) return;
    const u64 options = *reinterpret_cast<const u64*>(request);
    auto* const destination = *reinterpret_cast<unsigned char* const*>(request + 0x08);
    const unsigned destination_size = *reinterpret_cast<const unsigned*>(request + 0x18);
    const u64 source = *reinterpret_cast<const u64*>(request + 0x20);
    const u64 offset = *reinterpret_cast<const u64*>(request + 0x28);
    const unsigned source_size = *reinterpret_cast<const unsigned*>(request + 0x30);
    const bool memory_source = (options & 0x400000) != 0;
    const unsigned compression = static_cast<unsigned>(options & 0xFF);
    static LONG logged;
    if (x2w_local::first_calls(logged, 12)) {
        char bytes[3 * 0x40 + 1] = "";
        for (int i = 0; i < 0x40; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", request[i]);
        log("x2w-dstorage Queue::EnqueueRequest [%s]", bytes);
    }
    // Option bits (from the reference projects' notes, matched against this title's requests): bit 0
    // zlib, bits 1-4 a BCPack BC format number, bit 21 a destination given as 64 KiB physical page
    // tokens (+0x10 low 16 bits: offset into the first page), bit 22 a memory source.
    const bool zlib = (options & 1) != 0;
    const unsigned bcpack = static_cast<unsigned>((options >> 1) & 0xF);
    if (bcpack) InterlockedIncrement(&bcpack_requests[bcpack]);
    const bool page_destination = (options & (1ull << 21)) != 0;
    const unsigned page_offset = static_cast<unsigned>(*reinterpret_cast<const u64*>(request + 0x10) & 0xFFFF);
    const u64 known = 1 | (0xFull << 1) | (1ull << 21) | 0x400000;
    const char* outcome = "done";
    std::vector<unsigned char> packed, output;
    const unsigned char* input = nullptr;
    bool interval_request = false;
    Rds5Slice slice{};
    if (options & ~known) {
        outcome = "unknown option bits, not done";
    } else if (!destination || !source_size) {
        outcome = "empty";
    } else if (memory_source) {
        input = reinterpret_cast<const unsigned char*>(source);
    } else {
        auto* file_object = reinterpret_cast<Object*>(source);
        // A zlib or a decoded BCPack format, not both (the table has one codec per interval).
        const unsigned codec = zlib ? (bcpack ? 0u : 8u)
                                    : (bcpack == 7 || bcpack == 1 || (bcpack >= 3 && bcpack <= 5) ||
                                       (bcpack == 6 && bc6h_fallback_enabled()) ? bcpack : 0u);
        interval_request = codec && file_object && file_object->kind == file &&
                           rds5_slice(file_object->handle, offset, source_size, destination_size, codec, slice);
        if (file_object && file_object->kind == file &&
            x2w_startup::read_storage_file(file_object->handle, interval_request ? slice.stored : archive_read_offset(file_object->handle, offset, source_size), source_size, packed))
            input = packed.data();
        else
            outcome = "read failed";
    }
    const unsigned char* data = nullptr;
    std::size_t decode_length = source_size;
    const std::size_t destination_capacity = destination_size;
    bool written = false;

    if (input) {
        data = input;
        if (interval_request) {
            // LOCAL: decode the whole interval to its logical size once (kept for its other pieces), then
            // serve this request's slice of it (see rds5_slice).
            std::shared_ptr<const std::vector<unsigned char>> whole = remembered_stream(source, slice.stored, source_size);
            if (!whole || whole->size() != slice.decoded_size) {
                whole = nullptr;
                auto decoded = std::make_shared<std::vector<unsigned char>>(slice.decoded_size);
                std::size_t produced = 0;
                const bool ok = zlib ? x2w_inflate::inflate_zlib(input, source_size, decoded->data(), decoded->size(), &produced)
                                     : bcpack == 6 ? decode_bc6h_fallback(input, source_size, decoded->data(), decoded->size(), &produced)
                                     : x2w_bcpack::decode_bcpack(input, source_size, bcpack, decoded->data(), decoded->size(), &produced);
                if (ok && produced == decoded->size()) {
                    whole = std::move(decoded);
                    remember_stream(source, slice.stored, source_size, whole);
                }
            }
            if (whole) {
                output.assign(whole->begin() + slice.at, whole->begin() + slice.at + destination_size);
                data = output.data();
                decode_length = destination_size;
                outcome = zlib ? "inflated (RDS5 interval slice)" : "bcpack decoded (RDS5 interval slice)";
                InterlockedIncrement(&requests_interval_slices);
            } else {
                outcome = zlib ? "zlib failed (RDS5 interval)" : "bcpack failed (RDS5 interval)";
                data = nullptr;
            }
        } else if (zlib) {
            bool direct_decoded = false;
            bool decode_error = false;
            if (has_zlib_header(input, source_size)) {
                // With BCPack after it, the inflated size is not given; the destination size bounds it.
                // DirectStorage provides UncompressedSize at request + 0x38. When destination is smaller
                // than the stream (e.g. 64 KiB tile/page target), allocate for the full uncompressed stream
                // so single-pass inflate succeeds and verifies Adler32, then check capacity before writing.
                const unsigned uncompressed_size = *reinterpret_cast<const unsigned*>(request + 0x38);
                std::size_t target_size = (uncompressed_size > destination_size) ? uncompressed_size : destination_size;
                if (bcpack) target_size = target_size * 2 + 0x10000;
                output.resize(target_size);
                std::size_t produced = 0;
                bool ok = x2w_inflate::inflate_zlib(input, source_size, output.data(), output.size(), &produced);
                if (!ok && output.size() < 0x80000) {
                    // If destination_size was smaller than the uncompressed stream and uncompressed_size was unset,
                    // expand up to 512 KiB to accommodate larger chunks.
                    std::size_t big_size = std::size_t{destination_size} * 8 + 0x40000;
                    if (big_size < 0x80000) big_size = 0x80000;
                    std::vector<unsigned char> big(big_size);
                    std::size_t big_produced = 0;
                    if (x2w_inflate::inflate_zlib(input, source_size, big.data(), big.size(), &big_produced)) {
                        output = std::move(big);
                        produced = big_produced;
                        ok = true;
                    }
                }
                if (ok) {
                    output.resize(produced);
                    bool cached_chunked = false;
                    // If the stream expanded beyond this request's destination and continuation chunks are queued
                    // in this submission, scope the cache to those identified continuation requests and serve slice 0 (Refs #1136, #1214).
                    // BCPack streams are atomic multi-lane rANS payloads; they are not chunk-split continuation streams (Refs #1139).
                    if (!memory_source && !bcpack && produced > destination_size && self && self->work) {
                        auto continuation_ids = following_chunk_ids(self, submission_id, source, offset, source_size, destination_size, produced);
                        if (!continuation_ids.empty()) {
                            QueueWork& work = *self->work;
                            const std::lock_guard guard(work.mutex);
                            work.put_stream(source, offset, source_size, destination_size, std::move(continuation_ids), output);
                            decode_length = destination_size;
                            data = output.data();
                            outcome = "inflated (chunked stream piece 0)";
                            cached_chunked = true;
                        }
                    }
                    if (!cached_chunked && !memory_source && !bcpack && produced > destination_size && stream_pieces_enabled()) {
                        // LOCAL: piece 0 of a multi-piece stream whose other pieces are not in this
                        // submission: keep the whole stream for them and serve this piece's slice.
                        remember_stream(source, offset, source_size, std::make_shared<const std::vector<unsigned char>>(output));
                        decode_length = destination_size;
                        data = output.data();
                        outcome = "inflated (stream piece 0, shared cache)";
                        InterlockedIncrement(&requests_stream_pieces);
                        cached_chunked = true;
                    }
                    if (!cached_chunked) {
                        decode_length = produced;
                        data = output.data();
                        outcome = produced == destination_size || bcpack ? "inflated" : "inflated (larger/short)";
                    }
                    direct_decoded = true;
                } else {
                    decode_error = true;
                }
            }

            // If not directly decoded from a standalone stream, check if this request is a continuation
            // slice of an already-inflated chunked stream on this queue (Refs #1136, #1144, #1214).
            // A request presenting a zlib header that failed decode (e.g. damaged checksum) is an actual
            // decode error and must stay distinct from a proven continuation. Memory requests stay strictly on direct decode.
            if (!direct_decoded && !decode_error && !memory_source && self && self->work) {
                auto* file_object = reinterpret_cast<Object*>(source);
                if (file_object && file_object->kind == file && file_object->handle != INVALID_HANDLE_VALUE) {
                    QueueWork& work = *self->work;
                    const std::lock_guard guard(work.mutex);
                    for (auto it = work.cached_streams.begin(); it != work.cached_streams.end(); ++it) {
                        if (it->source == source && it->compressed_size == source_size && offset > it->base_offset) {
                            auto pos = std::find(it->continuation_ids.begin(), it->continuation_ids.end(), entry_id);
                            if (pos != it->continuation_ids.end()) {
                                const u64 delta = offset - it->base_offset;
                                const unsigned step = it->chunk_size ? it->chunk_size : 0x10000;
                                if ((delta % step) == 0 && delta < it->data.size()) {
                                    const std::size_t available = it->data.size() - delta;
                                    // Handle short final destinations in multi-piece streams (Refs #1144)
                                    const std::size_t slice_size = destination_size < available ? destination_size : available;
                                    if (slice_size > 0 && destination_size <= available) {
                                        output.assign(it->data.begin() + delta, it->data.begin() + delta + slice_size);
                                        decode_length = slice_size;
                                        data = output.data();
                                        outcome = "inflated (slice from stream cache)";
                                        direct_decoded = true;
                                        it->continuation_ids.erase(pos);
                                        if (it->continuation_ids.empty()) {
                                            work.cached_streams.erase(it);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!direct_decoded && !decode_error && !memory_source && !bcpack && stream_pieces_enabled()) {
                // LOCAL: a later piece that missed the submission's cache (see remember_stream). Not for a
                // request whose own bytes start a zlib stream that failed to decode: that is a damaged
                // stream, and it fails like on the queue-cache path above.
                std::shared_ptr<const std::vector<unsigned char>> whole;
                u64 base = 0;
                auto* file_object = reinterpret_cast<Object*>(source);
                const bool from_file = file_object && file_object->kind == file && file_object->handle != INVALID_HANDLE_VALUE;
                if (find_stream(source, offset, source_size, destination_size, whole, base)) {
                } else if (from_file && (whole = stream_on_disk(file_object->handle, offset, source_size, destination_size, base))) {
                    remember_stream(source, base, source_size, whole);
                    InterlockedIncrement(&requests_pieces_found_on_disk);
                }
                if (whole) {
                    const std::size_t at = static_cast<std::size_t>(offset - base);
                    const std::size_t take = std::min<std::size_t>(destination_size, whole->size() - at);
                    output.assign(whole->begin() + at, whole->begin() + at + take);
                    data = output.data();
                    decode_length = take;
                    outcome = "inflated (stream piece)";
                    InterlockedIncrement(&requests_stream_pieces);
                    direct_decoded = true;
                }
            }
            if (!direct_decoded) {
                outcome = "zlib failed";
                data = nullptr;
                static LONG diagnosed;
                if (x2w_local::first_calls(diagnosed, 12)) {
                    unsigned h[4] = {};
                    for (unsigned i = 0; i < 4 && i < source_size; ++i) h[i] = input[i];
                    log("x2w-dstorage zlib failure: header %02x %02x %02x %02x (dest 0x%x, source 0x%x)",
                        h[0], h[1], h[2], h[3], destination_size, source_size);
                }
            }
        }
        bool bcpack_decoded = interval_request && data;
        if (data && bcpack && !interval_request) {
            // BC3/BC4/BC5 (3-5) decode since 42cd462f; a failure there zero-fills as before (counted as
            // bcpack-blank), because a failed request has crashed the title and a blank texture has not.
            // BC6H (6) only with X2W_BC6H_FALLBACK=1; a failure zero-fills like BC3-5.
            const bool bc6h = bcpack == 6 && bc6h_fallback_enabled();
            const bool bcn_fallback = (bcpack >= 3 && bcpack <= 5) || bc6h;
            std::vector<unsigned char> decoded;
            std::size_t produced = 0;
            const bool tried = bcpack == 7 || bcpack == 1 || bcn_fallback;
            const bool ok = tried && (decoded.resize(destination_size),
                bc6h ? decode_bc6h_fallback(data, decode_length, decoded.data(), decoded.size(), &produced)
                     : x2w_bcpack::decode_bcpack(data, decode_length, bcpack, decoded.data(), decoded.size(), &produced));
            if (ok) {
                output = std::move(decoded);
                data = output.data();
                decode_length = produced;
                outcome = zlib ? "inflated + bcpack decoded" : "bcpack decoded";
                bcpack_decoded = true;
            } else if (tried && !bcn_fallback) {
                outcome = "bcpack failed";
                data = nullptr;
            } else {
                if (bcn_fallback) {
                    static LONG bcn_failed;
                    if (x2w_local::first_calls(bcn_failed, 16))
                        log("x2w-dstorage BCPack format %u decode failed: zero-filled as before (dest_size=0x%x offset=0x%llx size=0x%x)",
                            bcpack, destination_size, offset, source_size);
                }
                // shortcut for formats not decoded yet: texels are left zero (a blank texture) so the
                // title's loading continues. Counted as bcpack_blank.
                output.assign(destination_size, 0);
                data = output.data();
                decode_length = destination_size;
                outcome = "BCPack not decoded: zero-filled";
                static LONG blank_logged;
                if (x2w_local::first_calls(blank_logged, 16))
                    log("x2w-dstorage BCPack format %u not decoded: zero-filled (options=0x%llx dest_size=0x%x offset=0x%llx size=0x%x)",
                        bcpack, options, destination_size, offset, source_size);
            }
        }
        if (data) {
            // Track decode length and destination capacity as separate values (Refs #1144).
            // A short destination or missing/insufficient page grant must fail the request
            // and leave the destination memory completely unchanged.
            if (decode_length > destination_capacity) {
                outcome = "destination capacity exceeded";
                data = nullptr;
            } else if (page_destination) {
                const ULONG_PTR needed_pages = (page_offset + decode_length + 0xFFFF) / 0x10000;
                const ULONG_PTR granted_pages = (page_offset + destination_capacity + 0xFFFF) / 0x10000;
                if (needed_pages > granted_pages) {
                    outcome = "insufficient page grants";
                    data = nullptr;
                } else if (!tokens) {
                    outcome = "missing page tokens";
                    data = nullptr;
                } else {
                    for (ULONG_PTR i = 0; i < needed_pages; ++i) {
                        if (!tokens[i]) {
                            outcome = "missing page grant token";
                            data = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        if (data) {
            written = true;
            if (page_destination) {
                using WritePhysical = BOOL(WINAPI*)(const ULONG_PTR*, ULONG_PTR, SIZE_T, const void*, SIZE_T);
                static const WritePhysical write_physical = [] {
                    const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
                    return xmem ? reinterpret_cast<WritePhysical>(GetProcAddress(xmem, "x2w_xmem_write_physical")) : nullptr;
                }();
                const ULONG_PTR pages = (page_offset + decode_length + 0xFFFF) / 0x10000;
                written = write_physical && write_physical(tokens, pages, page_offset, data, decode_length);
                if (!written) {
                    outcome = "page destination write failed";
                    data = nullptr;
                }
            } else if (data != destination) {
                // The title's destination can be smaller in memory than its declared capacity (#870:
                // a write fault inside this copy). Judge the whole declared capacity before the first
                // byte is written, so a refusal leaves every page untouched.
                if (destination_writable(destination, destination_capacity)) {
                    std::memcpy(destination, data, decode_length);
                } else {
                    outcome = "destination not writable";
                    data = nullptr;
                    written = false;
                }
            }
            if (written) {
                if (bcpack && !bcpack_decoded) {
                    InterlockedIncrement(&requests_bcpack_blank);
                    InterlockedIncrement(&bcpack_blank[bcpack]);
                } else if (zlib || bcpack) {
                    InterlockedIncrement(&requests_compressed);
                } else {
                    InterlockedIncrement(&requests_done);
                }
            }
        }
    }
    const bool failed = (data == nullptr || !written) && std::strcmp(outcome, "empty") != 0;
    if (failed) InterlockedIncrement(&requests_failed);
    if (failed) tally_failure(outcome);
    if (failed && self) InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
    static LONG summaries;
    if (x2w_local::first_calls(summaries, 12))
        log("x2w-dstorage   options=0x%llx dest=%p dest_size=0x%x source=0x%llx offset=0x%llx size=0x%x -> %s", options,
            static_cast<void*>(destination), destination_size, source, offset, source_size, outcome);
    if (failed) {
        // LOCAL: every failure, one line, with the source file's path, for offline analysis.
        wchar_t path[300] = L"memory";
        LARGE_INTEGER file_size{};
        file_size.QuadPart = -1;
        auto* file_object = reinterpret_cast<Object*>(source);
        if (!(options & 0x400000) && file_object && x2w_local::readable(file_object, sizeof(Object)) && file_object->kind == file &&
            file_object->handle != INVALID_HANDLE_VALUE) {
            if (!GetFinalPathNameByHandleW(file_object->handle, path, 300, FILE_NAME_NORMALIZED)) std::wcscpy(path, L"?");
            if (!GetFileSizeEx(file_object->handle, &file_size)) file_size.QuadPart = -1;
        }
        const u64* words = reinterpret_cast<const u64*>(request);
        // The whole 0x48-byte request as enqueued, so a refused request can be compared field by field
        // with served ones of the same options (the two garbage-size requests of every run).
        char bytes[3 * 0x48 + 1] = "";
        for (int i = 0; i < 0x48; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", request[i]);
        log("x2w-dstorage FAIL options=0x%llx dest_size=0x%x offset=0x%llx size=0x%x outcome=\"%s\" file=\"%ls\" file_size=0x%llx +10=0x%llx +38=0x%llx +40=0x%llx bytes=[%s]", options,
            destination_size, offset, source_size, outcome, path, static_cast<unsigned long long>(file_size.QuadPart), words[2], words[7], words[8], bytes);
        if (self && self->work && self->work->served_any &&
            *reinterpret_cast<const u64*>(self->work->last_served) == options) {
            for (int i = 0; i < 0x48; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", self->work->last_served[i]);
            log("x2w-dstorage   the queue's latest served request with the same options: [%s]", bytes);
        }
    } else if (self && self->work && std::strcmp(outcome, "empty") != 0) {
        std::memcpy(self->work->last_served, request, sizeof self->work->last_served);
        self->work->served_any = true;
    }
    static LONG slices_logged;
    if (interval_request && slice.at && x2w_local::first_calls(slices_logged, 32))
        log("x2w-dstorage RDS5 slice options=0x%llx dest_size=0x%x offset=0x%llx size=0x%x: interval 0x%llx decoded 0x%zx at 0x%zx +10=0x%llx -> %s",
            options, destination_size, offset, source_size, slice.stored, slice.decoded_size, slice.at,
            *reinterpret_cast<const u64*>(request + 0x10), outcome);
}

void signal_fence(void* fence, u64 value);

void run_entry(Object* self, Entry& entry)
{
    if (entry.type == Entry::request) {
        const u64 options = *reinterpret_cast<const u64*>(entry.bytes);
        if ((options & (1ull << 21)) && entry.token_count) {
            const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(entry.bytes + 0x08);
            // The title's page array is read again in full when the request runs: entries are filled,
            // and also replaced, after enqueue. Writing through a token captured at enqueue put data
            // into a page the title had already reassigned. The caller has to keep the array readable
            // until the request runs; if it is not, the request fails before any page is written, so
            // no token captured at enqueue is ever used.
            const std::size_t bytes = entry.token_count * sizeof(ULONG_PTR);
            if (!title || !x2w_local::readable(title, bytes)) {
                InterlockedIncrement(&requests_failed);
                tally_failure("page array unreadable");
                if (self) InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
                static LONG unreadable;
                if (x2w_local::first_calls(unreadable, 12))
                    log("x2w-dstorage   options=0x%llx page array %p (%zu entries) unreadable when the request runs -> failed", options,
                        static_cast<const void*>(title), entry.token_count);
                return;
            }
            // Overwrite in place: the queue entry is kept free of allocations on this path.
            for (std::size_t i = 0; i < entry.token_count; ++i) {
                const ULONG_PTR now = title[i];
                if (entry.tokens[i] && now != entry.tokens[i]) InterlockedIncrement(&tokens_replaced_after_enqueue);
                if (!now) InterlockedIncrement(&zero_tokens_at_run);
                entry.tokens[i] = now;
            }
        }
        execute_request(self, entry.bytes, entry.token_count ? entry.tokens.get() : nullptr, entry.id, entry.submission_id);
    } else if (entry.type == Entry::status_entry) {
        const LONG result = InterlockedExchange(&self->pending, S_OK);
        x2w_dstorage_stats::on_complete(entry.array->stats, entry.index, entry.status_gen);
        InterlockedExchange(&entry.array->results[entry.index], result);
    } else {
        signal_fence(entry.fence, entry.value);
    }
}

// Stack committed up front for a queue worker. The thread decodes request buffers sized from request
// fields, and when one allocation fails it also runs the exception unwinder and the failure log. Once
// the system is out of commit a stack page that was never touched cannot be committed either, so the
// stack is committed at creation (CreateThread without STACK_SIZE_PARAMS_ARE_A_RESERVATION treats the
// size as the committed size; the reserved size stays the image default or larger).
constexpr SIZE_T worker_stack_commit = 1u << 20;

// Failure text is held in caller-provided storage: nothing here allocates.
void copy_reason(char (&reason)[96], const char* text) noexcept
{
    std::size_t i = 0;
    for (; text && text[i] && i + 1 < sizeof reason; ++i) reason[i] = text[i];
    reason[i] = 0;
}

// Fails the entry that threw: the next status entry reports E_FAIL, the worker goes on with the queue.
void fail_entry(Object* self, const Entry& entry, const char* reason) noexcept
{
    InterlockedIncrement(&requests_failed);
    tally_failure("exception");  // LOCAL: heartbeat tally (a literal: the tally keeps the pointer)
    InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
    static LONG reported;
    if (x2w_local::first_calls(reported, 12))
        log("x2w-dstorage entry type %d failed: %s (options=0x%llx)", static_cast<int>(entry.type), reason,
            entry.type == Entry::request ? *reinterpret_cast<const unsigned long long*>(entry.bytes) : 0ull);
}

thread_local bool on_queue_worker = false;  // Submit from a worker must not wait for itself

void complete_deferred(Object* self, QueueWork* work);

bool deferred_pending(const QueueWork& work);

DWORD WINAPI queue_worker(void* parameter)
{
    on_queue_worker = true;
    auto* const self = static_cast<Object*>(parameter);
    QueueWork& work = *self->work;
    for (;;) {
        Entry entry;
        {
            std::unique_lock lock(work.mutex);
            work.wake.wait(lock, [&] { return !work.submitted.empty() && !deferred_pending(work); });
            entry = std::move(work.submitted.front());
            work.submitted.pop_front();
        }
        // Buffers are sized from request fields; a failed allocation fails that request instead of
        // ending the process (an exception leaving a thread calls terminate). The handlers only note
        // the reason in a fixed buffer: the request's buffers are already freed when a handler runs,
        // and the log is written after the handler has left, so no formatting runs while the
        // exception is in flight on this stack.
        char reason[96] = "";
        bool failed = false;
        try {
            run_entry(self, entry);
        } catch (const std::bad_alloc&) {
            failed = true;
            copy_reason(reason, "out of memory");
        } catch (const std::exception& error) {
            failed = true;
            copy_reason(reason, error.what());
        } catch (...) {
            failed = true;
            copy_reason(reason, "unknown exception");
        }
        if (failed) fail_entry(self, entry, reason);
        {
            const std::lock_guard guard(work.mutex);
            if (work.submitted.empty()) {
                work.cached_streams.clear();
            }
            ++work.finished_total;
        }
        work.drained.notify_all();
        complete_deferred(self, &work);
    }
}

// Guards queue setup: building a queue's work, starting its worker and the unrecorded list.
std::mutex& setup_mutex()
{
    static std::mutex setup;
    return setup;
}

// Builds the queue's work once. The three deques allocate in their constructors, so even
// new (std::nothrow) can throw std::bad_alloc from here: that is treated like a null result, and
// nothing escapes to the title. The failure is logged after the handler has left.
QueueWork* ensure_work(Object* self)
{
    const std::lock_guard guard(setup_mutex());
    if (self->work) return self->work;
    QueueWork* work = nullptr;
    try {
        work = new (std::nothrow) QueueWork;
    } catch (const std::bad_alloc&) {
        work = nullptr;
    }
    if (!work) {
        log("x2w-dstorage queue work allocation failed");
        return nullptr;
    }
    self->work = work;
    return work;
}

// Starts the queue's worker unless it runs. The worker is created suspended with a committed stack,
// then once more with the default stack; false when neither thread could be created, which leaves
// the queue as it was so a later call tries again.
bool start_worker(Object* self, QueueWork* work)
{
    const std::lock_guard guard(setup_mutex());
    if (work->worker_started) return true;
    HANDLE worker = CreateThread(nullptr, worker_stack_commit, queue_worker, self, CREATE_SUSPENDED, nullptr);
    if (!worker) worker = CreateThread(nullptr, 0, queue_worker, self, CREATE_SUSPENDED, nullptr);
    if (!worker) {
        log("x2w-dstorage queue worker not started: error %lu", GetLastError());
        return false;
    }
    work->worker_started = true;
    ResumeThread(worker);
    CloseHandle(worker);
    return true;
}

// What an entry that could not be stored needs in order to fail at Submit: no request bytes, since a
// request only counts as failed.
struct Unrecorded {
    Unrecorded* next;
    u64 submission_id;
    u64 after;
    Entry::Type type;
    Object* array;
    unsigned index;
    void* fence;
    u64 value;
    LONG status_gen;
};

// True while snapshot entries of an earlier Submit are ready or being completed by some thread. The
// worker starts no newer entry then, so a deferred completion and a normal one keep the same order.
// The caller holds work.mutex.
bool deferred_pending(const QueueWork& work)
{
    if (work.completing) return true;
    if (!work.failing.empty() && work.failing.front().after <= work.finished_total) return true;
    return work.deferred_head && work.deferred_head->after <= work.finished_total;
}

// Fails an entry that will not run: a request counts as failed, a status entry completes with the
// pending failure (E_OUTOFMEMORY when nothing worse is pending), a fence is signaled.
void fail_unstarted(Object* self, Entry::Type type, Object* array, unsigned index, void* fence, u64 value, LONG status_gen)
{
    if (type == Entry::request) {
        InterlockedIncrement(&requests_failed);
        InterlockedCompareExchange(&self->pending, static_cast<LONG>(E_OUTOFMEMORY), S_OK);
    } else if (type == Entry::status_entry) {
        const LONG pending = InterlockedExchange(&self->pending, S_OK);
        if (array && array->results && index < array->capacity) {
            x2w_dstorage_stats::on_complete(array->stats, index, status_gen);
            InterlockedExchange(&array->results[index],
                                pending < 0 ? pending : static_cast<LONG>(E_OUTOFMEMORY));
        }
    } else {
        signal_fence(fence, value);
    }
}

// An entry the queue has no room for (its work could not be built or grown): remembered in a small
// record that needs no queue storage, failed by the next Submit. Only when even that record cannot be
// allocated is the entry failed here, because nothing is left to remember it with (a documented last
// resort: that entry may then be failed ahead of the ones stored before it).
//
// Order. One accumulator (Object::pending) holds the worst failure since the previous status entry,
// and a status entry consumes it, so an entry that lost its place in the queue order would leak a
// failure past the status entry it belongs to. Therefore, once an entry of this queue has been
// deferred, every later entry until the next Submit is deferred too, even if it could be stored:
// the stored entries (`waiting`) are an in-order prefix and the deferred ones an in-order suffix.
// Submit handles the prefix first (moves it to the worker, or fails it), then fails the suffix.
// Submit snapshots the failing entries at its cutoff and the worker completes that snapshot right after
// it finishes the prefix, so suffix completions are visible in queue order and see the prefix's failures,
// a later batch cannot be completed by this Submit, and Submit never waits for the prefix. After the first allocation failure the rest of the batch fails with E_OUTOFMEMORY even if
// memory has recovered, which is acceptable under out-of-memory and preferable to reordering.
bool has_unrecorded(Object* self)
{
    const std::lock_guard guard(setup_mutex());
    return self->unrecorded_head != nullptr;
}

void defer_unrecorded(Object* self, const Entry& entry)
{
    auto* node = new (std::nothrow) Unrecorded{nullptr, 0, 0, entry.type, entry.array, entry.index, entry.fence, entry.value, entry.status_gen};
    if (!node) {
        fail_unstarted(self, entry.type, entry.array, entry.index, entry.fence, entry.value, entry.status_gen);
        return;
    }
    const std::lock_guard guard(setup_mutex());
    (self->unrecorded_tail ? self->unrecorded_tail->next : self->unrecorded_head) = node;
    self->unrecorded_tail = node;
}

void fail_unrecorded(Object* self)
{
    Unrecorded* node;
    {
        const std::lock_guard guard(setup_mutex());
        node = self->unrecorded_head;
        self->unrecorded_head = self->unrecorded_tail = nullptr;
    }
    while (node) {
        Unrecorded* const next = node->next;
        fail_unstarted(self, node->type, node->array, node->index, node->fence, node->value, node->status_gen);
        delete node;
        node = next;
    }
}

// Fails every entry still waiting, in order; none of them will run.
void fail_waiting(Object* self, QueueWork* work)
{
    for (;;) {
        Entry entry{};
        {
            const std::lock_guard guard(work->mutex);
            if (work->waiting.empty()) break;
            entry = std::move(work->waiting.front());
            work->waiting.pop_front();
        }
        fail_unstarted(self, entry.type, entry.array, entry.index, entry.fence, entry.value, entry.status_gen);
    }
}

// Submit: waiting entries (the in-order prefix) go to the worker, which is started if it is not
// running yet (resources may have recovered since Enqueue). If it still cannot be started, or the
// worker's list cannot grow, the waiting entries fail. Only then do the entries that could not be
// stored (the in-order suffix, see defer_unrecorded) fail, so every failure reaches the status entry
// after it, in order. Their status entries and fences complete here, not earlier in Enqueue. No
// exception leaves Submit.
void submit(Object* self)
{
    QueueWork* work = ensure_work(self);
    if (!work) {
        fail_unrecorded(self);
        return;
    }
    if (!start_worker(self, work)) {
        fail_waiting(self, work);
        fail_unrecorded(self);
        return;
    }
    bool all_moved = true;
    bool snapshot_ok = true;
    bool snapshotted = false;
    u64 prefix_end = 0;
    {
        const std::lock_guard guard(work->mutex);
        const u64 submission_id = ++work->next_submission_id;
        try {
            // An entry leaves `waiting` only after it is in `submitted`, so a failed growth loses none.
            while (!work->waiting.empty()) {
                Entry& entry = work->waiting.front();
                entry.submission_id = submission_id;
                work->submitted.push_back(std::move(entry));
                ++work->submitted_total;
                work->waiting.pop_front();
            }
        } catch (const std::bad_alloc&) {
            all_moved = false;
        }
        prefix_end = work->submitted_total;
        // The cutoff: the entries that fail are snapshotted here, under the same lock, so a batch enqueued
        // later cannot be completed by this Submit. The snapshot completes on the worker right after it
        // finishes the prefix (see complete_deferred), so this call never waits for the prefix.
        try {
            while (!work->waiting.empty()) {
                Entry& entry = work->waiting.front();
                entry.submission_id = submission_id;
                entry.after = prefix_end;
                work->failing.push_back(std::move(entry));
                work->waiting.pop_front();
                snapshotted = true;
            }
        } catch (const std::bad_alloc&) {
            snapshot_ok = false;
        }
        if (snapshot_ok) {
            Unrecorded* node;
            {
                const std::lock_guard setup(setup_mutex());
                node = self->unrecorded_head;
                self->unrecorded_head = self->unrecorded_tail = nullptr;
            }
            for (Unrecorded* n = node; n; n = n->next) {
                n->submission_id = submission_id;
                n->after = prefix_end;
                if (!n->next) {
                    (work->deferred_tail ? work->deferred_tail->next : work->deferred_head) = node;
                    work->deferred_tail = n;
                }
                snapshotted = true;
            }
        }
    }
    work->wake.notify_one();
    if (!snapshot_ok) {
        // Last resort when even the snapshot could not grow: wait for the prefix as before, then fail in order.
        if (!on_queue_worker) {
            std::unique_lock lock(work->mutex);
            work->drained.wait(lock, [&] { return work->finished_total >= prefix_end; });
        }
        complete_deferred(self, work);
        fail_waiting(self, work);
        fail_unrecorded(self);
        return;
    }
    if (snapshotted) complete_deferred(self, work);
}

// Completes the snapshot entries (failed entries of earlier Submits) whose prefix the worker has finished,
// in Submit order and, within one Submit, stored entries before unrecorded ones. Called by the worker after
// every entry and by Submit, so nothing waits on a calling thread. Nothing allocates.
void complete_deferred(Object* self, QueueWork* work)
{
    // One owner completes at a time, but no lock is held while it does: a fence is a caller-supplied
    // operation. A thread that finds an owner returns at once; the owner takes what it queued before
    // it gives up ownership, so nothing is left behind and no Submit waits behind a fence.
    {
        const std::lock_guard guard(work->mutex);
        if (work->completing) return;
        work->completing = true;
    }
    for (;;) {
        Entry entry{};
        Unrecorded* node = nullptr;
        {
            const std::lock_guard guard(work->mutex);
            bool take_entry = !work->failing.empty() && work->failing.front().after <= work->finished_total;
            bool take_node = work->deferred_head && work->deferred_head->after <= work->finished_total;
            if (take_entry && take_node) {
                if (work->failing.front().submission_id <= work->deferred_head->submission_id) take_node = false;
                else take_entry = false;
            }
            if (take_entry) {
                entry = std::move(work->failing.front());
                work->failing.pop_front();
            } else if (take_node) {
                node = work->deferred_head;
                work->deferred_head = node->next;
                if (!work->deferred_head) work->deferred_tail = nullptr;
            } else {
                work->completing = false;
                work->wake.notify_all();  // the worker may be waiting for the owner to finish
                return;
            }
        }
        if (node) {
            fail_unstarted(self, node->type, node->array, node->index, node->fence, node->value, node->status_gen);
            delete node;
        } else {
            fail_unstarted(self, entry.type, entry.array, entry.index, entry.fence, entry.value, entry.status_gen);
        }
    }
}

// Enqueue only records the entry, whether or not a worker runs; nothing completes before Submit.
void push(Object* self, Entry&& entry)
{
    QueueWork* work = ensure_work(self);
    if (!work || has_unrecorded(self)) {  // after a deferred entry every later one is deferred: order
        defer_unrecorded(self, entry);
        return;
    }
    start_worker(self, work);  // a failure shows at Submit
    bool recorded = false;
    bool full = false;
    try {
        const std::lock_guard guard(work->mutex);
        entry.id = ++work->next_entry_id;
        work->waiting.push_back(std::move(entry));
        recorded = true;
        full = self->capacity && work->waiting.size() >= self->capacity;
    } catch (const std::bad_alloc&) {
    }
    if (!recorded) {
        defer_unrecorded(self, entry);
        return;
    }
    if (full) submit(self);  // a full queue submits itself, as on PC
}

// Queue slot 3, EnqueueRequest(request*): copied now, run after Submit.
void __stdcall queue_enqueue_request(Object* self, const unsigned char* request)
{
    if (!request) return;
    Entry entry{};
    entry.type = Entry::request;
    std::memcpy(entry.bytes, request, sizeof entry.bytes);
    const u64 options = *reinterpret_cast<const u64*>(request);
    if (options & (1ull << 21)) {
        const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(request + 0x08);
        const unsigned destination_size = *reinterpret_cast<const unsigned*>(request + 0x18);
        const unsigned page_offset = static_cast<unsigned>(*reinterpret_cast<const u64*>(request + 0x10) & 0xFFFF);
        const std::size_t pages = (std::size_t{page_offset} + destination_size + 0xFFFF) / 0x10000;
        if (title && pages && x2w_local::readable(title, pages * sizeof(ULONG_PTR))) {
            entry.tokens.reset(new (std::nothrow) ULONG_PTR[pages]);
            if (!entry.tokens) {
                defer_unrecorded(self, entry);  // fails at Submit; nothing leaves Enqueue
                return;
            }
            entry.token_count = pages;
            for (std::size_t i = 0; i < pages; ++i) {
                entry.tokens[i] = title[i];
                if (!title[i]) InterlockedIncrement(&zero_tokens_at_enqueue);
            }
        }
    }
    push(self, std::move(entry));
}

// Queue slot 4, EnqueueStatus(statusArray, index): completes, in order, with the worst result of the
// requests since the previous status.
void __stdcall queue_enqueue_status(Object* self, Object* array, unsigned index)
{
    const bool valid = array && array->kind == status && index < array->capacity;
    static LONG calls;
    if (!valid || x2w_local::first_calls(calls, 8))
        log("x2w-dstorage Queue::EnqueueStatus array=%p index=%u%s (requests done=%ld inflated=%ld failed=%ld)",
            static_cast<void*>(array), index, valid ? "" : " INVALID ARRAY", requests_done, requests_compressed, requests_failed);
    if (!valid) return;
    const LONG gen = x2w_dstorage_stats::on_enqueue(array->stats, index, self);
    InterlockedExchange(&array->results[index], static_cast<LONG>(0x8000000A));  // E_PENDING until its turn
    Entry entry{};
    entry.type = Entry::status_entry;
    entry.status_gen = gen;
    entry.array = array;
    entry.index = index;
    push(self, std::move(entry));
}

// The remaining queue and status-array slots follow the public PC IDStorageQueue / IDStorageStatusArray
// order, which slots 3, 4 and 6 matched; each logs its first calls with its arguments.
// Queue slot 5, EnqueueSignal(fence, value): requests are already complete, so the fence is signaled
// now through its own vtable (ID3D12Fence::Signal, slot 10).
void __stdcall queue_enqueue_signal(Object* self, void* fence, u64 value)
{
    Entry entry{};
    entry.type = Entry::signal;
    entry.fence = fence;
    entry.value = value;
    push(self, std::move(entry));
}
void signal_fence(void* fence, u64 value)
{
    long hr = E_POINTER;
    if (fence) {
        using Signal = long(__stdcall*)(void*, u64);
        hr = reinterpret_cast<Signal>((*static_cast<void***>(fence))[10])(fence, value);
    }
    static LONG calls;
    if (x2w_local::first_calls(calls, 4))
        log("x2w-dstorage Queue::EnqueueSignal fence=%p value=0x%llx -> 0x%08lx", fence, value, static_cast<unsigned long>(hr));
}
void __stdcall queue_submit(Object* self, u64 rdx, u64 r8)
{
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) log("x2w-dstorage Queue::Submit rdx=0x%llx r8=0x%llx", rdx, r8);
    submit(self);
}
void __stdcall queue_cancel(Object*, u64 mask, u64 value)
{
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) log("x2w-dstorage Queue::CancelRequestsWithTag mask=0x%llx value=0x%llx (no-op)", mask, value);
}
void __stdcall queue_close(Object*) { log("x2w-dstorage Queue::Close"); }
HANDLE __stdcall queue_error_event(Object*)
{
    static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) log("x2w-dstorage Queue::GetErrorEvent -> %p (never signaled)", event);
    return event;
}
// Status array slot 3 IsComplete(index), slot 4 GetHResult(index).
bool __stdcall status_is_complete(Object* self, unsigned index)
{
    const bool complete = index < self->capacity && self->results[index] != static_cast<LONG>(0x8000000A);
    x2w_dstorage_stats::on_poll(self->stats, complete, _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) log("x2w-dstorage StatusArray::IsComplete index=%u -> %d", index, complete);
    return complete;
}
long __stdcall status_hresult(Object* self, unsigned index)
{
    const long hr = index < self->capacity ? self->results[index] : E_INVALIDARG;
    static LONG calls;
    if (x2w_local::first_calls(calls, 4)) log("x2w-dstorage StatusArray::GetHResult index=%u -> 0x%08lx", index, static_cast<unsigned long>(hr));
    return hr;
}

void** make_table(unsigned kind)
{
    static void* tables[4][slots];
    static bool ready[4];
    const unsigned index = kind / 0x20 - 1;
    void** table = tables[index];
    if (!ready[index]) {
        fill_traps(table, std::make_index_sequence<slots>{});
        table[0] = reinterpret_cast<void*>(&query_interface);
        table[1] = reinterpret_cast<void*>(&add_ref);
        table[2] = reinterpret_cast<void*>(&release);
        if (kind == factory) {
            table[4] = reinterpret_cast<void*>(&factory_open_file);
            table[5] = reinterpret_cast<void*>(&factory_create_status_array);
            table[8] = reinterpret_cast<void*>(&factory_slot8);
            table[10] = reinterpret_cast<void*>(&factory_create_queue);
        } else if (kind == queue) {
            table[3] = reinterpret_cast<void*>(&queue_enqueue_request);
            table[4] = reinterpret_cast<void*>(&queue_enqueue_status);
            table[5] = reinterpret_cast<void*>(&queue_enqueue_signal);
            table[6] = reinterpret_cast<void*>(&queue_submit);
            table[7] = reinterpret_cast<void*>(&queue_cancel);
            table[8] = reinterpret_cast<void*>(&queue_close);
            table[9] = reinterpret_cast<void*>(&queue_error_event);
        } else if (kind == status) {
            table[3] = reinterpret_cast<void*>(&status_is_complete);
            table[4] = reinterpret_cast<void*>(&status_hresult);
        } else if (kind == file) {
            table[3] = reinterpret_cast<void*>(&file_close);
            table[4] = reinterpret_cast<void*>(&file_information);
        }
        ready[index] = true;
    }
    return table;
}

// What the heartbeat reports about memory (#1199): this process's commit charge and the live
// physical-page sections of the xmem stand-in, read through its probe export. The flags keep the
// line's shape when a reading is unavailable.
struct MemorySample {
    ULONGLONG peak_commit = 0, commit = 0, sections = 0;
    bool commit_known = false, sections_known = false;
};

MemorySample memory_sample() noexcept
{
    MemorySample sample;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        sample.commit = counters.PagefileUsage;
        sample.peak_commit = counters.PeakPagefileUsage;
        sample.commit_known = true;
    }
    using PageStats = BOOL(WINAPI*)(ULONGLONG*);
    HMODULE xmem = nullptr;
    if (GetModuleHandleExW(0, L"xmem.dll", &xmem)) {
        const auto page_stats = reinterpret_cast<PageStats>(GetProcAddress(xmem, "x2w_xmem_page_stats"));
        sample.sections_known = page_stats && page_stats(&sample.sections);
        FreeLibrary(xmem);
    }
    return sample;
}

}  // namespace

extern "C" long __stdcall DStorageGetFactory(const GUID* riid, void** out)
{
    char id[48];
    guid_text(riid, id, sizeof(id));
    log("x2w-dstorage DStorageGetFactory riid=%s", id);
    if (!out) return E_POINTER;
    static const bool bcpack_summary = [] {
        std::atexit(log_bcpack_summary);
        return true;
    }();
    (void)bcpack_summary;
    // Heartbeat: the request counters every 10 s, so a run shows whether loading still progresses
    // after the per-call logs have gone quiet. Peak commit and the live physical-page sections show
    // what the run spends (#1199), including while requests are stalled.
    static HANDLE heartbeat = CreateThread(nullptr, 0, [](void*) -> DWORD {
        LONG last = -1;
        MemorySample last_memory;
        for (int second = 10;; second += 10) {
            Sleep(10000);
            const LONG total = requests_done + requests_compressed + requests_failed;
            const LONG total2 = total + requests_bcpack_blank;
            const MemorySample sample = memory_sample();
            const ULONGLONG peak_mib = sample.peak_commit >> 20;
            const ULONGLONG current_mib = sample.commit >> 20;
            if (total2 != last || peak_mib != (last_memory.peak_commit >> 20) ||
                current_mib != (last_memory.commit >> 20) || sample.sections != last_memory.sections ||
                sample.commit_known != last_memory.commit_known || sample.sections_known != last_memory.sections_known) {
                char memory[96];
                if (sample.commit_known)
                    std::snprintf(memory, sizeof(memory), "commit peak=%llu MiB current=%llu MiB",
                                  static_cast<unsigned long long>(peak_mib),
                                  static_cast<unsigned long long>(current_mib));
                else
                    std::snprintf(memory, sizeof(memory), "commit unknown");
                char section_text[24];
                if (sample.sections_known)
                    std::snprintf(section_text, sizeof(section_text), "%llu",
                                  static_cast<unsigned long long>(sample.sections));
                else
                    std::snprintf(section_text, sizeof(section_text), "unknown");
                // Heartbeat preserves "inflated=%ld" format string for tools/startup/live_view.py;
                // requests_compressed counts all compressed streams (zlib and standalone/embedded BCPack).
                log("x2w-dstorage heartbeat %d s: requests done=%ld inflated=%ld failed=%ld bcpack-blank=%ld archive-shifted=%ld past-end=%ld stream-pieces=%ld found-on-disk=%ld rds5-slices=%ld; page tokens 0 at enqueue=%ld, "
                    "still 0 at run=%ld, replaced after enqueue=%ld; %s, live physical-page sections=%s", second, requests_done, requests_compressed,
                    requests_failed, requests_bcpack_blank, requests_archive_shifted, requests_archive_past_end, requests_stream_pieces, requests_pieces_found_on_disk, requests_interval_slices, zero_tokens_at_enqueue, zero_tokens_at_run, tokens_replaced_after_enqueue, memory,
                    section_text);
                char tally[512];
                int at = 0;
                tally[0] = 0;
                AcquireSRWLockShared(&failure_lock);
                for (const auto& t : failure_tally) {
                    if (!t.outcome || at >= static_cast<int>(sizeof(tally)) - 1) break;
                    at += std::snprintf(tally + at, sizeof(tally) - at, " [%s]=%ld", t.outcome, t.count);
                }
                ReleaseSRWLockShared(&failure_lock);
                if (tally[0]) log("x2w-dstorage failures by outcome:%s", tally);
                last = total2;
                last_memory = sample;
            }
            x2w_dstorage_stats::report(second, static_cast<LONG>(0x8000000A), [](const char* format, auto... args) { log(format, args...); }, [](void* queue, char* out, std::size_t size) {
                auto* const q = static_cast<Object*>(queue);
                QueueWork* const work = q ? q->work : nullptr;
                if (!work) {
                    std::snprintf(out, size, "queue %p: no work", queue);
                    return;
                }
                const std::lock_guard guard(work->mutex);
                std::snprintf(out, size, "queue %p: entries submitted %llu finished %llu, %zu to run, %zu waiting for Submit, %zu failing", queue,
                              static_cast<unsigned long long>(work->submitted_total), static_cast<unsigned long long>(work->finished_total),
                              work->submitted.size(), work->waiting.size(), work->failing.size());
            });
        }
    }, nullptr, 0, nullptr);
    (void)heartbeat;
    static Object* instance = make(factory);
    InterlockedIncrement(&instance->refs);
    *out = instance;
    return S_OK;
}
