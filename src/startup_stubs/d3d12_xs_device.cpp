// LOCAL EXPERIMENT ONLY (local/startup-stubs, never pushed).
//
// With X2W_D3D12_PERMISSIVE=1, D3D12XboxCreateDevice accepts a non-null Xbox parameter block and
// returns a recording wrapper around a desktop ID3D12Device. Every object the wrappers hand out is
// wrapped too. Every vtable slot traps ("x2w-trap d3d12_xs.dll!<Object>::slot<N>", then terminates)
// unless it is enabled in build_tables(). Enabled slots log "x2w-d3d12" lines (the first calls only).
//
// Evidence so far: the title requests desktop IIDs (ID3D12Device11, ID3D12CommandQueue, ID3D12Fence,
// ID3D12CommandAllocator), and device slots 8, 9, 14, 26, 28 and 36 have desktop call shapes. Slot 48
// does not (Xbox queue creation), so slots from 48 up are only enabled one by one from call sites.
// Methods that return a structure by value are called with the hidden result pointer in rcx and the
// object in rdx (desktop: object first); those slots accept either order.
#define X2W_TRAP_MODULE "d3d12_xs.dll"
#define X2W_TRAP_MODULE_ID 7
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using u64 = std::uint64_t;
constexpr int slot_count = 512;

enum Kind : int {
    device_kind, queue_kind, fence_kind, allocator_kind, list_kind, descriptor_heap_kind, root_signature_kind,
    resource_kind, heap_kind, query_heap_kind, pipeline_kind, command_signature_kind, other_kind, kind_count
};
const char* const kind_names[kind_count] = {
    "Device", "CommandQueue", "Fence", "CommandAllocator", "CommandList", "DescriptorHeap", "RootSignature",
    "Resource", "Heap", "QueryHeap", "PipelineState", "CommandSignature", "Object"};

// The title reads console object fields directly. Seen on a command list: +0x18 is the command
// buffer cursor and +0x30 its limit; inlined calls write GPU packets at the cursor while it is below
// the limit and otherwise call the virtual method (IASetPrimitiveTopology: slot 20, +0xa0). Both
// stay 0 here, so every inlined call takes its virtual path. The first 0x400 bytes after the vtable
// pointer are a zero-filled stand-in for such fields; the wrapper's own state follows.
struct Wrapper {
    void** vtable;
    std::uint64_t console_fields[0x7f];  // +0x08..+0x400, all zero
    IUnknown* real;
    LONG refs;
    Kind kind;
    void* title_memory;       // Resource: the title-side memory (placement address or hook allocation), or null
    std::uint64_t title_cpu;  // DescriptorHeap: title-owned descriptor memory, or 0
    // Fence: the completed value the title polls through the address from slot 12 (monitorx/mwaitx).
    // Updated when the fence is signaled; GPU work counts as complete at once while lists are no-ops.
    volatile LONG64 completed;
};

void fence_reached(Wrapper* fence, std::uint64_t value) noexcept
{
    LONG64 seen = fence->completed;
    while (static_cast<std::uint64_t>(seen) < value || seen == -1) {
        const LONG64 previous = InterlockedCompareExchange64(&fence->completed, static_cast<LONG64>(value), seen);
        if (previous == seen) break;
        seen = previous;
    }
}

// Descriptor heaps created over title memory: title CPU handles in [title, title + bytes) map to
// desktop CPU handles at the same offset (the title's increments come from the forwarded desktop
// GetDescriptorHandleIncrementSize, so offsets agree). Descriptors the title writes or copies into
// that memory itself are not seen by the desktop heap.
struct DescriptorRange {
    std::uint64_t title;
    std::uint64_t bytes;
    std::uint64_t desktop;
};
SRWLOCK descriptor_lock = SRWLOCK_INIT;
std::vector<DescriptorRange> descriptor_ranges;

std::uint64_t desktop_cpu_handle(std::uint64_t handle)
{
    AcquireSRWLockShared(&descriptor_lock);
    std::uint64_t result = handle;
    for (const auto& r : descriptor_ranges)
        if (handle >= r.title && handle < r.title + r.bytes) result = r.desktop + (handle - r.title);
    ReleaseSRWLockShared(&descriptor_lock);
    return result;
}

void* tables[kind_count][slot_count];
Wrapper* the_device;

void log(const char* format, ...)
{
    char line[1536];
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

void guid_text(const GUID* g, char* out, std::size_t size)
{
    if (!g || !x2w_local::readable(g, sizeof(GUID))) {
        std::snprintf(out, size, "%p", static_cast<const void*>(g));
        return;
    }
    std::snprintf(out, size, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3,
                  g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
                  g->Data4[7]);
}

void hex_text(const void* data, std::size_t bytes, char* out, std::size_t size)
{
    out[0] = 0;
    if (!data || !x2w_local::readable(data, bytes)) return;
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t used = 0;
    for (std::size_t i = 0; i < bytes && used + 4 < size; ++i) used += std::snprintf(out + used, size - used, "%02x ", p[i]);
}

bool first_calls(LONG& counter, LONG limit = 3) { return InterlockedIncrement(&counter) <= limit; }

// X2W_D3D12_DEBUG=1: the desktop debug layer's messages are drained into the log after forwarded
// calls, tagged with the call that preceded them, and a device removal is reported once with its reason.
ID3D12InfoQueue* info_queue;
ID3D12Device* removal_device;
LONG removal_reported, debug_lines;
SRWLOCK debug_lock = SRWLOCK_INIT;

void check_device(const char* context, int slot)
{
    if (removal_device && !removal_reported) {
        const HRESULT reason = removal_device->GetDeviceRemovedReason();
        if (reason != S_OK && InterlockedExchange(&removal_reported, 1) == 0)
            log("x2w-d3d12 DEVICE REMOVED, reason 0x%08lx, first noticed after %s %d", static_cast<unsigned long>(reason),
                context, slot);
    }
    if (!info_queue) return;
    AcquireSRWLockExclusive(&debug_lock);
    const UINT64 count = info_queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count && debug_lines < 400; ++i) {
        SIZE_T size = 0;
        info_queue->GetMessage(i, nullptr, &size);
        if (!size || size > 8192) continue;
        alignas(16) char storage[8192];
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage);
        if (SUCCEEDED(info_queue->GetMessage(i, message, &size))) {
            ++debug_lines;
            log("x2w-d3d12-debug [%s %d] sev=%d id=%d %.900s", context, slot, message->Severity, message->ID,
                message->pDescription);
        }
    }
    info_queue->ClearStoredMessages();
    ReleaseSRWLockExclusive(&debug_lock);
}

// Wrapper detection is a registry lookup: no argument is ever dereferenced to find out what it is.
SRWLOCK registry_lock = SRWLOCK_INIT;
std::unordered_set<const void*>& registry()
{
    static std::unordered_set<const void*> wrappers;
    return wrappers;
}

Wrapper* as_wrapper(const void* p)
{
    const auto value = reinterpret_cast<std::uintptr_t>(p);
    if (value < 0x10000 || (value & 7)) return nullptr;
    AcquireSRWLockShared(&registry_lock);
    const bool known = registry().count(p) != 0;
    ReleaseSRWLockShared(&registry_lock);
    return known ? const_cast<Wrapper*>(static_cast<const Wrapper*>(p)) : nullptr;
}

u64 unwrap(u64 value)
{
    Wrapper* w = as_wrapper(reinterpret_cast<const void*>(value));
    return w ? reinterpret_cast<u64>(w->real) : value;
}

Wrapper* wrap(IUnknown* real, Kind kind)
{
    auto* w = new Wrapper{};
    w->vtable = tables[kind];
    w->real = real;
    w->refs = 1;
    w->kind = kind;
    if (kind == fence_kind && real) w->completed = static_cast<LONG64>(static_cast<ID3D12Fence*>(real)->GetCompletedValue());
    AcquireSRWLockExclusive(&registry_lock);
    registry().insert(w);
    ReleaseSRWLockExclusive(&registry_lock);
    return w;
}

Kind kind_for(REFIID iid)
{
    if (iid == __uuidof(ID3D12Device) || iid == __uuidof(ID3D12Device1) || iid == __uuidof(ID3D12Device2) ||
        iid == __uuidof(ID3D12Device3) || iid == __uuidof(ID3D12Device4) || iid == __uuidof(ID3D12Device5) ||
        iid == __uuidof(ID3D12Device6) || iid == __uuidof(ID3D12Device7) || iid == __uuidof(ID3D12Device8) ||
        iid == __uuidof(ID3D12Device9) || iid == __uuidof(ID3D12Device10) || iid == __uuidof(ID3D12Device11))
        return device_kind;
    if (iid == __uuidof(ID3D12CommandQueue)) return queue_kind;
    if (iid == __uuidof(ID3D12Fence) || iid == __uuidof(ID3D12Fence1)) return fence_kind;
    if (iid == __uuidof(ID3D12CommandAllocator)) return allocator_kind;
    if (iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList) ||
        iid == __uuidof(ID3D12GraphicsCommandList1) || iid == __uuidof(ID3D12GraphicsCommandList2) ||
        iid == __uuidof(ID3D12GraphicsCommandList3) || iid == __uuidof(ID3D12GraphicsCommandList4) ||
        iid == __uuidof(ID3D12GraphicsCommandList5) || iid == __uuidof(ID3D12GraphicsCommandList6) ||
        iid == __uuidof(ID3D12GraphicsCommandList7))
        return list_kind;
    // Xbox-only command list interface requested from device slot 73 (not in the desktop SDK header).
    static const GUID xbox_list = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    if (iid == xbox_list) return list_kind;
    if (iid == __uuidof(ID3D12DescriptorHeap)) return descriptor_heap_kind;
    if (iid == __uuidof(ID3D12RootSignature)) return root_signature_kind;
    if (iid == __uuidof(ID3D12Resource) || iid == __uuidof(ID3D12Resource1) || iid == __uuidof(ID3D12Resource2))
        return resource_kind;
    if (iid == __uuidof(ID3D12Heap) || iid == __uuidof(ID3D12Heap1)) return heap_kind;
    if (iid == __uuidof(ID3D12QueryHeap)) return query_heap_kind;
    if (iid == __uuidof(ID3D12PipelineState)) return pipeline_kind;
    if (iid == __uuidof(ID3D12CommandSignature)) return command_signature_kind;
    return other_kind;
}

