// A DirectStorage request whose destination is not writable up to its declared capacity is refused
// before any byte is written (#870: a write fault inside the stand-in's copy, not in title code).
//
// Memory-source requests go through the queue of the real stand-in DLL (path in argv[1]). The
// destination is three pages that start out 0xAA. Each refusal case makes one part of them unusable
// and expects: a failure status, the process alive, every readable byte still 0xAA. After the
// refusals, a request into a fully writable buffer on the same queue must still succeed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using u64 = std::uint64_t;
struct Object { void** vtable; };
using GetFactoryFn = long(__stdcall*)(const void*, void**);
using CreateStatusArrayFn = long(__stdcall*)(void*, unsigned, const char*, const void*, void**);
using CreateQueueFn = long(__stdcall*)(void*, const void*, const void*, void**);
using EnqueueRequestFn = void(__stdcall*)(void*, const void*);
using EnqueueStatusFn = void(__stdcall*)(void*, void*, unsigned);
using SubmitFn = void(__stdcall*)(void*);
using IsCompleteFn = bool(__stdcall*)(void*, unsigned);
using GetHResultFn = long(__stdcall*)(void*, unsigned);

constexpr unsigned page = 4096;
constexpr unsigned pages = 3;
constexpr unsigned bytes = pages * page;
constexpr unsigned cases = 6;
unsigned char source[bytes];
int failures;

Object* status;
Object* queue;
unsigned next_index;

void fail(const char* name, const char* what)
{
    std::printf("FAIL %s: %s\n", name, what);
    ++failures;
}

// Enqueues one uncompressed memory-source request and returns its status HRESULT, or -1 on timeout.
long run_request(unsigned char* destination, unsigned capacity, unsigned size)
{
    unsigned char request[0x48] = {};
    *reinterpret_cast<u64*>(request + 0x00) = 0x400000;
    *reinterpret_cast<unsigned char**>(request + 0x08) = destination;
    *reinterpret_cast<unsigned*>(request + 0x18) = capacity;
    *reinterpret_cast<const unsigned char**>(request + 0x20) = source;
    *reinterpret_cast<unsigned*>(request + 0x30) = size;
    const unsigned index = next_index++;
    reinterpret_cast<EnqueueRequestFn>(queue->vtable[3])(queue, request);
    reinterpret_cast<EnqueueStatusFn>(queue->vtable[4])(queue, status, index);
    reinterpret_cast<SubmitFn>(queue->vtable[6])(queue);
    for (int i = 0; i < 500; ++i) {
        if (reinterpret_cast<IsCompleteFn>(status->vtable[3])(status, index))
            return reinterpret_cast<GetHResultFn>(status->vtable[4])(status, index);
        Sleep(10);
    }
    return -1;
}

bool untouched(const unsigned char* region, unsigned from, unsigned to)
{
    for (unsigned i = from; i < to; ++i)
        if (region[i] != 0xAA) return false;
    return true;
}

unsigned char* fresh_region()
{
    auto* region = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (region) std::memset(region, 0xAA, bytes);
    return region;
}

// Refusal: the middle page, or the last, is made unusable in the way `how` names.
void refused(const char* name, DWORD protect, bool decommit_last)
{
    unsigned char* region = fresh_region();
    if (!region) { fail(name, "VirtualAlloc"); return; }
    DWORD old = 0;
    bool ok = true;
    if (decommit_last) ok = VirtualFree(region + 2 * page, page, MEM_DECOMMIT) != 0;
    else ok = VirtualProtect(region + page, page, protect, &old) != 0;
    if (!ok) { fail(name, "could not change the page"); VirtualFree(region, 0, MEM_RELEASE); return; }
    const long hr = run_request(region, bytes, decommit_last ? page : bytes);
    if (hr == -1) fail(name, "no completion");
    else if (hr == 0) fail(name, "request succeeded");
    // The page the case changed is not read (a guard or no-access page would trip the check itself).
    else if (!untouched(region, 0, page)) fail(name, "first page was written");
    else if (!decommit_last && !untouched(region, 2 * page, bytes)) fail(name, "last page was written");
    else std::printf("ok: %s refused, bytes before the bad page intact\n", name);
    VirtualFree(region, 0, MEM_RELEASE);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::printf("usage: startup_dstorage_destination_check <XCRDAPI.dll>\n");
        return 2;
    }
    HMODULE library = LoadLibraryA(argv[1]);
    auto* get_factory = library ? reinterpret_cast<GetFactoryFn>(GetProcAddress(library, "DStorageGetFactory")) : nullptr;
    void* factory_pointer = nullptr;
    if (!get_factory || get_factory(nullptr, &factory_pointer) != 0 || !factory_pointer) return 1;
    auto* factory = static_cast<Object*>(factory_pointer);
    void* status_pointer = nullptr;
    void* queue_pointer = nullptr;
    unsigned char description[0x20] = {};
    *reinterpret_cast<std::uint16_t*>(description + 8) = 16;
    if (reinterpret_cast<CreateStatusArrayFn>(factory->vtable[5])(factory, cases, "Status", nullptr, &status_pointer) != 0 ||
        reinterpret_cast<CreateQueueFn>(factory->vtable[10])(factory, description, nullptr, &queue_pointer) != 0)
        return 1;
    status = static_cast<Object*>(status_pointer);
    queue = static_cast<Object*>(queue_pointer);
    for (unsigned i = 0; i < bytes; ++i) source[i] = static_cast<unsigned char>(i % 251);

    refused("read-only middle page", PAGE_READONLY, false);
    refused("guard middle page", PAGE_READWRITE | PAGE_GUARD, false);
    refused("no-access middle page", PAGE_NOACCESS, false);
    // The capacity runs one page past what is committed; the data itself (one page) would fit.
    refused("capacity past the committed pages", 0, true);

    // Control and recovery on the same queue: a fully writable destination still succeeds exactly.
    unsigned char* region = fresh_region();
    if (!region) return 1;
    const long hr = run_request(region, bytes, bytes);
    if (hr != 0) fail("writable control", "request failed");
    else if (std::memcmp(region, source, bytes) != 0) fail("writable control", "bytes differ");
    else std::printf("ok: writable control after the refusals wrote every byte\n");
    VirtualFree(region, 0, MEM_RELEASE);

    // Copy-on-write destination (PAGE_WRITECOPY) is also allowed and must succeed.
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr);
    auto* cow = mapping ? static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, bytes)) : nullptr;
    if (!cow) {
        fail("copy-on-write destination", "MapViewOfFile(FILE_MAP_COPY)");
    } else {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cow, &info, sizeof(info)) || (info.Protect & 0xFF) != PAGE_WRITECOPY) {
            fail("copy-on-write destination", "mapping is not PAGE_WRITECOPY");
        } else {
            const long cow_hr = run_request(cow, bytes, bytes);
            if (cow_hr != 0) fail("copy-on-write destination", "request failed");
            else if (std::memcmp(cow, source, bytes) != 0) fail("copy-on-write destination", "bytes differ");
            else std::printf("ok: copy-on-write destination wrote every byte\n");
        }
        UnmapViewOfFile(cow);
    }
    if (mapping) CloseHandle(mapping);

    if (failures) return 1;
    std::printf("dstorage destination: refused without writes, queue kept working\n");
    return 0;
}
