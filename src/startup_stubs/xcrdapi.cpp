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
#include <algorithm>
#include <memory>
#include <map>

#include <psapi.h>

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdarg>
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
};

// A queue entry as enqueued: a request (its 0x48 bytes copied, and a snapshot of its destination page
// tokens), a status entry or a fence signal. Entries run in order on the queue's worker after Submit,
// like DirectStorage; a destination page token that was 0 at enqueue is read again at the entry's turn
// (titles fill those in after EnqueueRequest).
struct Entry {
    enum Type { request, status_entry, signal } type;
    unsigned char bytes[0x48];
    std::vector<ULONG_PTR> tokens;
    Object* array;
    unsigned index;
    void* fence;
    u64 value;
    u64 id = 0;
    u64 submission_id = 0;
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
    u64 next_entry_id = 0;
    u64 next_submission_id = 0;
    static constexpr std::size_t max_cached_streams = 16;
    std::deque<CachedStream> cached_streams;

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
    wchar_t log_path[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"X2W_TRAP_LOG", log_path, MAX_PATH);
    if (length && length < MAX_PATH) {
        HANDLE out = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            x2w_trap::write_all(out, line, static_cast<DWORD>(n));
            CloseHandle(out);
        }
    }
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
    log("x2w-dstorage File::GetFileInformation -> %s size=0x%llx", ok ? "ok" : "fail",
        ok ? (static_cast<unsigned long long>(info->nFileSizeHigh) << 32) | info->nFileSizeLow : 0ull);
    return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// Queue slot 3, EnqueueRequest(request*) by the PC numbering. Request layout from the title's builder