bool is_base_iid(REFIID iid)
{
    return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
           iid == __uuidof(ID3D12Pageable);
}

// Traps. The kind is a template argument so a trap never dereferences its arguments.
template <int K, int N>
__declspec(noinline) u64 trap_slot(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%s::slot%d", kind_names[K], N);
    x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, (0x80 | N) & 0xFF, rcx, rdx, r8, r9, _ReturnAddress());
}

template <int K, std::size_t... I>
void fill_traps(void** table, std::index_sequence<I...>)
{
    ((table[I] = reinterpret_cast<void*>(&trap_slot<K, static_cast<int>(I)>)), ...);
}

template <std::size_t... K>
void fill_all_traps(std::index_sequence<K...>)
{
    (fill_traps<static_cast<int>(K)>(tables[K], std::make_index_sequence<slot_count>{}), ...);
}

// A call that passes a known interface IID is a creation or query call: a no-op that returned 0
// would hand the title "success" with no object. Such calls trap instead of being ignored.
bool names_known_iid(u64 value)
{
    if (value < 0x10000 || !x2w_local::readable(reinterpret_cast<const void*>(value), sizeof(GUID))) return false;
    return kind_for(*reinterpret_cast<const GUID*>(value)) != other_kind;
}
bool create_shaped(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g)
{
    return names_known_iid(a) || names_known_iid(b) || names_known_iid(c) || names_known_iid(d) ||
           names_known_iid(e) || names_known_iid(f) || names_known_iid(g);
}

bool list_noop()
{
    char value[4]{};
    return GetEnvironmentVariableA("X2W_D3D12_LIST_NOOP", value, sizeof(value)) && value[0] == '1';
}

// With X2W_D3D12_LIST_NOOP=1: an unknown command-list method records nothing and returns 0.
template <int K, int N>
__declspec(noinline) u64 noop_slot(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    // Register arguments only: stale caller stack words often still point at an IID.
    if (create_shaped(rdx, r8, r9, 0, 0, 0, 0)) {
        char name[64];
        std::snprintf(name, sizeof(name), "%s::slot%d(passes an IID)", kind_names[K], N);
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, rcx, rdx, r8, r9, _ReturnAddress());
    }
    static LONG calls;
    if (first_calls(calls, 1)) {
        char name[64];
        std::snprintf(name, sizeof(name), "%s::slot%d", kind_names[K], N);
        x2w_trap::record("x2w-noop", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, rcx, rdx, r8, r9,
                         _ReturnAddress());
    }
    return 0;
}

bool device_noop()
{
    char value[4]{};
    return GetEnvironmentVariableA("X2W_D3D12_DEVICE_NOOP", value, sizeof(value)) && value[0] == '1';
}

// With X2W_D3D12_DEVICE_NOOP=1: an unimplemented Xbox-order device method (slot 44 and up) logs its
// first call with the first 48 bytes behind rdx and r8 and returns 0.
template <int N>
__declspec(noinline) u64 device_noop_slot(u64 rcx, u64 rdx, u64 r8, u64 r9, u64 s0, u64 s1, u64 s2, u64 s3)
{
    if (create_shaped(rdx, r8, r9, s0, s1, s2, s3)) {
        char name[64];
        std::snprintf(name, sizeof(name), "Device::slot%d(passes an IID)", N);
        char first[3 * 64 + 1];
        hex_text(reinterpret_cast<const void*>(rdx), 64, first, sizeof(first));
        log("x2w-d3d12 Device::slot%d passes an IID: rdx=0x%llx r8=0x%llx r9=0x%llx stack=0x%llx,0x%llx,0x%llx,0x%llx [rdx]=[%s]",
            N, rdx, r8, r9, s0, s1, s2, s3, first);
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, rcx, rdx, r8, r9, _ReturnAddress());
    }
    static LONG calls;
    if (first_calls(calls, 1)) {
        char name[64];
        std::snprintf(name, sizeof(name), "Device::slot%d", N);
        x2w_trap::record("x2w-noop", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, rcx, rdx, r8, r9,
                         _ReturnAddress());
        char first[3 * 48 + 1], second[3 * 48 + 1];
        hex_text(reinterpret_cast<const void*>(rdx), 48, first, sizeof(first));
        hex_text(reinterpret_cast<const void*>(r8), 48, second, sizeof(second));
        log("x2w-noop   [rdx]=[%s] [r8]=[%s]", first, second);
    }
    return 0;
}

template <std::size_t... I>
void fill_device_noops(void** table, std::index_sequence<I...>)
{
    ((I >= 44 && table[I] == reinterpret_cast<void*>(&trap_slot<device_kind, static_cast<int>(I)>)
          ? (table[I] = reinterpret_cast<void*>(&device_noop_slot<static_cast<int>(I)>)) : nullptr), ...);
}

template <int K, std::size_t... I>
void fill_noops(void** table, std::index_sequence<I...>)
{
    ((I >= 8 ? (table[I] = reinterpret_cast<void*>(&noop_slot<K, static_cast<int>(I)>)) : nullptr), ...);
}

// With X2W_D3D12_DEVICE_NOOP=1, the same for the slots of the other object kinds that are still traps.
template <int K, std::size_t... I>
void fill_remaining_noops(void** table, std::index_sequence<I...>)
{
    ((I >= 8 && table[I] == reinterpret_cast<void*>(&trap_slot<K, static_cast<int>(I)>)
          ? (table[I] = reinterpret_cast<void*>(&noop_slot<K, static_cast<int>(I)>)) : nullptr), ...);
}
template <std::size_t... K>
void fill_all_remaining_noops(std::index_sequence<K...>)
{
    ((static_cast<int>(K) != device_kind && static_cast<int>(K) != list_kind
          ? fill_remaining_noops<static_cast<int>(K)>(tables[K], std::make_index_sequence<slot_count>{}) : void()), ...);
}

[[noreturn]] void fail(const char* what, const void* a, const void* b)
{
    x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, what, 0xFE, reinterpret_cast<u64>(a), reinterpret_cast<u64>(b),
                   0, 0, _ReturnAddress());
}

using Method = u64(__stdcall*)(IUnknown*, u64, u64, u64, u64, u64, u64, u64, u64);
Method method_of(IUnknown* real, int slot) { return reinterpret_cast<Method>((*reinterpret_cast<void***>(real))[slot]); }

// Desktop-order forwarder for a slot whose arguments are integers or pointers (no floating-point
// argument in the first four positions, at most eight arguments). Wrapped objects among the arguments
// are replaced by their desktop objects; unused argument words are caller stack the callee ignores.
template <int K, int N>
__declspec(noinline) u64 forward_slot(Wrapper* self, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    const u64 result = method_of(self->real, N)(self->real, unwrap(a), unwrap(b), unwrap(c), unwrap(d), unwrap(e),
                                                unwrap(f), unwrap(g), unwrap(h));
    static LONG calls;
    if (first_calls(calls))
        log("x2w-d3d12 %s::slot%d(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) -> 0x%llx", kind_names[K], N, a, b, c, d, e,
            result);
    check_device(kind_names[K], N);
    return result;
}

// Desktop-order create method whose last two arguments are (riid, ppv), riid at argument index R
// (0-based, after the object). The created object is wrapped by its interface.
// Resource descriptions: the title sets flag bits desktop D3D12 rejects (0x20000 on buffers,
// 0x100000 and 0x200000 on render-target textures). Desktop receives only the bits it defines
// up to ALLOW_SIMULTANEOUS_ACCESS (0x3f); dropped bits are logged once per value.
// Console-only DXGI format values (116-120 in the console's format extensions: two 10-bit float
// formats, D16_UNORM_S8_UINT and its two typed views) mapped to the nearest desktop formats. The
// bit layouts differ, so contents are not interchangeable; this only lets the resources exist.
DXGI_FORMAT desktop_format(DXGI_FORMAT format)
{
    DXGI_FORMAT mapped = format;
    switch (static_cast<UINT>(format)) {
    case 116: case 117: mapped = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case 118: mapped = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
    case 119: mapped = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case 120: mapped = DXGI_FORMAT_X24_TYPELESS_G8_UINT; break;
    default: break;
    }
    if (mapped != format) {
        static LONG calls;
        if (first_calls(calls, 6)) log("x2w-d3d12 console format %u -> desktop format %u", format, mapped);
    }
    return mapped;
}

