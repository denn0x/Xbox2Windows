// Local experiment (never upstream): proxy between the title and the system gaming runtime.
//
// The title's static runtime library loads the system xgameruntime.dll and asks it for API objects
// with QueryApiImpl(provider GUID, interface GUID, &object). With X2W_RUNTIME_PROXY=1 this file (built
// into the GameInput stand-in, which the title imports) patches the title's import of GetProcAddress
// so that the lookup of "QueryApiImpl" returns a wrapper. The wrapper hands out proxy objects whose
// methods log their first calls ("x2w-runtime") and forward to the real object.
//
// X2W_FAKE_USER=1 additionally answers the user API for one local user (see fake_user below).
#include "startup_stubs/local_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <utility>

namespace {
using u64 = std::uint64_t;
using Method = u64(__stdcall*)(void*, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
constexpr int max_providers = 24, max_slots = 96;

struct Proxy {
    void** vtable;
    void* real;
    int provider;
};
GUID provider_ids[max_providers];
Proxy* proxies[max_providers];
int provider_count;
LONG call_counts[max_providers][max_slots];
SRWLOCK proxy_lock = SRWLOCK_INIT;
void* slot_table[max_slots];

using QueryApi = HRESULT(__stdcall*)(const GUID*, const GUID*, void**);
QueryApi real_query;

// ---- local user (X2W_FAKE_USER=1) ----------------------------------------------------------
// The user API object is the provider whose GUID starts 01acd177 (seen: called at start-up and when
// the sign-in button is pressed). Slots are filled in from observed calls; see user_call below.
constexpr unsigned long user_provider_data1 = 0x01acd177;
constexpr u64 local_user_handle = 0x58325755'00000001ull;  // not a pointer the runtime knows
constexpr u64 local_user_id = 0x0009000012345678ull;
bool fake_user() noexcept { return x2w_local::env_on("X2W_FAKE_USER"); }
int user_provider = -1, save_provider = -1;
// The user provider also hands out a second interface (iid cef4fac0...) whose slot 3 is
// GetGamertag(handle, component, size, buffer, &used). It is proxied as an entry of its own (shown as
// provider cef4fac0), not as the main user object, whose slot 3 duplicates a handle.
int gamertag_provider = -1;
constexpr unsigned long gamertag_iid_data1 = 0xcef4fac0;
// Async blocks completed on behalf of a local answer (see complete_async): calls that mention them
// are always logged, and the borrowed operation's own failure code is not passed on for them.
u64 tracked_blocks[64];
volatile LONG tracked_count, tracked_logged;
constexpr u64 borrowed_failure = 0x89245110;
bool tracked(u64 value)
{
    if (!value) return false;
    for (LONG i = 0; i < tracked_count && i < 64; ++i)
        if (tracked_blocks[i] == value) return true;
    return false;
}
bool user_call(int slot, u64* args, u64* result);
bool save_call(int slot, u64* args, u64* result);
bool area_call(unsigned long provider, int slot, u64* args, u64* result);
u64 complete_async(u64 async_block);
void* storage_object();
constexpr unsigned long save_provider_data1 = 0x704c3f58;  // hypothesis: the game-save API (first seen: slot 4 with the user handle)

template <int N>
__declspec(noinline) u64 forward(Proxy* self, u64 a, u64 b, u64 c, u64 s0, u64 s1, u64 s2, u64 s3, u64 s4, u64 s5, u64 s6, u64 s7)
{
    const bool log_it = N >= 3 && InterlockedIncrement(&call_counts[self->provider][N]) <= 6;
    u64 args[11] = {a, b, c, s0, s1, s2, s3, s4, s5, s6, s7};
    u64 result = 0;
    bool handled = self->provider == user_provider && fake_user() && user_call(N, args, &result);
    if (!handled && self->provider == gamertag_provider && N == 3 && fake_user()) {
        // GetGamertag: the local user's name, NUL-terminated UTF-8; used = bytes including the NUL.
        static const char name[] = "Player";
        const u64 size = c, buffer = s0, used = s1;
        handled = true;
        if (a != local_user_handle) {
            result = 0x89245104;  // no such user
        } else if (size < sizeof(name) || !buffer || !x2w_local::writable(reinterpret_cast<void*>(buffer), sizeof(name))) {
            result = 0x8007007A;  // buffer too small
        } else {
            std::memcpy(reinterpret_cast<void*>(buffer), name, sizeof(name));
            if (used && x2w_local::writable(reinterpret_cast<void*>(used), 8)) *reinterpret_cast<u64*>(used) = sizeof(name);
            result = 0;
        }
    }
    if (!handled && self->provider == save_provider && N >= 3 && fake_user()) handled = save_call(N, args, &result);
    if (!handled && N >= 3 && fake_user()) handled = area_call(provider_ids[self->provider].Data1, N, args, &result);
    if (!handled && self->provider != user_provider && N >= 3 && fake_user()) {
        // Other API families cannot resolve the local user's handle: fail the call instead of passing it on.
        for (int i = 0; i < 4 && !handled; ++i) {
            if (args[i] == local_user_handle) {
                handled = true;
                result = 0x80004005;
            }
        }
    }
    if (!handled) {
        const Method method = reinterpret_cast<Method>((*reinterpret_cast<void***>(self->real))[N]);
        result = method(self->real, a, b, c, s0, s1, s2, s3, s4, s5, s6, s7);
    }
    const bool about_tracked = N >= 3 && (tracked(a) || tracked(b) || tracked(c));
    const u64 raw_result = result;
    if (about_tracked && !handled && fake_user() && (result & 0xffffffffu) == borrowed_failure) result = 0;
    if (log_it || (about_tracked && InterlockedIncrement(&tracked_logged) <= 120))
        x2w_local::logf("x2w-runtime %08lx::slot%d(0x%llx, 0x%llx, 0x%llx | 0x%llx, 0x%llx) -> 0x%llx%s%s from +0x%llx", provider_ids[self->provider].Data1, N,
                        a, b, c, s0, s1, result, handled ? " (local user)" : "",
                        about_tracked ? (raw_result != result ? " (tracked async, failure hidden)" : " (tracked async)") : "",
                        reinterpret_cast<u64>(_ReturnAddress()) - reinterpret_cast<u64>(GetModuleHandleW(nullptr)));
    return result;
}
template <std::size_t... I>
void fill_slots(std::index_sequence<I...>)
{
    ((slot_table[I] = reinterpret_cast<void*>(&forward<static_cast<int>(I)>)), ...);
}

HRESULT __stdcall proxy_query(const GUID* provider, const GUID* iid, void** out)
{
    const HRESULT hr = real_query(provider, iid, out);
    if (FAILED(hr) && out && provider && provider->Data1 == 0xf4faf4d4 && fake_user()) {
        // Persistent local storage: the system runtime has none for an unpackaged process.
        static LONG logged;
        if (x2w_local::first_calls(logged, 2)) x2w_local::logf("x2w-runtime persistent local storage: local object instead of 0x%08lx", static_cast<unsigned long>(hr));
        *out = storage_object();
        return S_OK;
    }
    if (FAILED(hr) || !out || !*out || !provider) return hr;
    GUID key = *provider;
    const bool gamertag = provider->Data1 == user_provider_data1 && iid && iid->Data1 == gamertag_iid_data1;
    if (gamertag) key.Data1 = gamertag_iid_data1;
    provider = &key;
    AcquireSRWLockExclusive(&proxy_lock);
    int index = -1;
    for (int i = 0; i < provider_count; ++i)
        if (std::memcmp(&provider_ids[i], provider, sizeof(GUID)) == 0) index = i;
    if (index < 0 && provider_count < max_providers) {
        index = provider_count++;
        provider_ids[index] = *provider;
        if (provider->Data1 == user_provider_data1) user_provider = index;
        if (gamertag) gamertag_provider = index;
        if (provider->Data1 == save_provider_data1) save_provider = index;
        x2w_local::logf("x2w-runtime provider %08lx-%04x-... -> object %p (proxied as #%d)", provider->Data1, provider->Data2, *out, index);
    }
    Proxy* proxy = nullptr;
    if (index >= 0) {
        if (!proxies[index] || proxies[index]->real != *out) proxies[index] = new Proxy{slot_table, *out, index};
        proxy = proxies[index];
    }
    ReleaseSRWLockExclusive(&proxy_lock);
    if (proxy) *out = proxy;
    return hr;
}

FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR name)
{
    const FARPROC real = GetProcAddress(module, name);
    if (real && reinterpret_cast<u64>(name) > 0xffff) {
        static LONG logged;
        if (x2w_local::first_calls(logged, 40)) x2w_local::logf("x2w-runtime GetProcAddress(%p, \"%s\") -> %p", static_cast<void*>(module), name, static_cast<void*>(real));
        if (std::strcmp(name, "QueryApiImpl") == 0) {
            real_query = reinterpret_cast<QueryApi>(real);
            return reinterpret_cast<FARPROC>(&proxy_query);
        }
    }
    return real;
}

// Replaces the main module's import of GetProcAddress (by name) with the hook.
void patch_imports()
{
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return;
    int patched = 0;
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name; ++import) {
        if (!import->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(by_name->Name, "GetProcAddress") != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) {
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&hooked_get_proc_address);
                VirtualProtect(&slots->u1.Function, 8, old, &old);
                ++patched;
            }
        }
    }
    x2w_local::logf("x2w-runtime proxy: %d GetProcAddress import(s) of the main module patched", patched);
}