// (a hypothesis from its two code paths): +0x00 options (0x400000: memory source; low byte: compression
// format, 1 seen), +0x08 destination pointer, +0x18 destination size (u32), +0x20 source (File object
// or memory pointer), +0x28 source offset, +0x30 source size (u32). Uncompressed requests are executed
// synchronously; compressed ones are logged and left undone. The first requests are logged in full.
LONG requests_done, requests_compressed, requests_failed, requests_bcpack_blank;
LONG zero_tokens_at_enqueue, zero_tokens_at_run, tokens_replaced_after_enqueue;
// Archives of one kind (an RDS5 boundary table at file offset 0) are addressed by the executable 14 x 64 KiB
// after where their data sits on disk (measured on one executable: every shifted zlib stream starts exactly
// at a table boundary 0xE0000 before the requested offset, and pieces in the last 0xE0000 bytes point past
// the end of the file). Why the executable adds the shift is not known. A request is read from the shifted
// offset when the table has a boundary there and none at the requested offset, or when only the shifted
// range fits in the file. LOCAL SHORTCUT until the offset arithmetic is understood.
struct Rds5Boundary { u64 stored, logical; unsigned codec; };
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
        slot->push_back({u64{low & 0x03ffffffu} * 256, (u64{high & 0xfffffu} * 4 + (low >> 30)) * 4096, (low >> 26) & 15});
    }
    return slot.get();
}
LONG requests_archive_shifted, requests_archive_past_end;
u64 archive_read_offset(HANDLE handle, u64 offset, unsigned size)
{
    constexpr u64 shift = 14 * 0x10000;
    if (offset < shift) return offset;
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

// LOCAL: multi-piece zlib streams across queues and submissions (measured on one executable). One
// stream that inflates to several destination-sized pieces is enqueued as one request per piece;
// every piece names the same compressed size and piece k's offset is the stream's offset plus k
// times the destination size. The continuation cache above only covers pieces queued in the same
// submission on the same queue. Pieces that miss it are served from a shared cache of recently
// inflated streams, or by finding the stream start on disk (offset - k * piece, a zlib header, and a
// full Adler-checked inflate past piece k).
LONG requests_stream_pieces, requests_pieces_found_on_disk;
struct InflatedStream { u64 source, base; unsigned compressed; std::shared_ptr<const std::vector<unsigned char>> data; };
std::mutex streams_lock;
std::deque<InflatedStream> streams;
void remember_stream(u64 source, u64 base, unsigned compressed, std::shared_ptr<const std::vector<unsigned char>> data)
{
    const std::lock_guard guard(streams_lock);
    streams.push_front({source, base, compressed, std::move(data)});
    if (streams.size() > 16) streams.pop_back();
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
    if (InterlockedIncrement(&logged) <= 12) {
        char bytes[3 * 0x40 + 1] = "";
        for (int i = 0; i < 0x40; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", request[i]);
        log("x2w-dstorage Queue::EnqueueRequest [%s]", bytes);
    }
    // Option bits (from the reference projects' notes, matched against this title's requests): bit 0
    // zlib, bits 1-4 a BCPack BC format number, bit 21 a destination given as 64 KiB physical page
    // tokens (+0x10 low 16 bits: offset into the first page), bit 22 a memory source.
    const bool zlib = (options & 1) != 0;
    const unsigned bcpack = static_cast<unsigned>((options >> 1) & 0xF);
    const bool page_destination = (options & (1ull << 21)) != 0;
    const unsigned page_offset = static_cast<unsigned>(*reinterpret_cast<const u64*>(request + 0x10) & 0xFFFF);
    const u64 known = 1 | (0xFull << 1) | (1ull << 21) | 0x400000;
    const char* outcome = "done";
    std::vector<unsigned char> packed, output;
    const unsigned char* input = nullptr;
    if (options & ~known) {
        outcome = "unknown option bits, not done";
    } else if (!destination || !source_size) {
        outcome = "empty";
    } else if (memory_source) {
        input = reinterpret_cast<const unsigned char*>(source);
    } else {
        auto* file_object = reinterpret_cast<Object*>(source);
        if (file_object && file_object->kind == file &&
            x2w_startup::read_storage_file(file_object->handle, archive_read_offset(file_object->handle, offset, source_size), source_size, packed))
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
        if (zlib) {
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
                    if (!cached_chunked && !memory_source && !bcpack && produced > destination_size) {
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

            if (!direct_decoded && !memory_source && !bcpack) {
                // LOCAL: a later piece that missed the submission's cache (see remember_stream).
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
                if (InterlockedIncrement(&diagnosed) <= 12) {
                    unsigned h[4] = {};
                    for (unsigned i = 0; i < 4 && i < source_size; ++i) h[i] = input[i];
                    log("x2w-dstorage zlib failure: header %02x %02x %02x %02x (dest 0x%x, source 0x%x)",
                        h[0], h[1], h[2], h[3], destination_size, source_size);
                }
            }
        }
        bool bcpack_decoded = false;
        if (data && bcpack) {
            if (bcpack == 7 || bcpack == 1) {
                std::vector<unsigned char> decoded(destination_size);
                std::size_t produced = 0;
                if (x2w_bcpack::decode_bcpack(data, decode_length, bcpack, decoded.data(), decoded.size(), &produced)) {
                    output = std::move(decoded);
                    data = output.data();
                    decode_length = produced;
                    outcome = zlib ? "inflated + bcpack decoded" : "bcpack decoded";
                    bcpack_decoded = true;
                } else {
                    outcome = "bcpack failed";
                    data = nullptr;
                }
            } else {
                // shortcut for formats not decoded yet: texels are left zero (a blank texture) so the
                // title's loading continues. Counted as bcpack_blank.
                output.assign(destination_size, 0);
                data = output.data();
                decode_length = destination_size;
                outcome = "BCPack not decoded: zero-filled";
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
                std::memcpy(destination, data, decode_length);
            }
            if (written) {
                if (bcpack && !bcpack_decoded) {
                    InterlockedIncrement(&requests_bcpack_blank);
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
    if (failed && self) InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
    static LONG summaries, failures;
    if (InterlockedIncrement(&summaries) <= 12 || (failed && InterlockedIncrement(&failures) <= 12))
        log("x2w-dstorage   options=0x%llx dest=%p dest_size=0x%x source=0x%llx offset=0x%llx size=0x%x -> %s", options,
            static_cast<void*>(destination), destination_size, source, offset, source_size, outcome);
}

void signal_fence(void* fence, u64 value);

void run_entry(Object* self, Entry& entry)
{
    if (entry.type == Entry::request) {
        const u64 options = *reinterpret_cast<const u64*>(entry.bytes);
        if ((options & (1ull << 21)) && !entry.tokens.empty()) {
            const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(entry.bytes + 0x08);
            // The title's page array is read again in full when the request runs: entries are filled,
            // and also replaced, after enqueue. Writing through a token captured at enqueue put data
            // into a page the title had already reassigned. The caller has to keep the array readable
            // until the request runs; if it is not, the request fails before any page is written, so
            // no token captured at enqueue is ever used.
            const std::size_t bytes = entry.tokens.size() * sizeof(ULONG_PTR);
            if (!title || !x2w_local::readable(title, bytes)) {
                InterlockedIncrement(&requests_failed);
                if (self) InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
                static LONG unreadable;
                if (InterlockedIncrement(&unreadable) <= 12)
                    log("x2w-dstorage   options=0x%llx page array %p (%zu entries) unreadable when the request runs -> failed", options,
                        static_cast<const void*>(title), entry.tokens.size());
                return;
            }
            std::vector<ULONG_PTR> now(title, title + entry.tokens.size());  // one snapshot of the whole array
            for (std::size_t i = 0; i < now.size(); ++i) {
                if (entry.tokens[i] && now[i] != entry.tokens[i]) InterlockedIncrement(&tokens_replaced_after_enqueue);
                if (!now[i]) InterlockedIncrement(&zero_tokens_at_run);
            }
            entry.tokens = std::move(now);
        }
        execute_request(self, entry.bytes, entry.tokens.empty() ? nullptr : entry.tokens.data(), entry.id, entry.submission_id);
    } else if (entry.type == Entry::status_entry) {
        const LONG result = InterlockedExchange(&self->pending, S_OK);
        InterlockedExchange(&entry.array->results[entry.index], result);
    } else {
        signal_fence(entry.fence, entry.value);
    }
}

QueueWork& work_of(Object* self)
{
    static std::mutex create;
    const std::lock_guard guard(create);
    if (!self->work) {
        self->work = new QueueWork;
        std::thread([self] {
            QueueWork& work = *self->work;
            for (;;) {
                Entry entry;
                {
                    std::unique_lock lock(work.mutex);
                    work.wake.wait(lock, [&] { return !work.submitted.empty(); });
                    entry = std::move(work.submitted.front());
                    work.submitted.pop_front();
                }
                // Buffers are sized from request fields; a failed allocation fails that request
                // instead of ending the process (an exception leaving a thread calls terminate).
                try {
                    run_entry(self, entry);
                } catch (const std::exception& error) {
                    InterlockedIncrement(&requests_failed);
                    InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
                    static LONG reported;
                    if (InterlockedIncrement(&reported) <= 12)
                        log("x2w-dstorage entry type %d failed: %s (options=0x%llx)", static_cast<int>(entry.type), error.what(),
                            entry.type == Entry::request ? *reinterpret_cast<const unsigned long long*>(entry.bytes) : 0ull);
                }
                {
                    const std::lock_guard guard(work.mutex);
                    if (work.submitted.empty()) {
                        work.cached_streams.clear();
                    }
                }
            }
        }).detach();
    }
    return *self->work;
}

void submit(Object* self)
{
    QueueWork& work = work_of(self);
    {
        const std::lock_guard guard(work.mutex);
        const u64 submission_id = ++work.next_submission_id;
        for (auto& entry : work.waiting) {
            entry.submission_id = submission_id;
            work.submitted.push_back(std::move(entry));
        }
        work.waiting.clear();
    }
    work.wake.notify_one();
}

void push(Object* self, Entry&& entry)
{
    QueueWork& work = work_of(self);
    bool full;
    {
        const std::lock_guard guard(work.mutex);
        entry.id = ++work.next_entry_id;
        work.waiting.push_back(std::move(entry));
        full = self->capacity && work.waiting.size() >= self->capacity;
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
            entry.tokens.assign(title, title + pages);
            for (ULONG_PTR token : entry.tokens)
                if (!token) InterlockedIncrement(&zero_tokens_at_enqueue);
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
    if (!valid || InterlockedIncrement(&calls) <= 8)
        log("x2w-dstorage Queue::EnqueueStatus array=%p index=%u%s (requests done=%ld inflated=%ld failed=%ld)",
            static_cast<void*>(array), index, valid ? "" : " INVALID ARRAY", requests_done, requests_compressed, requests_failed);
    if (!valid) return;
    InterlockedExchange(&array->results[index], static_cast<LONG>(0x8000000A));  // E_PENDING until its turn
    Entry entry{};
    entry.type = Entry::status_entry;
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
    if (InterlockedIncrement(&calls) <= 4)
        log("x2w-dstorage Queue::EnqueueSignal fence=%p value=0x%llx -> 0x%08lx", fence, value, static_cast<unsigned long>(hr));
}
void __stdcall queue_submit(Object* self, u64 rdx, u64 r8)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 4) log("x2w-dstorage Queue::Submit rdx=0x%llx r8=0x%llx", rdx, r8);
    submit(self);
}
void __stdcall queue_cancel(Object*, u64 mask, u64 value)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 4) log("x2w-dstorage Queue::CancelRequestsWithTag mask=0x%llx value=0x%llx (no-op)", mask, value);
}
void __stdcall queue_close(Object*) { log("x2w-dstorage Queue::Close"); }
HANDLE __stdcall queue_error_event(Object*)
{
    static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 4) log("x2w-dstorage Queue::GetErrorEvent -> %p (never signaled)", event);
    return event;
}
// Status array slot 3 IsComplete(index), slot 4 GetHResult(index).
bool __stdcall status_is_complete(Object* self, unsigned index)
{
    const bool complete = index < self->capacity && self->results[index] != static_cast<LONG>(0x8000000A);
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 4) log("x2w-dstorage StatusArray::IsComplete index=%u -> %d", index, complete);
    return complete;
}
long __stdcall status_hresult(Object* self, unsigned index)
{
    const long hr = index < self->capacity ? self->results[index] : E_INVALIDARG;
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 4) log("x2w-dstorage StatusArray::GetHResult index=%u -> 0x%08lx", index, static_cast<unsigned long>(hr));
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
                log("x2w-dstorage heartbeat %d s: requests done=%ld inflated=%ld failed=%ld bcpack-blank=%ld archive-shifted=%ld past-end=%ld stream-pieces=%ld found-on-disk=%ld; page tokens 0 at enqueue=%ld, "
                    "still 0 at run=%ld, replaced after enqueue=%ld; %s, live physical-page sections=%s", second, requests_done, requests_compressed,
                    requests_failed, requests_bcpack_blank, requests_archive_shifted, requests_archive_past_end, requests_stream_pieces, requests_pieces_found_on_disk, zero_tokens_at_enqueue, zero_tokens_at_run, tokens_replaced_after_enqueue, memory,
                    section_text);
                last = total2;
                last_memory = sample;
            }
        }
    }, nullptr, 0, nullptr);
    (void)heartbeat;
    static Object* instance = make(factory);
    InterlockedIncrement(&instance->refs);
    *out = instance;
    return S_OK;
}