constexpr UINT desktop_resource_flags = 0x3f;
D3D12_RESOURCE_DESC sanitized(const D3D12_RESOURCE_DESC& desc)
{
    D3D12_RESOURCE_DESC copy = desc;
    const UINT dropped = static_cast<UINT>(desc.Flags) & ~desktop_resource_flags;
    if (dropped) {
        static UINT seen[16];
        static LONG count;
        bool known = false;
        for (LONG i = 0; i < count && i < 16; ++i) known |= seen[i] == dropped;
        if (!known && count < 16) {
            seen[count++] = dropped;
            log("x2w-d3d12 resource flags 0x%x: dropped Xbox bits 0x%x (dimension %d)", desc.Flags, dropped, desc.Dimension);
        }
    }
    copy.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(desc.Flags) & desktop_resource_flags);
    // Depth-stencil with unordered access or render target (seen: 0x6 on a depth texture) is refused by desktop.
    if (copy.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        copy.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    // The Layout field carries a console swizzle mode (0x100 and up, the value XGSuggestSwizzleMode
    // works on); desktop gets UNKNOWN. A 4 KiB alignment request becomes the desktop default.
    if (static_cast<UINT>(desc.Layout) > D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE) {
        static LONG layouts;
        if (first_calls(layouts, 4)) log("x2w-d3d12 resource layout 0x%x (console swizzle) -> UNKNOWN", desc.Layout);
        copy.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    if (desc.Alignment != 0 && desc.Alignment != 0x10000 && desc.Alignment != 0x400000) copy.Alignment = 0;
    copy.Format = desktop_format(desc.Format);
    return copy;
}

template <int K, int N, int R, int Desc = -1>
__declspec(noinline) u64 create_slot(Wrapper* self, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    u64 args[8] = {a, b, c, d, e, f, g, h};
    for (int i = 0; i < R; ++i) args[i] = unwrap(args[i]);
    D3D12_RESOURCE_DESC desc_copy{};
    if (Desc >= 0 && args[Desc]) {
        desc_copy = sanitized(*reinterpret_cast<const D3D12_RESOURCE_DESC*>(args[Desc]));
        args[Desc] = reinterpret_cast<u64>(&desc_copy);
    }
    const u64 result = method_of(self->real, N)(self->real, args[0], args[1], args[2], args[3], args[4], args[5],
                                                args[6], args[7]);
    check_device(kind_names[K], N);
    const auto* iid = reinterpret_cast<const GUID*>(args[R]);
    auto** out = reinterpret_cast<void**>(args[R + 1]);
    Kind kind = other_kind;
    if (SUCCEEDED(static_cast<HRESULT>(result)) && iid && out && *out) {
        kind = kind_for(*iid);
        *out = wrap(static_cast<IUnknown*>(*out), kind);
    }
    static LONG calls;
    if (first_calls(calls, 4)) {
        char text[48];
        guid_text(iid, text, sizeof(text));
        log("x2w-d3d12 %s::slot%d create(0x%llx, 0x%llx, 0x%llx, 0x%llx) riid=%s -> 0x%08llx %s", kind_names[K], N, a, b,
            c, d, text, result, kind_names[kind]);
    }
    return result;
}

// Methods returning a structure by value: accept the object in either of the first two registers.
Wrapper* receiver(void* a, void* b, void** result, const char* method)
{
    if (Wrapper* w = as_wrapper(a)) {
        *result = b;
        return w;
    }
    *result = a;
    if (Wrapper* w = as_wrapper(b)) return w;
    fail(method, a, b);
}

// Slot 0, all kinds.
HRESULT __stdcall query_interface(Wrapper* self, const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !iid) return E_POINTER;
    *out = nullptr;
    HRESULT hr = E_NOINTERFACE;
    if (is_base_iid(*iid) || kind_for(*iid) == self->kind) {
        InterlockedIncrement(&self->refs);
        *out = self;
        hr = S_OK;
    }
    log("x2w-d3d12 %s::QueryInterface riid=%s -> 0x%08lx", kind_names[self->kind], text, static_cast<unsigned long>(hr));
    return hr;
}
ULONG __stdcall add_ref(Wrapper* self) { return static_cast<ULONG>(InterlockedIncrement(&self->refs)); }
// The wrappers and their desktop objects stay alive for the process (experiment scope).
ULONG __stdcall release(Wrapper* self) { return static_cast<ULONG>(InterlockedDecrement(&self->refs)); }

// Slot 7 of every device child, desktop ID3D12DeviceChild::GetDevice(riid, ppv).
HRESULT __stdcall child_get_device(Wrapper* self, const GUID* iid, void** out)
{
    if (!out || !iid) return E_POINTER;
    return query_interface(the_device, iid, out);
}

ID3D12Device* device_of(Wrapper* w) { return static_cast<ID3D12Device*>(w->real); }

// Device slot 8, desktop CreateCommandQueue(desc, riid, ppv).
HRESULT __stdcall device_create_command_queue(Wrapper* self, const D3D12_COMMAND_QUEUE_DESC* desc, const GUID* iid,
                                              void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    const HRESULT hr = kind_for(*iid) == queue_kind
        ? device_of(self)->CreateCommandQueue(desc, IID_PPV_ARGS(&queue)) : E_NOINTERFACE;
    log("x2w-d3d12 Device::CreateCommandQueue type=%d priority=%d flags=0x%x node=0x%x riid=%s -> 0x%08lx", desc->Type,
        desc->Priority, desc->Flags, desc->NodeMask, text, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    *out = wrap(queue, queue_kind);
    return S_OK;
}

// Device slot 48 (Xbox order; desktop slot 48 is OpenExistingHeapFromAddress): called four times at
// startup with a 16-byte queue descriptor (type 2, priority 0, flags 1/2/3/0, node 0/0/0/1), the
// desktop ID3D12CommandQueue IID and an output; the title asserts success. The Xbox flag meaning is
// unknown; the desktop queue is created with flags 0.
HRESULT __stdcall device_create_command_queue_x(Wrapper* self, const D3D12_COMMAND_QUEUE_DESC* desc, const GUID* iid,
                                                void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    D3D12_COMMAND_QUEUE_DESC copy = *desc;
    copy.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ID3D12CommandQueue* queue = nullptr;
    const HRESULT hr = kind_for(*iid) == queue_kind
        ? device_of(self)->CreateCommandQueue(&copy, IID_PPV_ARGS(&queue)) : E_NOINTERFACE;
    log("x2w-d3d12 Device::slot48(CreateCommandQueue?) type=%d priority=%d flags=0x%x(dropped) node=0x%x riid=%s -> 0x%08lx",
        desc->Type, desc->Priority, desc->Flags, desc->NodeMask, text, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    *out = wrap(queue, queue_kind);
    return S_OK;
}

// Device slot 10/11, desktop CreateGraphics/ComputePipelineState(desc, riid, ppv): logged and refused
// until the Xbox shader bytecode is translated (the title's shaders are not DXIL).
template <int N>
HRESULT __stdcall device_create_pipeline(Wrapper*, const void* desc, const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    char bytes[3 * 64 + 1];
    hex_text(desc, 64, bytes, sizeof(bytes));
    log("x2w-d3d12 Device::slot%d(Create%sPipelineState) desc=[%s] riid=%s -> trap", N, N == 10 ? "Graphics" : "Compute",
        bytes, text);
    if (out) *out = nullptr;
    trap_slot<device_kind, N>(0, reinterpret_cast<u64>(desc), reinterpret_cast<u64>(iid), reinterpret_cast<u64>(out));
    return E_NOTIMPL;
}

// Device slot 25, desktop GetResourceAllocationInfo(visibleMask, count, descs), returned by value.
void* __stdcall device_resource_allocation_info(void* a, void* b, UINT mask, UINT count, const D3D12_RESOURCE_DESC* descs)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "Device::GetResourceAllocationInfo(no receiver)");
    std::vector<D3D12_RESOURCE_DESC> copies(count);
    for (UINT i = 0; i < count; ++i) copies[i] = sanitized(descs[i]);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_of(self)->GetResourceAllocationInfo(mask, count, copies.data());
    *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(result) = info;
    static LONG calls, invalid;
    if (first_calls(calls))
        log("x2w-d3d12 Device::GetResourceAllocationInfo count=%u dim=%d -> size=0x%llx align=0x%llx", count,
            count && descs ? descs[0].Dimension : -1, info.SizeInBytes, info.Alignment);
    if (info.SizeInBytes == ~0ull && first_calls(invalid, 8)) {
        for (UINT i = 0; i < count && i < 4; ++i) {
            char bytes[3 * 0x38 + 1];
            hex_text(&descs[i], 0x38, bytes, sizeof(bytes));
            log("x2w-d3d12   invalid desc[%u]=[%s]", i, bytes);
        }
    }
    return result;
}

// Device slot 38, desktop GetCopyableFootprints(desc, first, count, base, layouts, rows, row bytes, total).
void __stdcall device_copyable_footprints(Wrapper* self, const D3D12_RESOURCE_DESC* desc, UINT first, UINT count,
                                          UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows,
                                          UINT64* row_bytes, UINT64* total)
{
    const D3D12_RESOURCE_DESC copy = sanitized(*desc);
    device_of(self)->GetCopyableFootprints(&copy, first, count, base, layouts, rows, row_bytes, total);
}

// Device slot 41, desktop CreateCommandSignature(desc, root, riid, ppv); on failure the descriptor and
// its argument descriptors are logged.
u64 __stdcall device_create_command_signature(Wrapper* self, const D3D12_COMMAND_SIGNATURE_DESC* desc, u64 root,
                                              u64 iid, u64 out)
{
    const u64 result = create_slot<device_kind, 41, 2>(self, reinterpret_cast<u64>(desc), root, iid, out, 0, 0, 0, 0);
    static LONG failures;
    if (FAILED(static_cast<HRESULT>(result)) && desc && first_calls(failures, 8)) {
        log("x2w-d3d12   CreateCommandSignature failed: stride=%u args=%u node=0x%x root=0x%llx", desc->ByteStride,
            desc->NumArgumentDescs, desc->NodeMask, root);
        for (UINT i = 0; i < desc->NumArgumentDescs && i < 8; ++i) {
            char bytes[3 * 16 + 1];
            hex_text(&desc->pArgumentDescs[i], sizeof(D3D12_INDIRECT_ARGUMENT_DESC), bytes, sizeof(bytes));
            log("x2w-d3d12     arg[%u]=[%s]", i, bytes);
        }
    }
    // Argument types outside the desktop enumeration (0x69 seen, stride 12) cannot be created on
    // desktop; the title gets a placeholder CommandSignature (command lists are not executed).
    bool console_type = false;
    for (UINT i = 0; desc && i < desc->NumArgumentDescs; ++i)
        console_type |= static_cast<UINT>(desc->pArgumentDescs[i].Type) > D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    if (FAILED(static_cast<HRESULT>(result)) && console_type && out) {
        log("x2w-d3d12   CreateCommandSignature: console argument type -> placeholder");
        *reinterpret_cast<void**>(out) = wrap(nullptr, command_signature_kind);
        return S_OK;
    }
    return result;
}

