// Desktop XCRDAPI.dll for local startup experiments (local/startup-stubs, never pushed).
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

#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdarg>
#include <algorithm>
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
    const unsigned char* origin;  // the title's request, to see whether it changed after enqueue
    unsigned zero_at_enqueue;
    Object* array;
    unsigned index;
    void* fence;
    u64 value;
};
struct QueueWork {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> waiting, submitted;
};

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
    log("x2w-dstorage   file object %p = \"%ls\"", static_cast<void*>(object), path);
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
LONG zero_tokens_at_enqueue, zero_tokens_at_run, requests_stream_pieces;

// Inflated multi-page streams, newest first (see execute_request).
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

// A zlib request whose offset holds no stream (measured on one executable, archive files of one kind):
// a valid stream of exactly `compressed` bytes starts a multiple of 64 KiB earlier. Two cases:
// - a piece whose stream is not cached (evicted, or its first piece ran on another queue): the stream
//   starts at offset - k * piece and inflates past piece k; the request gets piece k's slice;
// - a whole stream at offset - k * 64 KiB that inflates to exactly the destination size (one run: 19,393
//   failing requests, all with k = 14, each matching the archive's own boundary table). Why the
//   executable adds the shift is not known; LOCAL SHORTCUT until its offset arithmetic is understood.
// A candidate needs a zlib header and must inflate in full from `compressed` bytes (Adler-32 checked).
LONG requests_pieces_found_on_disk, requests_shifted_streams, requests_shift_unconfirmed;
// RDS5 boundary table of an archive (cached per handle): stored offset, logical offset, codec.
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

std::shared_ptr<const std::vector<unsigned char>> stream_on_disk(HANDLE handle, u64 offset, unsigned compressed, unsigned piece,
                                                                 u64& base, bool& whole_shifted)
{
    whole_shifted = false;
    if (!piece || compressed > (64u << 20)) return nullptr;
    const auto read_at = [handle](u64 at, unsigned char* out, unsigned size) {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(at);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        return ReadFile(handle, out, size, &read, &overlapped) && read == size;
    };
    std::vector<unsigned char> packed;
    const auto try_at = [&](u64 at, std::size_t limit, std::size_t& produced) -> std::shared_ptr<const std::vector<unsigned char>> {
        unsigned char head[2];
        if (!read_at(at, head, 2) || head[0] != 0x78 || ((head[0] << 8) | head[1]) % 31) return nullptr;
        packed.resize(compressed);
        if (!read_at(at, packed.data(), compressed)) return nullptr;
        std::vector<unsigned char> big(limit);
        if (!x2w_inflate::inflate_zlib(packed.data(), compressed, big.data(), big.size(), &produced)) return nullptr;
        return std::make_shared<const std::vector<unsigned char>>(big.begin(), big.begin() + produced);
    };
    for (u64 k = 1; k <= 64 && k * piece <= offset; ++k) {
        std::size_t produced = 0;
        const u64 at = offset - k * piece;
        auto data = try_at(at, std::min<std::size_t>(std::size_t{32} << 20, std::max<std::size_t>(std::size_t{4} << 20, std::size_t{piece} * (k + 64))), produced);
        if (data && produced > k * piece) {
            base = at;
            return data;
        }
    }
    // A real shift is one global constant (measured: 14 x 64 KiB in every archive family that shows
    // it). Single "matches" at other k deliver wrong data, so a shift is accepted only when the
    // archive's own RDS5 boundary table proves it: offset - k x 64 KiB is exactly a boundary, the
    // interval is zlib (codec 8) and its logical size equals the destination size. 14 is tried first.
    const auto* table = rds5_table(handle);
    if (!table) return nullptr;
    const auto proven = [&](u64 at) {
        const auto it = std::lower_bound(table->begin(), table->end(), at,
                                         [](const Rds5Boundary& b, u64 v) { return b.stored < v; });
        return it != table->end() && it + 1 != table->end() && it->stored == at && it->codec == 8 &&
               (it + 1)->logical - it->logical == piece;
    };
    for (u64 step = 0; step <= 256; ++step) {
        const u64 k = step ? step : 14;
        if ((step && k == 14) || k * 0x10000 > offset || !proven(offset - k * 0x10000)) continue;
        std::size_t produced = 0;
        auto data = try_at(offset - k * 0x10000, std::size_t{piece} + 1, produced);
        if (data && produced == piece) {
            base = offset;  // the request takes the stream from its start
            whole_shifted = true;
            if (k != 14) InterlockedIncrement(&requests_shift_unconfirmed);  // proven, but not the usual 14
            return data;
        }
    }
    return nullptr;
}

