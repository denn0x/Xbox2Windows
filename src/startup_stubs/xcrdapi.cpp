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
LONG zero_tokens_at_enqueue, zero_tokens_at_run;
void execute_request(Object* self, const unsigned char* request, const ULONG_PTR* tokens)
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
        packed.resize(source_size);
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (file_object && file_object->kind == file &&
            ReadFile(file_object->handle, packed.data(), source_size, &read, &overlapped) && read == source_size)
            input = packed.data();
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
                static LONG diagnosed;
                if (InterlockedIncrement(&diagnosed) <= 12) {
                    std::vector<unsigned char> big(std::size_t{destination_size} * 8 + 0x40000);
                    std::size_t big_produced = 0;
                    const bool big_ok = x2w_inflate::inflate_zlib(input, source_size, big.data(), big.size(), &big_produced);
                    log("x2w-dstorage zlib failure: header %02x %02x %02x %02x, larger buffer -> %s produced=0x%zx (dest 0x%x, source 0x%x)",
                        input[0], input[1], input[2], input[3], big_ok ? "ok" : "still failed", big_produced, destination_size, source_size);
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
                std::memcpy(destination, data, size);
            }
            if (written && !zlib && !bcpack) InterlockedIncrement(&requests_done);
        }
    }
    if (outcome[0] == 'r' || outcome[0] == 'z' || outcome[0] == 'u' || outcome[0] == 'p') InterlockedIncrement(&requests_failed);
    const bool failed = outcome[0] == 'r' || outcome[0] == 'z' || outcome[0] == 'u' || outcome[0] == 'p';  // read, zlib, unknown, page write
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
            for (std::size_t i = 0; i < entry.tokens.size(); ++i)
                if (!entry.tokens[i] && title && x2w_local::readable(title + i, sizeof(ULONG_PTR))) {
                    entry.tokens[i] = title[i];
                    if (!entry.tokens[i]) InterlockedIncrement(&zero_tokens_at_run);
                }
        }
        execute_request(self, entry.bytes, entry.tokens.empty() ? nullptr : entry.tokens.data());
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
                run_entry(self, entry);
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
    const u64 options = *reinterpret_cast<const u64*>(request);
    if (options & (1ull << 21)) {
        const auto* title = *reinterpret_cast<const ULONG_PTR* const*>(request + 0x08);
        const unsigned size = *reinterpret_cast<const unsigned*>(request + 0x18);
        const unsigned page_offset = static_cast<unsigned>(*reinterpret_cast<const u64*>(request + 0x10) & 0xFFFF);
        const std::size_t pages = (std::size_t{page_offset} + size + 0xFFFF) / 0x10000;
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
                log("x2w-dstorage heartbeat %d s: requests done=%ld inflated=%ld failed=%ld bcpack-blank=%ld; page tokens 0 at enqueue=%ld, "
                    "still 0 at run=%ld", second, requests_done, requests_compressed, requests_failed, requests_bcpack_blank,
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