// Device slot 108 (Xbox order): (mask, count, descs, info1) returned by value, the desktop
// ID3D12Device4::GetResourceAllocationInfo1 shape. The caller asks with a 4 KiB alignment first and
// repeats with 0 when the result's alignment is not 4 KiB.
void* __stdcall device_resource_allocation_info1(void* a, void* b, UINT mask, UINT count, const D3D12_RESOURCE_DESC* descs,
                                                 D3D12_RESOURCE_ALLOCATION_INFO1* info1)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "Device::slot108(no receiver)");
    std::vector<D3D12_RESOURCE_DESC> copies(count);
    for (UINT i = 0; i < count; ++i) copies[i] = sanitized(descs[i]);
    D3D12_RESOURCE_ALLOCATION_INFO info{~0ull, 0};
    ID3D12Device4* device4 = nullptr;
    if (SUCCEEDED(device_of(self)->QueryInterface(IID_PPV_ARGS(&device4)))) {
        info = device4->GetResourceAllocationInfo1(mask, count, copies.data(), info1);
        device4->Release();
    }
    *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(result) = info;
    static LONG calls, invalid;
    if (first_calls(calls))
        log("x2w-d3d12 Device::slot108(GetResourceAllocationInfo1?) count=%u dim=%d align=0x%llx info1=%p -> size=0x%llx "
            "align=0x%llx", count, count ? descs[0].Dimension : -1, count ? descs[0].Alignment : 0ull,
            static_cast<void*>(info1), info.SizeInBytes, info.Alignment);
    if (info.SizeInBytes == ~0ull && first_calls(invalid, 8)) {
        char bytes[3 * 0x38 + 1];
        hex_text(descs, 0x38, bytes, sizeof(bytes));
        log("x2w-d3d12   slot108 invalid desc[0]=[%s]", bytes);
    }
    return result;
}

// Device slot 26, desktop GetCustomHeapProperties(nodeMask, heapType), returned by value.
void* __stdcall device_custom_heap_properties(void* a, void* b, UINT node_mask, D3D12_HEAP_TYPE type)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "Device::GetCustomHeapProperties(no receiver)");
    const D3D12_HEAP_PROPERTIES properties = device_of(self)->GetCustomHeapProperties(node_mask, type);
    *static_cast<D3D12_HEAP_PROPERTIES*>(result) = properties;
    static LONG calls;
    if (first_calls(calls))
        log("x2w-d3d12 Device::GetCustomHeapProperties order=%s node=0x%x type=%d -> type=%d page=%d pool=%d",
            result == a ? "ret,this" : "this,ret", node_mask, type, properties.Type, properties.CPUPageProperty,
            properties.MemoryPoolPreference);
    return result;
}

// Device slot 43, desktop GetAdapterLuid(), returned by value.

// Device slots 34/35, desktop MakeResident/Evict(count, objects).
template <int N>
HRESULT __stdcall device_residency(Wrapper* self, UINT count, void* const* objects)
{
    std::vector<ID3D12Pageable*> real(count);
    for (UINT i = 0; i < count; ++i) real[i] = reinterpret_cast<ID3D12Pageable*>(unwrap(reinterpret_cast<u64>(objects[i])));
    return N == 34 ? device_of(self)->MakeResident(count, real.data()) : device_of(self)->Evict(count, real.data());
}

// Queue slot 10, desktop ExecuteCommandLists(count, lists).
void __stdcall queue_execute(Wrapper* self, UINT count, void* const* lists)
{
    static LONG calls;
    if (list_noop()) {
        if (first_calls(calls)) log("x2w-d3d12 CommandQueue::ExecuteCommandLists count=%u -> not submitted (noop lists)", count);
        return;
    }
    std::vector<ID3D12CommandList*> real(count);
    for (UINT i = 0; i < count; ++i) real[i] = reinterpret_cast<ID3D12CommandList*>(unwrap(reinterpret_cast<u64>(lists[i])));
    static_cast<ID3D12CommandQueue*>(self->real)->ExecuteCommandLists(count, real.data());
    if (first_calls(calls)) log("x2w-d3d12 CommandQueue::ExecuteCommandLists count=%u", count);
}

// Fence slot 10, desktop Signal(value) (also used by the DirectStorage stand-in's EnqueueSignal).
HRESULT __stdcall fence_signal(Wrapper* self, UINT64 value)
{
    const HRESULT hr = static_cast<ID3D12Fence*>(self->real)->Signal(value);
    fence_reached(self, value);
    return hr;
}
// Fence slot 8, desktop GetCompletedValue().
UINT64 __stdcall fence_completed_value(Wrapper* self) { return static_cast<UINT64>(self->completed); }
// Fence slot 12 (Xbox order): (value, ptr, flag) -> address of the completed value, which the title
// reads and waits on directly; all ones there means device removed.
volatile LONG64* __stdcall fence_completed_address(Wrapper* self, u64 value, u64 r8, u64 r9)
{
    static LONG calls;
    if (first_calls(calls, 2))
        log("x2w-d3d12 Fence::slot12(completed value address) value=0x%llx r8=0x%llx r9=0x%llx -> %p (now 0x%llx)", value, r8,
            r9, static_cast<void*>(const_cast<LONG64*>(&self->completed)), static_cast<unsigned long long>(self->completed));
    return &self->completed;
}

// Queue slot 18 (Xbox order): (UINT64* frequency) -> HRESULT; the title divides by the value.
HRESULT __stdcall queue_timestamp_frequency(Wrapper* self, UINT64* frequency)
{
    const HRESULT hr = static_cast<ID3D12CommandQueue*>(self->real)->GetTimestampFrequency(frequency);
    static LONG calls;
    if (first_calls(calls, 2))
        log("x2w-d3d12 CommandQueue::slot18(GetTimestampFrequency) -> 0x%08lx %llu", static_cast<unsigned long>(hr),
            frequency ? *frequency : 0ull);
    return hr;
}

// Queue slots whose Xbox meaning is unknown, dispatched by argument shape:
// - (fence wrapper, value): Signal on the desktop queue. Wait has the same shape; treating it as a
//   signal can complete a fence early but cannot leave the title waiting forever.
// - (count, array of command-list wrappers): ExecuteCommandLists (not submitted while lists are no-ops).
// Anything else logs its first call and returns 0. Each shape logs the slot it arrived on.
template <int N>
__declspec(noinline) u64 queue_dynamic(Wrapper* self, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    const bool log_it = first_calls(calls, 2);
    if (Wrapper* fence = as_wrapper(reinterpret_cast<void*>(rdx)); fence && fence->kind == fence_kind) {
        const HRESULT hr = static_cast<ID3D12CommandQueue*>(self->real)->Signal(static_cast<ID3D12Fence*>(fence->real), r8);
        fence_reached(fence, r8);
        if (log_it)
            log("x2w-d3d12 CommandQueue::slot%d(fence=%p, value=0x%llx) -> Signal 0x%08lx", N, static_cast<void*>(fence), r8,
                static_cast<unsigned long>(hr));
        return static_cast<u64>(hr);
    }
    if (rdx >= 1 && rdx <= 64 && r8 >= 0x10000 && x2w_local::readable(reinterpret_cast<void*>(r8), rdx * sizeof(void*))) {
        auto* const* lists = reinterpret_cast<void* const*>(r8);
        bool all_lists = true;
        for (u64 i = 0; i < rdx; ++i) {
            Wrapper* w = as_wrapper(lists[i]);
            all_lists &= w && w->kind == list_kind;
        }
        if (all_lists) {
            if (log_it) log("x2w-d3d12 CommandQueue::slot%d(count=%llu, lists) -> ExecuteCommandLists", N, rdx);
            queue_execute(self, static_cast<UINT>(rdx), lists);
            return 0;
        }
    }
    if (create_shaped(rdx, r8, r9, 0, 0, 0, 0)) {
        char name[64];
        std::snprintf(name, sizeof(name), "CommandQueue::slot%d(passes an IID)", N);
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, reinterpret_cast<u64>(self), rdx, r8, r9,
                       _ReturnAddress());
    }
    if (log_it) {
        char name[64];
        std::snprintf(name, sizeof(name), "CommandQueue::slot%d", N);
        x2w_trap::record("x2w-noop", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N & 0xFF, reinterpret_cast<u64>(self), rdx, r8,
                         r9, _ReturnAddress());
    }
    return 0;
}

template <std::size_t... I>
void fill_queue_dynamic(void** table, std::index_sequence<I...>)
{
    ((I >= 8 ? (table[I] = reinterpret_cast<void*>(&queue_dynamic<static_cast<int>(I)>)) : nullptr), ...);
}

// Queue GetDesc(), returned by value (desktop slot 18; Xbox slot unknown, not installed).
void* __stdcall queue_get_desc(void* a, void* b)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "CommandQueue::GetDesc(no receiver)");
    *static_cast<D3D12_COMMAND_QUEUE_DESC*>(result) = static_cast<ID3D12CommandQueue*>(self->real)->GetDesc();
    return result;
}