bool is_failure(const char* outcome) { return outcome[0] == 'r' || outcome[0] == 'z' || outcome[0] == 'u' || outcome[0] == 'p'; }

void log_request(Object* self, u64 options, unsigned char* destination, unsigned destination_size, u64 source,
                 u64 offset, unsigned source_size, const char* outcome)
{
    const bool failed = is_failure(outcome);  // read, zlib, unknown, page write
    if (failed) InterlockedIncrement(&requests_failed);
    if (failed && self) InterlockedExchange(&self->pending, static_cast<LONG>(E_FAIL));
    static LONG summaries, failures;
    if (InterlockedIncrement(&summaries) <= 12 || (failed && InterlockedIncrement(&failures) <= 12))
        log("x2w-dstorage   options=0x%llx dest=%p dest_size=0x%x source=0x%llx offset=0x%llx size=0x%x -> %s", options,
            static_cast<void*>(destination), destination_size, source, offset, source_size, outcome);
}

// LOCAL DIAGNOSTIC: X2W_DSTORAGE_TRACE names a text file that gets one line per request (queue, page
// tokens 0 at enqueue and at run, whether the title's copy changed since enqueue, the 0x48 bytes, outcome).
void trace_request(Object* self, const Entry& entry, unsigned zero_at_run, const char* outcome)
{
    static HANDLE out = [] {
        wchar_t path[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"X2W_DSTORAGE_TRACE", path, MAX_PATH);
        return n && n < MAX_PATH ? CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr)
                                 : INVALID_HANDLE_VALUE;
    }();
    if (out == INVALID_HANDLE_VALUE) return;
    const char* changed = "?";
    if (entry.origin && x2w_local::readable(entry.origin, sizeof entry.bytes))
        changed = std::memcmp(entry.origin, entry.bytes, sizeof entry.bytes) ? "changed" : "same";
    static std::mutex lock;
    static unsigned long long sequence;
    const std::lock_guard guard(lock);
    char line[512];
    int n = std::snprintf(line, sizeof line, "%llu %lu q=%p z0=%u zr=%u %s [", sequence++, GetTickCount(),
                          static_cast<void*>(self), entry.zero_at_enqueue, zero_at_run, changed);
    for (int i = 0; i < 0x48; ++i) n += std::snprintf(line + n, sizeof line - n, "%02x", entry.bytes[i]);
    n += std::snprintf(line + n, sizeof line - n, "] %s\r\n", outcome);
    DWORD written;
    WriteFile(out, line, static_cast<DWORD>(n), &written, nullptr);
}