// ---- the local user -------------------------------------------------------------------------
// Filled in from observed calls (slot numbers are positions in the user API object's vtable).
// Returning false forwards the call to the real runtime.
bool user_call(int slot, u64* args, u64* result)
{
    const auto out = [](u64 pointer, std::size_t bytes) { return pointer && x2w_local::writable(reinterpret_cast<void*>(pointer), bytes); };
    constexpr u64 ok = 0, fail = 0x80004005, not_found = 0x80070490;
    switch (slot) {
    case 3:  // XUserDuplicateHandle(handle, &duplicate)
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = local_user_handle;
        *result = ok;
        return true;
    case 4:  // XUserCloseHandle(handle)
        if (args[0] != local_user_handle) return false;
        *result = 0;
        return true;
    case 5:  // XUserCompare(a, b)
        if (args[0] != local_user_handle && args[1] != local_user_handle) return false;
        *result = args[0] == args[1] ? 0 : 1;
        return true;
    case 8:  // XUserAddResult(async, &handle): the real operation ends with "no default user"; answer with the local user
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = local_user_handle;
        *result = ok;
        return true;
    case 9:  // XUserGetLocalId(handle, &local id)
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = 1;
        *result = ok;
        return true;
    case 10:  // XUserFindUserByLocalId(local id, &handle)
    case 12:  // XUserFindUserById(id, &handle)
        if (args[0] != (slot == 10 ? 1 : local_user_id)) return false;
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = local_user_handle;
        *result = ok;
        return true;
    case 11:  // XUserGetId(handle, &id)
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = local_user_id;
        *result = ok;
        return true;
    case 13:  // XUserGetIsGuest(handle, &is guest)
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 1)) *reinterpret_cast<bool*>(args[1]) = false;
        *result = ok;
        return true;
    case 14:  // XUserGetState(handle, &state): 0 = signed in
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 4)) *reinterpret_cast<std::uint32_t*>(args[1]) = 0;
        *result = ok;
        return true;
    case 15: {  // XUserGetGamertag(handle, component, size, buffer, &used)
        if (args[0] != local_user_handle) return false;
        static const char name[] = "Player";
        const u64 size = args[2];
        if (size >= sizeof(name) && out(args[3], sizeof(name))) std::memcpy(reinterpret_cast<void*>(args[3]), name, sizeof(name));
        if (out(args[4], 8)) *reinterpret_cast<u64*>(args[4]) = sizeof(name);
        *result = size >= sizeof(name) ? ok : 0x8007007A;  // ERROR_INSUFFICIENT_BUFFER
        return true;
    }
    case 19:  // XUserGetAgeGroup(handle, &group): 3 = adult
        if (args[0] != local_user_handle) return false;
        if (out(args[1], 4)) *reinterpret_cast<std::uint32_t*>(args[1]) = 3;
        *result = ok;
        return true;
    case 20:  // XUserCheckPrivilege(handle, options, privilege, &has, &deny reason)
        if (args[0] != local_user_handle) return false;
        if (out(args[3], 1)) *reinterpret_cast<bool*>(args[3]) = true;
        if (out(args[4], 4)) *reinterpret_cast<std::uint32_t*>(args[4]) = 0;
        *result = ok;
        return true;
    case 40:  // XUserFindForDevice(device id, &handle): the virtual gamepad belongs to the local user
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = local_user_handle;
        *result = ok;
        return true;
    default:
        break;
    }
    // Anything else that names the local user cannot go to the real runtime (the handle is not its own).
    for (int i = 0; i < 4; ++i) {
        if (args[i] == local_user_handle) {
            *result = slot == 35 ? not_found : fail;
            return true;
        }
    }
    return false;
}