// Resource slot 8, desktop Map(subresource, readRange, data). On the console a resource's memory is
// title-visible: for a resource placed at a title address or backed by a hook allocation, the mapped
// pointer is that memory (subresource offsets inside it are not modeled). Others map on desktop.
HRESULT __stdcall resource_map(Wrapper* self, UINT subresource, const D3D12_RANGE* range, void** data)
{
    HRESULT hr = S_OK;
    void* mapped = self->title_memory;
    if (!mapped) hr = static_cast<ID3D12Resource*>(self->real)->Map(subresource, range, &mapped);
    if (data) *data = SUCCEEDED(hr) ? mapped : nullptr;
    if (FAILED(hr)) check_device("Resource::Map", 8);
    static LONG calls, failures;
    if (first_calls(calls, 4) || (FAILED(hr) && first_calls(failures, 8)))
        log("x2w-d3d12 Resource::Map subresource=%u title_memory=%p -> 0x%08lx %p", subresource, self->title_memory,
            static_cast<unsigned long>(hr), mapped);
    return hr;
}
void __stdcall resource_unmap(Wrapper* self, UINT subresource, const D3D12_RANGE* range)
{
    if (!self->title_memory) static_cast<ID3D12Resource*>(self->real)->Unmap(subresource, range);
}

// Resource slot 11, desktop GetGPUVirtualAddress() (called with no arguments). On the console a
// resource placed at an address has that address as its GPU address, so the title gets its own
// address back; resources without title memory report the desktop address (0 for textures).
UINT64 __stdcall resource_gpu_address(Wrapper* self)
{
    const UINT64 desktop = static_cast<ID3D12Resource*>(self->real)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
        ? static_cast<ID3D12Resource*>(self->real)->GetGPUVirtualAddress() : 0;
    const UINT64 result = self->title_memory ? reinterpret_cast<UINT64>(self->title_memory) : desktop;
    static LONG calls;
    if (first_calls(calls, 4))
        log("x2w-d3d12 Resource::GetGPUVirtualAddress title_memory=%p desktop=0x%llx -> 0x%llx", self->title_memory, desktop,
            result);
    return result;
}

// Resource slot 10, desktop GetDesc(), returned by value.
void* __stdcall resource_get_desc(void* a, void* b)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "Resource::GetDesc(no receiver)");
    *static_cast<D3D12_RESOURCE_DESC*>(result) = static_cast<ID3D12Resource*>(self->real)->GetDesc();
    return result;
}

// Heap slot 8, desktop GetDesc(), returned by value.
void* __stdcall heap_get_desc(void* a, void* b)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "Heap::GetDesc(no receiver)");
    *static_cast<D3D12_HEAP_DESC*>(result) = static_cast<ID3D12Heap*>(self->real)->GetDesc();
    return result;
}

// Descriptor heap slots 8-10, desktop GetDesc / GetCPU/GPUDescriptorHandleForHeapStart, by value.
void* __stdcall descriptor_heap_get_desc(void* a, void* b)
{
    void* result = nullptr;
    Wrapper* self = receiver(a, b, &result, "DescriptorHeap::GetDesc(no receiver)");
    *static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(result) = static_cast<ID3D12DescriptorHeap*>(self->real)->GetDesc();
    return result;
}
// The title returns structures of at most 8 bytes in rax (this in rcx), larger ones through a hidden
// pointer passed first. If the second argument is a wrapper, the hidden-pointer form is used instead.
template <class T, class Get>
std::uint64_t small_result(void* a, void* b, const char* method, Get get)
{
    if (Wrapper* w = as_wrapper(b)) {
        const T value = get(w);
        *static_cast<T*>(a) = value;
        return reinterpret_cast<std::uint64_t>(a);
    }
    Wrapper* w = as_wrapper(a);
    if (!w) fail(method, a, b);
    const T value = get(w);
    std::uint64_t bits = 0;
    static_assert(sizeof(T) <= sizeof(bits));
    std::memcpy(&bits, &value, sizeof(T));
    return bits;
}
std::uint64_t __stdcall descriptor_heap_cpu_start(void* a, void* b)
{
    return small_result<D3D12_CPU_DESCRIPTOR_HANDLE>(a, b, "DescriptorHeap::GetCPUDescriptorHandleForHeapStart(no receiver)",
        [](Wrapper* w) {
            if (w->title_cpu) return D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(w->title_cpu)};
            return static_cast<ID3D12DescriptorHeap*>(w->real)->GetCPUDescriptorHandleForHeapStart();
        });
}

// Device slot 14, desktop CreateDescriptorHeap(desc, riid, ppv). The title's descriptor is longer:
// +0x10 a title CPU address for the descriptors (or null) and +0x18 their size in bytes. With an
// address, GetCPUDescriptorHandleForHeapStart must return it (the title asserts this).
HRESULT __stdcall device_create_descriptor_heap(Wrapper* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const GUID* iid,
                                                void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    const auto* extra = reinterpret_cast<const std::uint64_t*>(desc + 1);
    ID3D12DescriptorHeap* heap = nullptr;
    const HRESULT hr = kind_for(*iid) == descriptor_heap_kind
        ? device_of(self)->CreateDescriptorHeap(desc, IID_PPV_ARGS(&heap)) : E_NOINTERFACE;
    const std::uint64_t desktop = heap ? heap->GetCPUDescriptorHandleForHeapStart().ptr : 0;
    static LONG calls;
    if (first_calls(calls, 16))
        log("x2w-d3d12 Device::CreateDescriptorHeap type=%d count=%u flags=0x%x node=0x%x title=0x%llx bytes=0x%llx riid=%s "
            "-> 0x%08lx desktop=0x%llx", desc->Type, desc->NumDescriptors, desc->Flags, desc->NodeMask, extra[0], extra[1],
            text, static_cast<unsigned long>(hr), desktop);
    if (FAILED(hr)) return hr;
    Wrapper* w = wrap(heap, descriptor_heap_kind);
    const std::uint64_t bytes = static_cast<std::uint64_t>(desc->NumDescriptors) *
                                device_of(self)->GetDescriptorHandleIncrementSize(desc->Type);
    // Some callers pass the 16-byte desktop descriptor; the words after it are then unrelated stack
    // data. The extension is taken only when it holds a pointer and exactly the heap's size.
    if (extra[0] >= 0x10000 && extra[1] == bytes) {
        w->title_cpu = extra[0];
        AcquireSRWLockExclusive(&descriptor_lock);
        descriptor_ranges.push_back({extra[0], bytes, desktop});
        ReleaseSRWLockExclusive(&descriptor_lock);
    }
    *out = w;
    return S_OK;
}

// Device view slots: forwarded with the destination CPU handle (argument D, 0-based after the
// object) translated from title to desktop.
// Desktop removes the device (DXGI_ERROR_INVALID_CALL) on view calls it rejects, so they are checked
// first and skipped with a log: a view type the desktop resource's flags do not allow (the console
// makes unordered-access views of depth textures), and constant-buffer views, whose BufferLocation is
// a title GPU address. Console format values in the view description are mapped like resource formats.
template <int N, int D>
__declspec(noinline) u64 view_slot(Wrapper* self, u64 a, u64 b, u64 c, u64 d)
{
    u64 args[4] = {unwrap(a), unwrap(b), unwrap(c), unwrap(d)};
    args[D] = desktop_cpu_handle(args[D]);
    const char* skip = nullptr;
    if (N == 17) skip = "constant buffer view (title GPU address)";
    if (N >= 18 && N <= 21 && args[0]) {
        const UINT flags = reinterpret_cast<ID3D12Resource*>(args[0])->GetDesc().Flags;
        if (N == 18 && (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) skip = "resource denies shader resource views";
        if (N == 19 && !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) skip = "resource lacks unordered access";
        if (N == 20 && !(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) skip = "resource lacks render target";
        if (N == 21 && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) skip = "resource lacks depth stencil";
    }
    if (skip) {
        static LONG skipped;
        if (first_calls(skipped, 6)) log("x2w-d3d12 Device::slot%d view skipped: %s", N, skip);
        return 0;
    }
    // The view descriptions of slots 18-21 start with a DXGI_FORMAT.
    unsigned char desc_copy[64];
    const int desc_index = N == 19 ? 2 : 1;
    if (N >= 18 && N <= 21 && args[desc_index] && x2w_local::readable(reinterpret_cast<const void*>(args[desc_index]), sizeof(desc_copy))) {
        std::memcpy(desc_copy, reinterpret_cast<const void*>(args[desc_index]), sizeof(desc_copy));
        DXGI_FORMAT format;
        std::memcpy(&format, desc_copy, sizeof(format));
        const DXGI_FORMAT mapped = desktop_format(format);
        if (mapped != format) {
            std::memcpy(desc_copy, &mapped, sizeof(mapped));
            args[desc_index] = reinterpret_cast<u64>(desc_copy);
        }
    }
    const u64 result = method_of(self->real, N)(self->real, args[0], args[1], args[2], args[3], 0, 0, 0, 0);
    check_device("Device view", N);
    static LONG calls;
    if (first_calls(calls))
        log("x2w-d3d12 Device::slot%d view(0x%llx, 0x%llx, 0x%llx, 0x%llx) dest 0x%llx -> 0x%llx", N, a, b, c, d,
            D == 0 ? a : D == 1 ? b : D == 2 ? c : d, args[D]);
    return result;
}

// Device slot 24, desktop CopyDescriptorsSimple(count, dest, src, type).
void __stdcall device_copy_descriptors_simple(Wrapper* self, UINT count, u64 dest, u64 src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    device_of(self)->CopyDescriptorsSimple(count, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(desktop_cpu_handle(dest))},
                                           D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(desktop_cpu_handle(src))}, type);
    check_device("Device CopyDescriptorsSimple", 24);
    static LONG calls;
    if (first_calls(calls)) log("x2w-d3d12 Device::CopyDescriptorsSimple count=%u dest=0x%llx src=0x%llx type=%d", count, dest, src, type);
}