const char* execute_request(Object* self, const unsigned char* request, const ULONG_PTR* tokens)
{
    if (!request) return "null";
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
    // Option bits (matched against the requests one executable makes): bit 0
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
    HANDLE file_handle = nullptr;  // the source file, for pieces whose stream start must be found on disk
    if (options & ~known) {
        outcome = "unknown option bits, not done";
    } else if (!destination || !source_size) {
        outcome = "empty";
    } else if (memory_source) {
        input = reinterpret_cast<const unsigned char*>(source);
    } else {
        auto* file_object = reinterpret_cast<Object*>(source);
        LARGE_INTEGER file_size{};
        if (!file_object || !x2w_local::readable(file_object, sizeof(Object)) || file_object->kind != file ||
            !GetFileSizeEx(file_object->handle, &file_size) || offset + source_size > static_cast<u64>(file_size.QuadPart)) {
            // Checked before the buffer is sized from the request (one request named 0xd508a3ec bytes).
            outcome = "read failed (no file, or past its end)";
            log_request(self, options, destination, destination_size, source, offset, source_size, outcome);
            return outcome;
        }
        packed.resize(source_size);
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (file_object && file_object->kind == file &&
            ReadFile(file_object->handle, packed.data(), source_size, &read, &overlapped) && read == source_size) {
            input = packed.data();
            file_handle = file_object->handle;
        }
        else
            outcome = "read failed";
    }
    if (input) {
        const unsigned char* data = input;
        std::size_t size = source_size;
        if (zlib) {
            // With BCPack after it, the inflated size is not given; the destination size bounds it.
            output.resize(bcpack ? std::size_t{destination_size} * 2 + 0x10000 : destination_size);
            std::size_t produced = 0;
            if (!x2w_inflate::inflate_zlib(input, source_size, output.data(), output.size(), &produced)) {
                outcome = "zlib failed";
                data = nullptr;
                // Multi-page streams (hypothesis, measured on one executable): one zlib stream that
                // inflates to several destination-sized pieces is enqueued as one request per piece.
                // Every piece names the same compressed size; piece k's source offset is the stream's
                // offset plus k times the destination size. The first piece is inflated in full and
                // cached; each piece gets its slice.
                std::shared_ptr<const std::vector<unsigned char>> whole;
                u64 base = 0;
                if (!bcpack && find_stream(source, offset, source_size, destination_size, whole, base)) {
                    outcome = "inflated (stream piece)";
                } else if (bool shifted = false;
                           !bcpack && file_handle && (whole = stream_on_disk(file_handle, offset, source_size, destination_size, base, shifted))) {
                    if (shifted) {
                        outcome = "inflated (shifted stream)";
                        InterlockedIncrement(&requests_shifted_streams);
                    } else {
                        remember_stream(source, base, source_size, whole);
                        outcome = "inflated (stream piece)";
                        InterlockedIncrement(&requests_pieces_found_on_disk);
                    }
                } else if (!bcpack && destination_size <= (16u << 20)) {
                    auto big = std::make_shared<std::vector<unsigned char>>(
                        std::min<std::size_t>(std::size_t{32} << 20, std::max<std::size_t>(std::size_t{4} << 20, std::size_t{destination_size} * 64)));
                    std::size_t big_produced = 0;
                    if (x2w_inflate::inflate_zlib(input, source_size, big->data(), big->size(), &big_produced) && big_produced > destination_size) {
                        // An exact-size copy: resize would keep the large buffer's capacity in the cache.
                        auto exact = std::make_shared<const std::vector<unsigned char>>(big->begin(), big->begin() + big_produced);
                        big.reset();
                        remember_stream(source, offset, source_size, exact);
                        whole = exact;
                        base = offset;
                        outcome = "inflated (stream piece)";
                    }
                }
                if (whole) {
                    const std::size_t at = static_cast<std::size_t>(offset - base);
                    const std::size_t take = std::min<std::size_t>(destination_size, whole->size() - at);
                    output.assign(whole->begin() + at, whole->begin() + at + take);
                    data = output.data();
                    size = take;
                    InterlockedIncrement(&requests_compressed);
                    InterlockedIncrement(&requests_stream_pieces);
                } else {
                    static LONG diagnosed;
                    if (InterlockedIncrement(&diagnosed) <= 12)
                        log("x2w-dstorage zlib failure: header %02x %02x %02x %02x (dest 0x%x, source 0x%x, offset 0x%llx)",
                            input[0], input[1], input[2], input[3], destination_size, source_size, offset);
                }
            } else {
                output.resize(produced);
                data = output.data();
                size = produced;
                outcome = produced == destination_size || bcpack ? "inflated" : "inflated (short)";
                InterlockedIncrement(&requests_compressed);
            }
        }
        if (data && bcpack) {
            // shortcut: BCPack is not decoded yet; the texels are left zero (a blank texture) so the
            // title's loading continues. Counted as bcpack_blank.
            output.assign(destination_size, 0);
            data = output.data();
            size = destination_size;
            outcome = "BCPack not decoded: zero-filled";
            InterlockedIncrement(&requests_bcpack_blank);
        }
        if (data) {
            if (size > destination_size) size = destination_size;
            bool written = true;
            if (page_destination) {
                using WritePhysical = BOOL(WINAPI*)(const ULONG_PTR*, ULONG_PTR, SIZE_T, const void*, SIZE_T);
                static const WritePhysical write_physical = [] {
                    const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
                    return xmem ? reinterpret_cast<WritePhysical>(GetProcAddress(xmem, "x2w_xmem_write_physical")) : nullptr;
                }();
                const ULONG_PTR pages = (page_offset + size + 0xFFFF) / 0x10000;
                written = write_physical && tokens && write_physical(tokens, pages, page_offset, data, size);
                if (!written) outcome = "page destination write failed";
            } else if (data != destination) {
                // LOCAL: a destination that is not committed and writable over its whole span (seen
                // under memory pressure, after the title's allocator failed) fails this request
                // instead of ending the process in memcpy.
                const auto span_writable = [](unsigned char* at, std::size_t bytes) {
                    while (bytes) {
                        MEMORY_BASIC_INFORMATION mbi{};
                        if (!VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
                        const DWORD p = mbi.Protect & 0xFF;
                        if (p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_WRITECOPY)
                            return false;
                        const std::size_t left = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize - at;
                        const std::size_t step = left < bytes ? left : bytes;
                        at += step;
                        bytes -= step;
                    }
                    return true;
                };
                if (span_writable(static_cast<unsigned char*>(destination), size)) {
                    std::memcpy(destination, data, size);
                } else {
                    written = false;
                    outcome = "destination not writable";
                    InterlockedIncrement(&requests_failed);
                }
            }
            if (written && !zlib && !bcpack) InterlockedIncrement(&requests_done);
        }
    }
    log_request(self, options, destination, destination_size, source, offset, source_size, outcome);
    return outcome;
}

void signal_fence(void* fence, u64 value);

void run_entry(Object* self, Entry& entry)
{
    if (entry.type == Entry::request) {
        unsigned zero_at_run = 0;
        const u64 options = *reinterpret_cast<const u64*>(entry.bytes);
        if ((options & (1ull << 21)) && !entry.tokens.empty()) {
            const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(entry.bytes + 0x08);
            for (std::size_t i = 0; i < entry.tokens.size(); ++i)
                if (!entry.tokens[i] && title && x2w_local::readable(title + i, sizeof(ULONG_PTR))) {
                    entry.tokens[i] = title[i];
                    if (!entry.tokens[i]) InterlockedIncrement(&zero_tokens_at_run), ++zero_at_run;
                }
        }
        const char* outcome = execute_request(self, entry.bytes, entry.tokens.empty() ? nullptr : entry.tokens.data());
        trace_request(self, entry, zero_at_run, outcome);
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
        for (auto& entry : work.waiting) work.submitted.push_back(std::move(entry));
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
    entry.origin = request;
    const u64 options = *reinterpret_cast<const u64*>(request);
    if (options & (1ull << 21)) {
        const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(request + 0x08);
        const unsigned size = *reinterpret_cast<const unsigned*>(request + 0x18);
        const unsigned page_offset = static_cast<unsigned>(*reinterpret_cast<const u64*>(request + 0x10) & 0xFFFF);
        const std::size_t pages = (std::size_t{page_offset} + size + 0xFFFF) / 0x10000;
        if (title && pages && x2w_local::readable(title, pages * sizeof(ULONG_PTR))) {
            entry.tokens.assign(title, title + pages);
            for (ULONG_PTR token : entry.tokens)
                if (!token) InterlockedIncrement(&zero_tokens_at_enqueue), ++entry.zero_at_enqueue;
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

}  // namespace

extern "C" long __stdcall DStorageGetFactory(const GUID* riid, void** out)
{
    char id[48];
    guid_text(riid, id, sizeof(id));
    log("x2w-dstorage DStorageGetFactory riid=%s", id);
    if (!out) return E_POINTER;
    // Heartbeat: the request counters every 10 s, so a run shows whether loading still progresses
    // after the per-call logs have gone quiet.
    static HANDLE heartbeat = CreateThread(nullptr, 0, [](void*) -> DWORD {
        LONG last = -1;
        for (int second = 10;; second += 10) {
            Sleep(10000);
            const LONG total = requests_done + requests_compressed + requests_failed;
            const LONG total2 = total + requests_bcpack_blank;
            if (total2 != last)
                log("x2w-dstorage heartbeat %d s: requests done=%ld inflated=%ld (stream pieces %ld, starts found on disk %ld, shifted streams %ld, shifts other than 14 %ld) failed=%ld bcpack-blank=%ld; page tokens 0 at enqueue=%ld, "
                    "still 0 at run=%ld", second, requests_done, requests_compressed, requests_stream_pieces, requests_pieces_found_on_disk, requests_shifted_streams, requests_shift_unconfirmed, requests_failed, requests_bcpack_blank,
                    zero_tokens_at_enqueue, zero_tokens_at_run);
            last = total2;
        }
    }, nullptr, 0, nullptr);
    (void)heartbeat;
    static Object* instance = make(factory);
    InterlockedIncrement(&instance->refs);
    *out = instance;
    return S_OK;
}