// ---- local game saves -----------------------------------------------------------------------
// Slot numbers follow the order of the public XGameSave.h (hypothesis, checked against the logged
// argument shapes). There are no stored saves: enumerations report nothing, reads find nothing,
// writes are accepted and dropped. Asynchronous calls are completed by starting the real
// "add default user silently" operation on the caller's async block: it fails quickly inside the
// runtime and runs the caller's completion callback on its task queue; the matching Result call is
// then answered here.
constexpr u64 save_provider_handle = 0x58325753'00000001ull, save_container_handle = 0x58325743'00000001ull,
              save_update_handle = 0x58325755'50000001ull;
u64 complete_async(u64 async_block)
{
    Proxy* user = user_provider >= 0 ? proxies[user_provider] : nullptr;
    if (!user || !async_block) return 0x80004005;
    const LONG index = InterlockedIncrement(&tracked_count) - 1;
    if (index < 64) tracked_blocks[index] = async_block;
    const Method add = reinterpret_cast<Method>((*reinterpret_cast<void***>(user->real))[7]);
    return add(user->real, 1 /* add default user silently */, async_block, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
bool save_call(int slot, u64* args, u64* result)
{
    const auto out = [](u64 pointer, std::size_t bytes) { return pointer && x2w_local::writable(reinterpret_cast<void*>(pointer), bytes); };
    constexpr u64 ok = 0, blob_not_found = 0x80830008;
    *result = ok;
    switch (slot) {
    case 3:  // XGameSaveInitializeProvider(user, configuration id, sync on demand, &provider)
        if (out(args[3], 8)) *reinterpret_cast<u64*>(args[3]) = save_provider_handle;
        return true;
    case 4:  // XGameSaveInitializeProviderAsync(user, configuration id, sync on demand, async)
        *result = complete_async(args[3]);
        return true;
    case 5:  // XGameSaveInitializeProviderResult(async, &provider)
        if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = save_provider_handle;
        return true;
    case 6: case 17: case 24:  // close provider / container / update
        return true;
    case 7:  // XGameSaveGetRemainingQuota(provider, &bytes)
        if (out(args[1], 8)) *reinterpret_cast<std::int64_t*>(args[1]) = 256ll << 20;
        return true;
    case 8:  // XGameSaveGetRemainingQuotaAsync(provider, async)
        *result = complete_async(args[1]);
        return true;
    case 9:  // XGameSaveGetRemainingQuotaResult(async, &bytes)
        if (out(args[1], 8)) *reinterpret_cast<std::int64_t*>(args[1]) = 256ll << 20;
        return true;
    case 10: case 12: case 13: case 14: case 15: case 18: case 19:  // delete, container/blob queries: nothing stored
        return true;
    case 11:  // XGameSaveDeleteContainerAsync(provider, name, async)
        *result = complete_async(args[2]);
        return true;
    case 16:  // XGameSaveCreateContainer(provider, name, &container)
        if (out(args[2], 8)) *reinterpret_cast<u64*>(args[2]) = save_container_handle;
        return true;
    case 20:  // XGameSaveReadBlobData(container, blobs, &count, size, data)
        *result = blob_not_found;
        return true;
    case 21:  // XGameSaveReadBlobDataAsync(container, names, count, async)
        *result = complete_async(args[3]);
        return true;
    case 22:  // XGameSaveReadBlobDataResult(async, size, data, &count)
        *result = blob_not_found;
        return true;
    case 23:  // XGameSaveCreateUpdate(container, display name, &update)
        if (out(args[2], 8)) *reinterpret_cast<u64*>(args[2]) = save_update_handle;
        return true;
    case 25: case 26: case 27: case 29:  // blob write / delete, submit, submit result: accepted
        return true;
    case 28:  // XGameSaveSubmitUpdateAsync(update, async)
        *result = complete_async(args[1]);
        return true;
    default:
        *result = 0x80004001;  // E_NOTIMPL
        return true;
    }
}

// ---- other areas, answered as an offline console with one installed package --------------------
// Which provider is which area, and what an offline console answers, follow the public VIctory
// project's runtime notes (github.com/Marvisak/VIctory, used as a reference for facts only).
unsigned env_number(const char* name, unsigned fallback, int base)
{
    char text[32]{};
    return GetEnvironmentVariableA(name, text, sizeof(text)) ? static_cast<unsigned>(std::strtoul(text, nullptr, base)) : fallback;
}
bool area_call(unsigned long provider, int slot, u64* args, u64* result)
{
    const auto out = [](u64 pointer, std::size_t bytes) { return pointer && x2w_local::writable(reinterpret_cast<void*>(pointer), bytes); };
    if (provider == 0x8836fe87 && slot == 3) {
        // IsFeatureAvailable(feature) -> bool. X2W_FEATURES_OFF="20,..." (decimal) reports those features
        // as unavailable (20 = the game event / telemetry API, which an offline run has no use for).
        char list[128];
        const DWORD n = GetEnvironmentVariableA("X2W_FEATURES_OFF", list, sizeof list);
        if (!n || n >= sizeof list) return false;
        for (char* token = std::strtok(list, ","); token; token = std::strtok(nullptr, ","))
            if (std::strtoull(token, nullptr, 10) == args[0]) {
                *result = 0;
                return true;
            }
        return false;
    }
    if (provider == 0x973a344e && slot == 3) {  // title id (a 32-bit value from the title's manifest: X2W_TITLE_ID, hex)
        const unsigned id = env_number("X2W_TITLE_ID", 0, 16);
        if (!id) return false;
        if (out(args[0], 4)) *reinterpret_cast<std::uint32_t*>(args[0]) = id;
        *result = 0;
        return true;
    }
    if (provider == 0xaf406016 && slot == 3) {  // the running package's identifier (size, buffer): the title id in hex
        char text[16];
        const int length = std::snprintf(text, sizeof(text), "%08X", env_number("X2W_TITLE_ID", 0, 16));
        if (args[0] <= static_cast<u64>(length) || !out(args[1], static_cast<std::size_t>(length) + 1)) {
            *result = 0x8007007A;
            return true;
        }
        std::memcpy(reinterpret_cast<void*>(args[1]), text, static_cast<std::size_t>(length) + 1);
        *result = 0;
        return true;
    }
    if (provider == 0xaf406016 && slot >= 5 && slot <= 38) {
        // The rest of the package area, for one fully installed package (the title itself).
        static const unsigned char monitor = 0;
        const u64 monitor_handle = reinterpret_cast<u64>(&monitor);
        constexpr u64 no_package = 0x89245208;
        *result = 0;
        switch (slot) {
        case 5:  // create an installation monitor (id, selector count, selectors, interval, queue, &monitor)
            if (out(args[5], 8)) *reinterpret_cast<u64*>(args[5]) = monitor_handle;
            return true;
        case 6: case 14: case 19:  // close monitor, change install order, uninstall chunks: accepted
            return true;
        case 7:  // progress (monitor, &progress): everything installed
            if (out(args[1], 32)) {
                const u64 bytes = u64{200} << 30;
                u64 words[4] = {bytes, bytes, bytes, 0x0101 /* launchable, completed */};
                std::memcpy(reinterpret_cast<void*>(args[1]), words, sizeof(words));
            }
            return true;
        case 9:  // register for progress changes (monitor, context, callback, &token): never changes
            if (out(args[3], 8)) *reinterpret_cast<u64*>(args[3]) = 1;
            return true;
        case 10:  // unregister
            *result = 1;
            return true;
        case 12:  // chunk availability (id, count, selectors, &availability): 0 = ready
            if (out(args[3], 4)) *reinterpret_cast<std::uint32_t*>(args[3]) = 0;
            return true;
        case 16:  // install chunks (id, count, selectors, interval, suppress confirmation, async)
            *result = complete_async(args[5]);
            return true;
        case 17:  // result of that: a monitor
            if (out(args[1], 8)) *reinterpret_cast<u64*>(args[1]) = monitor_handle;
            return true;
        case 18:  // download estimate (id, count, selectors, &bytes, &needs confirmation): nothing to download
            if (out(args[3], 8)) *reinterpret_cast<u64*>(args[3]) = 0;
            if (out(args[4], 1)) *reinterpret_cast<bool*>(args[4]) = false;
            return true;
        case 34:  // uninstall another package: refused
            return true;
        case 37:  // mount another package (id, async): none is installed
            *result = complete_async(args[1]);
            return true;
        case 38:
            *result = no_package;
            return true;
        default:
            return false;
        }
    }
    if (provider == 0xaf406016 && slot == 4) {  // is the title a package: yes
        *result = 1;
        return true;
    }
    if (provider == 0xaf406016 && slot == 39) {  // enumerate packages: none besides the title
        *result = 0;
        return true;
    }
    if (provider == 0x37e56907 && slot == 15) {  // connectivity hint: stack initialized, no connectivity
        if (out(args[0], 16)) {
            const std::uint32_t words[4] = {1 /* level: none */, 0 /* cost: unknown */, 0, 1 /* initialized; other flags clear */};
            std::memcpy(reinterpret_cast<void*>(args[0]), words, sizeof(words));
        }
        *result = 0;
        return true;
    }
    return false;
}

// Persistent local storage: the root of a drive (X2W_STORAGE_PATH, e.g. "S:\\", mapped by the run
// script onto a folder outside the game) with the quota from the manifest (X2W_STORAGE_MB).
struct StorageObject { void* const* vtable; };
const char* storage_path()
{
    static char path[64];
    if (!path[0] && !GetEnvironmentVariableA("X2W_STORAGE_PATH", path, sizeof(path))) std::strcpy(path, "S:\\");
    return path;
}
HRESULT __stdcall storage_query(StorageObject* self, const GUID*, void** out) { if (out) *out = self; return S_OK; }
u64 __stdcall storage_ref(StorageObject*) { return 1; }
HRESULT __stdcall storage_path_size(StorageObject*, std::size_t* out)
{
    if (out) *out = std::strlen(storage_path()) + 1;
    x2w_local::logf("x2w-runtime storage path size -> %zu", std::strlen(storage_path()) + 1);
    return S_OK;
}
HRESULT __stdcall storage_get_path(StorageObject*, std::size_t size, char* buffer, std::size_t* used)
{
    const std::size_t need = std::strlen(storage_path()) + 1;
    if (used) *used = need;
    if (!buffer || size < need) return static_cast<HRESULT>(0x8007007A);
    std::memcpy(buffer, storage_path(), need);
    x2w_local::logf("x2w-runtime storage path -> %s", storage_path());
    return S_OK;
}
HRESULT __stdcall storage_space(StorageObject*, u64* out)
{
    const u64 total = u64{env_number("X2W_STORAGE_MB", 1024, 10)} << 20;
    if (out) {
        out[0] = total;  // writable now
        out[1] = total;  // left in the quota
        out[2] = 0;      // used
        out[3] = total;  // quota
    }
    return S_OK;
}
HRESULT __stdcall storage_prompt(StorageObject*, u64, u64 block) { return static_cast<HRESULT>(complete_async(block)); }
HRESULT __stdcall storage_prompt_result(StorageObject*, u64) { return S_OK; }
void* storage_object()
{
    static void* const table[8] = {
        reinterpret_cast<void*>(&storage_query), reinterpret_cast<void*>(&storage_ref), reinterpret_cast<void*>(&storage_ref),
        reinterpret_cast<void*>(&storage_path_size), reinterpret_cast<void*>(&storage_get_path), reinterpret_cast<void*>(&storage_space),
        reinterpret_cast<void*>(&storage_prompt), reinterpret_cast<void*>(&storage_prompt_result)};
    static StorageObject object{table};
    return &object;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH && x2w_local::env_on("X2W_RUNTIME_PROXY")) {
        fill_slots(std::make_index_sequence<max_slots>{});
        patch_imports();
        // Heartbeat: every 10 s, the runtime methods called since the last report and how often, so
        // a title that keeps polling one call (waiting for something) shows it after the first-call
        // logs have gone quiet.
        CreateThread(nullptr, 0, [](void*) -> DWORD {
            static LONG seen[max_providers][max_slots];
            for (int second = 10;; second += 10) {
                Sleep(10000);
                char line[1024];
                int n = std::snprintf(line, sizeof(line), "x2w-runtime calls %d s:", second);
                bool any = false;
                for (int p = 0; p < provider_count && p < max_providers; ++p)
                    for (int slot = 3; slot < max_slots; ++slot) {
                        const LONG now = call_counts[p][slot], delta = now - seen[p][slot];
                        seen[p][slot] = now;
                        if (delta > 0 && n < static_cast<int>(sizeof(line)) - 40) {
                            n += std::snprintf(line + n, sizeof(line) - n, " %08lx:%d x%ld", provider_ids[p].Data1, slot, delta);
                            any = true;
                        }
                    }
                if (any) x2w_local::logf("%s", line);
            }
        }, nullptr, 0, nullptr);
    }
    return TRUE;
}