// Device slot 23, desktop CopyDescriptors(destCount, destStarts, destSizes, srcCount, srcStarts, srcSizes, type).
void __stdcall device_copy_descriptors(Wrapper* self, UINT dest_count, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                                       const UINT* dest_sizes, UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                       const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dest(dest_count), src(src_count);
    for (UINT i = 0; i < dest_count; ++i) dest[i].ptr = static_cast<SIZE_T>(desktop_cpu_handle(dest_starts[i].ptr));
    for (UINT i = 0; i < src_count; ++i) src[i].ptr = static_cast<SIZE_T>(desktop_cpu_handle(src_starts[i].ptr));
    device_of(self)->CopyDescriptors(dest_count, dest.data(), dest_sizes, src_count, src.data(), src_sizes, type);
    check_device("Device CopyDescriptors", 23);
    static LONG calls;
    if (first_calls(calls)) log("x2w-d3d12 Device::CopyDescriptors dest=%u src=%u type=%d", dest_count, src_count, type);
}
std::uint64_t __stdcall descriptor_heap_gpu_start(void* a, void* b)
{
    return small_result<D3D12_GPU_DESCRIPTOR_HANDLE>(a, b, "DescriptorHeap::GetGPUDescriptorHandleForHeapStart(no receiver)",
        [](Wrapper* w) { return static_cast<ID3D12DescriptorHeap*>(w->real)->GetGPUDescriptorHandleForHeapStart(); });
}
std::uint64_t __stdcall device_adapter_luid(void* a, void* b)
{
    return small_result<LUID>(a, b, "Device::GetAdapterLuid(no receiver)",
                              [](Wrapper* w) { return device_of(w)->GetAdapterLuid(); });
}

// Device slot 73 (Xbox order): (desc*, allocator, pso, riid, ppv) with a 16-byte descriptor
// (observed type 0/2 at +0, then 2, 1, 0) and an Xbox-only list IID. Creates a closed desktop
// list of the descriptor's type on the allocator so object methods have a real target; its Xbox methods are traps,
// or no-ops with X2W_D3D12_LIST_NOOP=1.
HRESULT __stdcall device_create_command_list_x(Wrapper* self, const void* desc, void* allocator, void* pso,
                                               const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    char bytes[3 * 16 + 1];
    hex_text(desc, 16, bytes, sizeof(bytes));
    if (!out || !iid) return E_POINTER;
    *out = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT hr = kind_for(*iid) == list_kind
        ? device_of(self)->CreateCommandList(0, desc ? *static_cast<const D3D12_COMMAND_LIST_TYPE*>(desc)
                                                     : D3D12_COMMAND_LIST_TYPE_DIRECT,
              reinterpret_cast<ID3D12CommandAllocator*>(unwrap(reinterpret_cast<u64>(allocator))),
              reinterpret_cast<ID3D12PipelineState*>(unwrap(reinterpret_cast<u64>(pso))), IID_PPV_ARGS(&list))
        : E_NOINTERFACE;
    if (SUCCEEDED(hr)) list->Close();
    static LONG calls;
    if (first_calls(calls, 4))
        log("x2w-d3d12 Device::slot73(CreateCommandList?) desc=[%s] allocator=%p pso=%p riid=%s -> 0x%08lx", bytes,
            allocator, pso, text, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    *out = wrap(list, list_kind);
    return S_OK;
}

// Device slots 78 and 79 (Xbox order): frame pacing setup. 78 receives (0, 33333, 3, 0), a 30 Hz
// frame length in microseconds and a period; 79 receives (0, offset, 0, 0). The title tolerates
// failure. Recorded and accepted; nothing paces frames yet.
UINT frame_interval_us, frame_period;
HRESULT __stdcall device_frame_interval(Wrapper*, void* output, UINT length_us, UINT period, UINT flags)
{
    frame_interval_us = length_us;
    frame_period = period;
    log("x2w-d3d12 Device::slot78(frame interval?) output=%p length=%u us period=%u flags=0x%x -> S_OK", output, length_us,
        period, flags);
    return S_OK;
}
HRESULT __stdcall device_frame_event(Wrapper*, UINT type, UINT offset_us, void* reserved, UINT flags)
{
    log("x2w-d3d12 Device::slot79(frame event?) type=%u offset=%u us reserved=%p flags=0x%x -> S_OK", type, offset_us,
        reserved, flags);
    return S_OK;
}

// Device slot 133 (Xbox order): (address, D3D12_RESOURCE_DESC*, state, clear, riid, ppv), a resource
// placed at a title address. First seen with a row-major buffer desc (flags 0x20004) over a
// placeholder-replaced XMem region, state 0xffffffff, a null clear value and the ID3D12Resource IID.
// Desktop: the allocation containing the address is opened as an existing heap (cached) and a
// cross-adapter buffer is placed at the address's offset. Its desktop GPU address differs from the
// title address; nothing translates title GPU addresses yet. Textures trap.
std::mutex placed_mutex;
std::vector<std::pair<void*, ID3D12Heap*>> existing_heaps;

HRESULT __stdcall device_place_resource_x(Wrapper* self, void* address, const D3D12_RESOURCE_DESC* desc, UINT state,
                                          const void* clear, const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    check_device("Device before slot", 133);
    if (kind_for(*iid) != resource_kind) return E_NOINTERFACE;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Textures cannot be placed on title memory on desktop: a committed default-heap texture
        // stands in, not aliased with the title address. The clear value is dropped if refused.
        const D3D12_RESOURCE_DESC texture = sanitized(*desc);
        const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        const auto* clear_value = static_cast<const D3D12_CLEAR_VALUE*>(clear);
        ID3D12Resource* created = nullptr;
        HRESULT texture_hr = device_of(self)->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture,
                                                                      D3D12_RESOURCE_STATE_COMMON, clear_value,
                                                                      IID_PPV_ARGS(&created));
        if (FAILED(texture_hr) && clear_value)
            texture_hr = device_of(self)->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture,
                                                                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&created));
        static LONG textures, failures;
        if (first_calls(textures, 6) || (FAILED(texture_hr) && first_calls(failures, 8))) {
            char bytes[3 * 0x38 + 1];
            hex_text(desc, 0x38, bytes, sizeof(bytes));
            log("x2w-d3d12 Device::slot133(CreatePlacedResourceX?) texture address=%p dim=%d %llux%ux%u mips=%u fmt=%d "
                "samples=%u flags=0x%x layout=0x%x state=0x%x clear=%p -> 0x%08lx not aliased [%s]", address, desc->Dimension,
                desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format, desc->SampleDesc.Count,
                desc->Flags, desc->Layout, state, clear, static_cast<unsigned long>(texture_hr),
                FAILED(texture_hr) ? bytes : "");
        }
        if (FAILED(texture_hr)) return texture_hr;
        Wrapper* w = wrap(created, resource_kind);
        w->title_memory = address;
        *out = w;
        return S_OK;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!address || VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
        log("x2w-d3d12 Device::slot133(CreatePlacedResourceX?) address=%p not committed -> E_INVALIDARG", address);
        return E_INVALIDARG;
    }
    void* base = info.AllocationBase;
    const UINT64 offset = static_cast<char*>(address) - static_cast<char*>(base);
    ID3D12Heap* heap = nullptr;
    HRESULT hr = S_OK;
    {
        std::lock_guard lock(placed_mutex);
        for (auto& [known, h] : existing_heaps)
            if (known == base) heap = h;
        if (!heap) {
            ID3D12Device3* device3 = nullptr;
            hr = device_of(self)->QueryInterface(IID_PPV_ARGS(&device3));
            if (SUCCEEDED(hr)) {
                hr = device3->OpenExistingHeapFromAddress(base, IID_PPV_ARGS(&heap));
                device3->Release();
            }
            if (SUCCEEDED(hr)) existing_heaps.emplace_back(base, heap);
        }
    }
    D3D12_RESOURCE_DESC copy = *desc;
    copy.Flags = static_cast<D3D12_RESOURCE_FLAGS>((sanitized(*desc).Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
                                                   D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    ID3D12Resource* resource = nullptr;
    if (SUCCEEDED(hr))
        hr = device_of(self)->CreatePlacedResource(heap, offset, &copy, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&resource));
    // A buffer that spans several allocations (adjacent placeholder pieces) cannot be placed on one
    // existing heap. Fallback: a committed default-heap buffer, not aliased with the title memory.
    const char* placement = "aliased";
    if (FAILED(hr)) {
        const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        copy.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device_of(self)->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &copy, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, IID_PPV_ARGS(&resource));
        placement = "not aliased";
    }
    log("x2w-d3d12 Device::slot133(CreatePlacedResourceX?) address=%p base=%p offset=0x%llx width=0x%llx flags=0x%x "
        "state=0x%x clear=%p riid=%s -> 0x%08lx %s gpuva=0x%llx", address, base, offset, desc->Width, desc->Flags, state,
        clear, text, static_cast<unsigned long>(hr), placement, resource ? resource->GetGPUVirtualAddress() : 0ull);
    if (FAILED(hr)) return hr;
    Wrapper* w = wrap(resource, resource_kind);
    w->title_memory = address;
    *out = w;
    return S_OK;
}

// The title exports __XMemAllocationHooks__ = {alloc(size, attributes), free(ptr, attributes), ...}.
// Its alloc routine selects a hook chain by allocatorId (attribute bits 16-23). Around the creation
// of its two display render targets the title pushes a bump allocator for allocatorId 0x80 that
// serves pageSize 2 (bits 14-15) from a 64 MiB block, then asserts that the block was used up:
// the console runtime evidently takes committed-resource memory from XMemAlloc with allocatorId
// 0x80. Hypothesis used here: attributes = pageSize (2 from 4 MiB up, else 1) << 14 | 0x80 << 16 |
// log2(alignment) << 24, memory type 0. The block is bookkeeping only; the desktop resource has its
// own memory and does not alias it.
using XMemAllocHook = void*(__stdcall*)(SIZE_T, ULONGLONG);
XMemAllocHook title_alloc_hook()
{
    static const XMemAllocHook hook = [] {
        auto* const* table = reinterpret_cast<void* const*>(GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__"));
        return table ? reinterpret_cast<XMemAllocHook>(table[0]) : nullptr;
    }();
    return hook;
}

void* title_resource_memory(UINT64 size, UINT64 alignment)
{
    const XMemAllocHook hook = title_alloc_hook();
    if (!hook || !size) return nullptr;
    unsigned log2_alignment = 0;
    while ((UINT64{1} << (log2_alignment + 1)) <= alignment && log2_alignment < 31) ++log2_alignment;
    const ULONGLONG page = size >= (UINT64{4} << 20) ? 2 : 1;
    const ULONGLONG attributes = (page << 14) | (ULONGLONG{0x80} << 16) | (ULONGLONG{log2_alignment} << 24);
    void* memory = hook(static_cast<SIZE_T>(size), attributes);
    static LONG calls;
    if (first_calls(calls, 12))
        log("x2w-d3d12 title XMem hook alloc size=0x%llx attributes=0x%llx -> %p", size, attributes, memory);
    return memory;
}

// Placeholder objects (no desktop object behind the wrapper): ID3D12Object methods succeed and do nothing.
HRESULT __stdcall placeholder_ok(Wrapper*, u64, u64, u64) { return S_OK; }

// Device slot 62 (Xbox order): (D3D12_COMPUTE_PIPELINE_STATE_DESC*, 0, 0, riid, ppv); first seen from
// XboxBvhBuilder.dll, followed by SetName on the result. The bytecode is console GPU code, not DXIL,
// so no desktop pipeline is built: the result is a placeholder PipelineState until shader translation
// exists. Logs the root signature, bytecode address, length and first 32 bytes.
HRESULT __stdcall device_create_compute_pipeline_x(Wrapper*, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, u64 extra0,
                                                   u64 extra1, const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    char bytes[3 * 32 + 1];
    hex_text(desc->CS.pShaderBytecode, desc->CS.BytecodeLength < 32 ? desc->CS.BytecodeLength : 32, bytes, sizeof(bytes));
    static LONG calls;
    if (first_calls(calls, 24))
        log("x2w-d3d12 Device::slot62(CreateComputePipelineState?) root=%p cs=%p len=0x%llx node=0x%x flags=0x%x extra=0x%llx,0x%llx "
            "riid=%s -> placeholder [%s]", static_cast<void*>(desc->pRootSignature), desc->CS.pShaderBytecode,
            static_cast<unsigned long long>(desc->CS.BytecodeLength), desc->NodeMask, desc->Flags, extra0, extra1, text, bytes);
    if (kind_for(*iid) != pipeline_kind) return E_NOINTERFACE;
    *out = wrap(nullptr, pipeline_kind);
    return S_OK;
}

// Device slot 86 (Xbox order): (D3D12_PIPELINE_STATE_STREAM_DESC*, riid, ppv), the desktop
// CreatePipelineState shape (stream of 0x2b0 bytes first seen, ID3D12PipelineState IID). The stream
// carries console shaders; the result is a placeholder PipelineState.
HRESULT __stdcall device_create_pipeline_stream_x(Wrapper*, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, const GUID* iid,
                                                  void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !desc || !iid) return E_POINTER;
    *out = nullptr;
    static LONG calls;
    if (first_calls(calls, 8))
        log("x2w-d3d12 Device::slot86(CreatePipelineState?) stream=%p bytes=0x%llx riid=%s -> placeholder",
            desc->pPipelineStateSubobjectStream, static_cast<unsigned long long>(desc->SizeInBytes), text);
    if (kind_for(*iid) != pipeline_kind) return E_NOINTERFACE;
    *out = wrap(nullptr, pipeline_kind);
    return S_OK;
}

// Device slots 57 and 67 (Xbox order): (desc*, riid, ppv) with the ID3D12PipelineState IID; pipeline
// descriptions whose layout is not decoded yet. The result is a placeholder PipelineState.
template <int N>
HRESULT __stdcall device_create_pipeline_placeholder(Wrapper*, const void* desc, const GUID* iid, void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !iid) return E_POINTER;
    *out = nullptr;
    static LONG calls;
    if (first_calls(calls, 2)) {
        char bytes[3 * 96 + 1];
        hex_text(desc, 96, bytes, sizeof(bytes));
        log("x2w-d3d12 Device::slot%d(pipeline?) desc=[%s] riid=%s -> placeholder", N, bytes, text);
    }
    if (kind_for(*iid) != pipeline_kind) return E_NOINTERFACE;
    *out = wrap(nullptr, pipeline_kind);
    return S_OK;
}

// Device slot 55 (Xbox order): (existing object, 1, desc*, riid, ppv) with the ID3D12PipelineState
// IID: a pipeline derived from an existing one. Placeholder PipelineState, like the other creators.
HRESULT __stdcall device_create_derived_pipeline(Wrapper*, void* base, u64 count, const void* desc, const GUID* iid,
                                                 void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !iid) return E_POINTER;
    *out = nullptr;
    static LONG calls;
    if (first_calls(calls, 3)) {
        Wrapper* w = as_wrapper(base);
        char bytes[3 * 64 + 1];
        hex_text(desc, 64, bytes, sizeof(bytes));
        log("x2w-d3d12 Device::slot55(derived pipeline?) base=%p (%s) count=%llu desc=[%s] riid=%s -> placeholder", base,
            w ? kind_names[w->kind] : "not a wrapper", count, bytes, text);
    }
    if (kind_for(*iid) != pipeline_kind) return E_NOINTERFACE;
    *out = wrap(nullptr, pipeline_kind);
    return S_OK;
}

// Device slot 127 (Xbox order): ten arguments in the CreateCommittedResource3 order: heap properties,
// heap flags (8, ALLOW_DISPLAY, for the 4K render targets), resource desc, an initial state or
// layout, clear value, then three zero words (protected session, castable count and formats in the
// desktop method), riid and ppv. Desktop: CreateCommittedResource with the sanitized desc in
// COMMON; when desktop rejects the clear value it is retried without one.
HRESULT __stdcall device_create_committed_x(Wrapper* self, const D3D12_HEAP_PROPERTIES* props, UINT heap_flags,
                                            const D3D12_RESOURCE_DESC* desc, UINT state, const D3D12_CLEAR_VALUE* clear,
                                            void* session, UINT castable_count, const void* castable, const GUID* iid,
                                            void** out)
{
    char text[48];
    guid_text(iid, text, sizeof(text));
    if (!out || !props || !desc || !iid) return E_POINTER;
    *out = nullptr;
    const D3D12_RESOURCE_DESC copy = sanitized(*desc);
    const auto flags = static_cast<D3D12_HEAP_FLAGS>(heap_flags);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_of(self)->GetResourceAllocationInfo(0, 1, &copy);
    check_device("Device before slot", 127);
    void* const title_memory = info.SizeInBytes != ~0ull ? title_resource_memory(info.SizeInBytes, info.Alignment) : nullptr;
    ID3D12Resource* resource = nullptr;
    HRESULT hr = kind_for(*iid) == resource_kind
        ? device_of(self)->CreateCommittedResource(props, flags, &copy, D3D12_RESOURCE_STATE_COMMON, clear,
                                                   IID_PPV_ARGS(&resource))
        : E_NOINTERFACE;
    bool cleared = clear != nullptr;
    if (FAILED(hr) && clear && kind_for(*iid) == resource_kind) {
        hr = device_of(self)->CreateCommittedResource(props, flags, &copy, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&resource));
        cleared = false;
    }
    static LONG calls;
    if (first_calls(calls, 12))
        log("x2w-d3d12 Device::slot127(CreateCommittedResource3?) heap=%d flags=0x%x dim=%d %llux%u fmt=%d rflags=0x%x "
            "state=0x%x clear=%p%s session=%p castable=%u/%p riid=%s -> 0x%08lx", props->Type, heap_flags, desc->Dimension,
            desc->Width, desc->Height, desc->Format, desc->Flags, state, static_cast<const void*>(clear),
            clear && !cleared ? "(dropped)" : "", session, castable_count, castable, text, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    Wrapper* w = wrap(resource, resource_kind);
    w->title_memory = title_memory;
    *out = w;
    return S_OK;
}

template <int K, int N>
void enable_forward() { tables[K][N] = reinterpret_cast<void*>(&forward_slot<K, N>); }
template <int K, int N, int R, int Desc = -1>
void enable_create() { tables[K][N] = reinterpret_cast<void*>(&create_slot<K, N, R, Desc>); }
template <int K>
void enable_object()
{
    // ID3D12Object: GetPrivateData, SetPrivateData, SetPrivateDataInterface, SetName.
    enable_forward<K, 3>();
    enable_forward<K, 4>();
    enable_forward<K, 5>();
    enable_forward<K, 6>();
    if (K != device_kind) tables[K][7] = reinterpret_cast<void*>(&child_get_device);
}
template <std::size_t... K>
void enable_objects(std::index_sequence<K...>) { (enable_object<static_cast<int>(K)>(), ...); }

void build_tables()
{
    fill_all_traps(std::make_index_sequence<kind_count>{});
    for (auto& table : tables) {
        table[0] = reinterpret_cast<void*>(&query_interface);
        table[1] = reinterpret_cast<void*>(&add_ref);
        table[2] = reinterpret_cast<void*>(&release);
    }
    enable_objects(std::make_index_sequence<kind_count>{});

    // Device, desktop ID3D12Device order (slots 7-47).
    enable_forward<device_kind, 7>();                    // GetNodeCount
    tables[device_kind][8] = reinterpret_cast<void*>(&device_create_command_queue);
    enable_create<device_kind, 9, 1>();                  // CreateCommandAllocator(type, riid, ppv)
    tables[device_kind][10] = reinterpret_cast<void*>(&device_create_pipeline<10>);
    tables[device_kind][11] = reinterpret_cast<void*>(&device_create_pipeline<11>);
    enable_create<device_kind, 12, 4>();                 // CreateCommandList(node, type, allocator, pso, riid, ppv)
    enable_forward<device_kind, 13>();                   // CheckFeatureSupport
    tables[device_kind][14] = reinterpret_cast<void*>(&device_create_descriptor_heap);
    enable_forward<device_kind, 15>();                   // GetDescriptorHandleIncrementSize
    enable_create<device_kind, 16, 3>();                 // CreateRootSignature(node, blob, size, riid, ppv)
    tables[device_kind][17] = reinterpret_cast<void*>(&view_slot<17, 1>);  // CreateConstantBufferView(desc, dest)
    tables[device_kind][18] = reinterpret_cast<void*>(&view_slot<18, 2>);  // CreateShaderResourceView(res, desc, dest)
    tables[device_kind][19] = reinterpret_cast<void*>(&view_slot<19, 3>);  // CreateUnorderedAccessView(res, counter, desc, dest)
    tables[device_kind][20] = reinterpret_cast<void*>(&view_slot<20, 2>);  // CreateRenderTargetView(res, desc, dest)
    tables[device_kind][21] = reinterpret_cast<void*>(&view_slot<21, 2>);  // CreateDepthStencilView(res, desc, dest)
    tables[device_kind][22] = reinterpret_cast<void*>(&view_slot<22, 1>);  // CreateSampler(desc, dest)
    tables[device_kind][23] = reinterpret_cast<void*>(&device_copy_descriptors);
    tables[device_kind][24] = reinterpret_cast<void*>(&device_copy_descriptors_simple);
    tables[device_kind][25] = reinterpret_cast<void*>(&device_resource_allocation_info);
    tables[device_kind][26] = reinterpret_cast<void*>(&device_custom_heap_properties);
    enable_create<device_kind, 27, 5, 2>();                 // CreateCommittedResource(props, flags, desc, state, clear, riid, ppv)
    enable_create<device_kind, 28, 1>();                 // CreateHeap(desc, riid, ppv)
    enable_create<device_kind, 29, 5, 2>();                 // CreatePlacedResource(heap, offset, desc, state, clear, riid, ppv)
    enable_create<device_kind, 30, 3, 0>();                 // CreateReservedResource(desc, state, clear, riid, ppv)
    tables[device_kind][34] = reinterpret_cast<void*>(&device_residency<34>);
    tables[device_kind][35] = reinterpret_cast<void*>(&device_residency<35>);
    enable_create<device_kind, 36, 2>();                 // CreateFence(initial, flags, riid, ppv)
    enable_forward<device_kind, 37>();                   // GetDeviceRemovedReason
    tables[device_kind][38] = reinterpret_cast<void*>(&device_copyable_footprints);
    enable_create<device_kind, 39, 1>();                 // CreateQueryHeap(desc, riid, ppv)
    enable_forward<device_kind, 40>();                   // SetStablePowerState
    tables[device_kind][41] = reinterpret_cast<void*>(&device_create_command_signature);                 // CreateCommandSignature(desc, root, riid, ppv)
    enable_forward<device_kind, 42>();                   // GetResourceTiling
    // Slot 43 (desktop GetAdapterLuid) is not installed: no call site has confirmed it.
    (void)&device_adapter_luid;
    // Xbox order from here.
    tables[device_kind][48] = reinterpret_cast<void*>(&device_create_command_queue_x);
    tables[device_kind][73] = reinterpret_cast<void*>(&device_create_command_list_x);
    tables[device_kind][78] = reinterpret_cast<void*>(&device_frame_interval);
    tables[device_kind][79] = reinterpret_cast<void*>(&device_frame_event);
    tables[device_kind][55] = reinterpret_cast<void*>(&device_create_derived_pipeline);
    tables[device_kind][57] = reinterpret_cast<void*>(&device_create_pipeline_placeholder<57>);
    tables[device_kind][67] = reinterpret_cast<void*>(&device_create_pipeline_placeholder<67>);
    tables[device_kind][62] = reinterpret_cast<void*>(&device_create_compute_pipeline_x);
    tables[device_kind][86] = reinterpret_cast<void*>(&device_create_pipeline_stream_x);
    for (int slot = 3; slot <= 6; ++slot) tables[pipeline_kind][slot] = reinterpret_cast<void*>(&placeholder_ok);
    for (int slot = 3; slot <= 6; ++slot) tables[command_signature_kind][slot] = reinterpret_cast<void*>(&placeholder_ok);
    tables[device_kind][108] = reinterpret_cast<void*>(&device_resource_allocation_info1);
    tables[device_kind][127] = reinterpret_cast<void*>(&device_create_committed_x);
    tables[device_kind][133] = reinterpret_cast<void*>(&device_place_resource_x);
    if (device_noop()) fill_device_noops(tables[device_kind], std::make_index_sequence<slot_count>{});
    if (list_noop()) {
        fill_noops<list_kind>(tables[list_kind], std::make_index_sequence<slot_count>{});
        tables[list_kind][7] = reinterpret_cast<void*>(&child_get_device);
    }

    // Queue: not the desktop order (slot 18 is GetTimestampFrequency, desktop 16). Slots are
    // dispatched by argument shape until the order is known; see queue_dynamic.
    fill_queue_dynamic(tables[queue_kind], std::make_index_sequence<64>{});
    tables[queue_kind][18] = reinterpret_cast<void*>(&queue_timestamp_frequency);

    // Fence: GetCompletedValue, SetEventOnCompletion, Signal, GetCreationFlags.
    tables[fence_kind][8] = reinterpret_cast<void*>(&fence_completed_value);
    // Desktop slots 9 (SetEventOnCompletion) and 11 (GetCreationFlags) wait for a confirming call site.
    tables[fence_kind][10] = reinterpret_cast<void*>(&fence_signal);
    tables[fence_kind][12] = reinterpret_cast<void*>(&fence_completed_address);

    enable_forward<allocator_kind, 8>();                 // Reset

    // Resource: Map, Unmap, GetDesc, GetGPUVirtualAddress, Write/ReadFromSubresource, GetHeapProperties.
    tables[resource_kind][8] = reinterpret_cast<void*>(&resource_map);
    tables[resource_kind][9] = reinterpret_cast<void*>(&resource_unmap);
    tables[resource_kind][11] = reinterpret_cast<void*>(&resource_gpu_address);
    // Desktop Resource 10 (GetDesc), 12-14, Heap 8 and DescriptorHeap 8 (GetDesc, struct returns) are
    // not installed until a call site confirms them: the queue's slot 18 showed what a wrong one does.
    (void)&resource_get_desc;
    (void)&heap_get_desc;
    (void)&descriptor_heap_get_desc;
    tables[descriptor_heap_kind][9] = reinterpret_cast<void*>(&descriptor_heap_cpu_start);
    tables[descriptor_heap_kind][10] = reinterpret_cast<void*>(&descriptor_heap_gpu_start);
    if (device_noop()) fill_all_remaining_noops(std::make_index_sequence<kind_count>{});
}

}  // namespace

bool x2w_d3d12_permissive()
{
    char value[4]{};
    return GetEnvironmentVariableA("X2W_D3D12_PERMISSIVE", value, sizeof(value)) && value[0] == '1';
}

HRESULT x2w_d3d12_permissive_create_device(IUnknown* adapter, const void* parameters, REFIID iid, void** out)
{
    static const bool ready = (build_tables(), true);
    (void)ready;
    char text[48];
    guid_text(&iid, text, sizeof(text));
    char block[3 * 0x40 + 1];
    hex_text(parameters, 0x40, block, sizeof(block));
    if (!out) return E_POINTER;
    *out = nullptr;
    const bool debug = [] {
        char value[4]{};
        return GetEnvironmentVariableA("X2W_D3D12_DEBUG", value, sizeof(value)) && value[0] == '1';
    }();
    if (debug) {
        ID3D12Debug* layer = nullptr;
        const HRESULT debug_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&layer));
        if (SUCCEEDED(debug_hr)) layer->EnableDebugLayer();
        log("x2w-d3d12 debug layer: 0x%08lx%s", static_cast<unsigned long>(debug_hr),
            SUCCEEDED(debug_hr) ? " enabled" : " (not installed?)");
    }
    ID3D12Device* device = nullptr;
    const HRESULT hr = kind_for(iid) == device_kind || iid == __uuidof(IUnknown)
        ? D3D12CreateDevice(adapter ? reinterpret_cast<IUnknown*>(unwrap(reinterpret_cast<u64>(adapter))) : nullptr,
                            D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))
        : E_NOINTERFACE;
    log("x2w-d3d12 D3D12XboxCreateDevice adapter=%p params=[%s] riid=%s -> 0x%08lx", static_cast<void*>(adapter), block,
        text, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return hr;
    removal_device = device;
    if (debug) device->QueryInterface(IID_PPV_ARGS(&info_queue));
    the_device = wrap(device, device_kind);
    *out = the_device;
    return S_OK;
}
