// Fixture-only desktop xgameruntime.dll. It is built into the test fixture
// directory, never beside the normal shims. The table below distinguishes
// implemented synchronous contracts from logged E_NOTIMPL stubs. This is not
// an ABI-complete XGameRuntime provider.
//
// Public GDK references used for the names and signatures:
//   XUserAddAsync                       https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/functions/xuseraddasync?view=gdk-2604
//   XUserFindUserById                   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/functions/xuserfinduserbyid?view=gdk-2610
//   XUserGetId                          https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/functions/xusergetid?view=gdk-2610
//   XUserDuplicateHandle                https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/functions/xuserduplicatehandle?view=gdk-2604
//   XUserCloseHandle                    https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xuser/functions/xuserclosehandle?view=gdk-2604
//   XGameSaveInitializeProviderAsync    https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesaveinitializeproviderasync?view=gdk-2610
//   XGameSaveInitializeProvider         https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesaveinitializeprovider?view=gdk-2604
//   XGameSaveCreateContainer            https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavecreatecontainer?view=gdk-2604
//   XGameSaveCreateUpdate               https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavecreateupdate?view=gdk-2610
//   XGameSaveSubmitBlobWrite            https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavesubmitblobwrite?view=gdk-2604
//   XGameSaveSubmitUpdate               https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavesubmitupdate?view=gdk-2610
//   XGameSaveReadBlobData               https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavereadblobdata?view=gdk-2604
//   XGameSaveCloseProvider              https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavecloseprovider?view=gdk-2610
//   XGameSaveCloseContainer             https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesaveclosecontainer?view=gdk-2610
//   XGameSaveCloseUpdate                https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgamesave/functions/xgamesavecloseupdate?view=gdk-2610
//   XPersistentLocalStorageGetPathSize  https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpersistentlocalstorage/functions/xpersistentlocalstoragegetpathsize?view=gdk-2604
//   XPersistentLocalStorageGetPath      https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpersistentlocalstorage/functions/xpersistentlocalstoragegetpath?view=gdk-2604
//   XPersistentLocalStorageGetSpaceInfo https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpersistentlocalstorage/functions/xpersistentlocalstoragegetspaceinfo?view=gdk-2510
//   XPersistentLocalStorageSpaceInfo    https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpersistentlocalstorage/structs/xpersistentlocalstoragespaceinfo?view=gdk-2604
//   XPackageMount                       https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpackage/functions/xpackagemount?view=gdk-2604
//   XPackageGetMountPathSize            https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpackage/xpackage_members?view=gdk-2604
//   XPackageGetMountPath                https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpackage/functions/xpackagegetmountpath?view=gdk-2604
//   XPackageCloseMountHandle            https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpackage/functions/xpackageclosemounthandle?view=gdk-2510
//   XPackageMountWithUiAsync            https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xpackage/functions/xpackagemountwithuiasync?view=gdk-2610
//   XSystemGetConsoleId                 https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xsystem/functions/xsystemgetconsoleid?view=gdk-2604
//   XSystemGetDeviceType                https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xsystem/functions/xsystemgetdevicetype?view=gdk-2604
//   XSystemDeviceType                   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xsystem/enums/xsystemdevicetype?view=gdk-2604
//   XGameGetXboxTitleId                 https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgame/functions/xgamegetxboxtitleid?view=gdk-2604
//   XAsyncGetStatus                     https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xasync/functions/xasyncgetstatus?view=gdk-2604
//   XTaskQueueCreate                    https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xtaskqueue/functions/xtaskqueuecreate?view=gdk-2604
//
// Async gap: XUserAddAsync, XGameSaveInitializeProviderAsync and
// XPackageMountWithUiAsync remain stubs. Their public completion contract
// depends on XAsyncBlock plus user/save/package result queue, callback and
// result semantics. This repository has no GDK headers, so the shim does not
// invent that async completion state. XPackageMount is documented as a
// deprecated synchronous alternative; the shim implements it against a local
// synthetic package directory. The implemented surface is the synchronous
// XUser, XGameSave, PLS, XPackage, XSystem and XGame subset below. The public
// XSystem and XGame member lists do not document XSystemGetConsoleType or
// XGameGetXboxTitleIdEx, so those names are not invented here.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct XAsyncBlock;
namespace xgameruntime_shim {
struct XUserHandleStorage;
struct XGameSaveProviderStorage;
struct XGameSaveContainerStorage;
struct XGameSaveUpdateStorage;
struct XPackageMountStorage;
}
using XUserHandle = xgameruntime_shim::XUserHandleStorage*;
struct XTaskQueueHandleStorage;
using XTaskQueueHandle = XTaskQueueHandleStorage*;
using XGameSaveProviderHandle = xgameruntime_shim::XGameSaveProviderStorage*;
using XGameSaveContainerHandle = xgameruntime_shim::XGameSaveContainerStorage*;
using XGameSaveUpdateHandle = xgameruntime_shim::XGameSaveUpdateStorage*;
using XPackageMountHandle = xgameruntime_shim::XPackageMountStorage*;
enum XUserAddOptions : std::uint32_t;
enum XTaskQueueDispatchMode : std::uint32_t;

// Public XSystem.h enumeration. The shim reports Pc because it is a desktop
// process; the remaining values are named so the ABI and switch sites use the
// documented constants without guessing numeric values.
enum class XSystemDeviceType : std::uint32_t {
    Unknown = 0x00,
    Pc = 0x01,
    XboxOne = 0x02,
    XboxOneS = 0x03,
    XboxOneX = 0x04,
    XboxOneXDevkit = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit = 0x08
};

static_assert(sizeof(XSystemDeviceType) == 4);
static_assert(static_cast<std::uint32_t>(XSystemDeviceType::Unknown) == 0x00);
static_assert(static_cast<std::uint32_t>(XSystemDeviceType::Pc) == 0x01);
static_assert(static_cast<std::uint32_t>(XSystemDeviceType::XboxScarlettDevkit) == 0x08);

// Public XPersistentLocalStorage.h structure used by the synchronous space
// contract. The four fields follow the documented order and width.
struct XPersistentLocalStorageSpaceInfo {
    std::uint64_t availableFreeBytes;
    std::uint64_t totalFreeBytes;
    std::uint64_t usedBytes;
    std::uint64_t totalBytes;
};

static_assert(sizeof(XPersistentLocalStorageSpaceInfo) == 32);
static_assert(offsetof(XPersistentLocalStorageSpaceInfo, totalBytes) == 24);

// Public XGameSave.h structures used by the synchronous read contract.
struct XGameSaveBlobInfo {
    const char* name;
    std::uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    std::uint8_t* data;
};

// Shim-only observability interface. It is not a GDK API.
struct XGameRuntimeShimLogEntry {
    std::uint32_t sequence;
    HRESULT result;
    const char* component;
    const char* function;
};

static_assert(sizeof(XGameRuntimeShimLogEntry) == 24);
static_assert(offsetof(XGameRuntimeShimLogEntry, sequence) == 0);
static_assert(offsetof(XGameRuntimeShimLogEntry, result) == 4);
static_assert(offsetof(XGameRuntimeShimLogEntry, component) == 8);
static_assert(offsetof(XGameRuntimeShimLogEntry, function) == 16);

// Each row has one export name and one component. The state is stub,
// implemented_hr, implemented_void, or implemented_value; implemented rows name
// an internal function and its arguments, while stub rows deliberately have no
// implementation arguments.
// The same row list defines the exports and the inspectable metadata table.
#define XGAMERUNTIME_THUNK_TABLE(X) \
    X(XUserAddAsync, XUser, HRESULT, (XUserAddOptions, XAsyncBlock*), stub, (), stub) \
    X(XUserFindUserById, XUser, HRESULT, (std::uint64_t userId, XUserHandle* handle), implemented_hr, (userId, handle), user_find_by_id) \
    X(XUserGetId, XUser, HRESULT, (XUserHandle user, std::uint64_t* userId), implemented_hr, (user, userId), user_get_id) \
    X(XUserDuplicateHandle, XUser, HRESULT, (XUserHandle handle, XUserHandle* duplicatedHandle), implemented_hr, (handle, duplicatedHandle), user_duplicate_handle) \
    X(XUserCloseHandle, XUser, void, (XUserHandle user), implemented_void, (user), user_close_handle) \
    X(XGameSaveInitializeProviderAsync, XGameSave, HRESULT, (XUserHandle, const char*, bool, XAsyncBlock*), stub, (), stub) \
    X(XGameSaveInitializeProvider, XGameSave, HRESULT, (XUserHandle requestingUser, const char* configurationId, bool syncOnDemand, XGameSaveProviderHandle* provider), implemented_hr, (requestingUser, configurationId, syncOnDemand, provider), game_save_initialize_provider) \
    X(XGameSaveCreateContainer, XGameSave, HRESULT, (XGameSaveProviderHandle provider, const char* containerName, XGameSaveContainerHandle* containerContext), implemented_hr, (provider, containerName, containerContext), game_save_create_container) \
    X(XGameSaveCreateUpdate, XGameSave, HRESULT, (XGameSaveContainerHandle container, const char* containerDisplayName, XGameSaveUpdateHandle* updateContext), implemented_hr, (container, containerDisplayName, updateContext), game_save_create_update) \
    X(XGameSaveSubmitBlobWrite, XGameSave, HRESULT, (XGameSaveUpdateHandle updateContext, const char* blobName, const std::uint8_t* data, std::size_t byteCount), implemented_hr, (updateContext, blobName, data, byteCount), game_save_submit_blob_write) \
    X(XGameSaveSubmitUpdate, XGameSave, HRESULT, (XGameSaveUpdateHandle updateContext), implemented_hr, (updateContext), game_save_submit_update) \
    X(XGameSaveReadBlobData, XGameSave, HRESULT, (XGameSaveContainerHandle container, const char** blobNames, std::uint32_t* countOfBlobs, std::size_t blobsSize, XGameSaveBlob* blobData), implemented_hr, (container, blobNames, countOfBlobs, blobsSize, blobData), game_save_read_blob_data) \
    X(XGameSaveCloseProvider, XGameSave, void, (XGameSaveProviderHandle provider), implemented_void, (provider), game_save_close_provider) \
    X(XGameSaveCloseContainer, XGameSave, void, (XGameSaveContainerHandle context), implemented_void, (context), game_save_close_container) \
    X(XGameSaveCloseUpdate, XGameSave, void, (XGameSaveUpdateHandle context), implemented_void, (context), game_save_close_update) \
    X(XPersistentLocalStorageGetPathSize, XPersistentLocalStorage, HRESULT, (std::size_t* pathSize), implemented_hr, (pathSize), pls_get_path_size) \
    X(XPersistentLocalStorageGetPath, XPersistentLocalStorage, HRESULT, (std::size_t pathSize, char* path, std::size_t* pathUsed), implemented_hr, (pathSize, path, pathUsed), pls_get_path) \
    X(XPersistentLocalStorageGetSpaceInfo, XPersistentLocalStorage, HRESULT, (XPersistentLocalStorageSpaceInfo* info), implemented_hr, (info), pls_get_space_info) \
    X(XPackageMount, XPackage, HRESULT, (const char* packageIdentifier, XPackageMountHandle* mountHandle), implemented_hr, (packageIdentifier, mountHandle), xpackage_mount) \
    X(XPackageGetMountPathSize, XPackage, HRESULT, (XPackageMountHandle mountHandle, std::size_t* pathSize), implemented_hr, (mountHandle, pathSize), xpackage_get_mount_path_size) \
    X(XPackageGetMountPath, XPackage, HRESULT, (XPackageMountHandle mountHandle, std::size_t pathSize, char* path), implemented_hr, (mountHandle, pathSize, path), xpackage_get_mount_path) \
    X(XPackageCloseMountHandle, XPackage, void, (XPackageMountHandle mountHandle), implemented_void, (mountHandle), xpackage_close_mount_handle) \
    X(XPackageMountWithUiAsync, XPackage, HRESULT, (const char*, XAsyncBlock*), stub, (), stub) \
    X(XSystemGetConsoleId, XSystem, HRESULT, (std::size_t consoleIdSize, char* consoleId, std::size_t* consoleIdUsed), implemented_hr, (consoleIdSize, consoleId, consoleIdUsed), xsystem_get_console_id) \
    X(XSystemGetDeviceType, XSystem, XSystemDeviceType, (), implemented_value, (), xsystem_get_device_type) \
    X(XGameGetXboxTitleId, XGame, HRESULT, (std::uint32_t* titleId), implemented_hr, (titleId), xgame_get_xbox_title_id) \
    X(XAsyncGetStatus, XAsync, HRESULT, (XAsyncBlock*, bool), stub, (), stub) \
    X(XTaskQueueCreate, XTaskQueue, HRESULT, (XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle*), stub, (), stub)

namespace xgameruntime_shim {
constexpr HRESULT not_implemented = static_cast<HRESULT>(0x80004001L);
static_assert(not_implemented == E_NOTIMPL);

constexpr std::uint64_t synthetic_user_id = 0x0009000000000001ull;

// XSystemConsoleIdBytes is the public XSystem.h buffer constant named by the
// XSystemGetConsoleId docs. Those docs print the 38-character default value but
// not the constant; the documented value plus its terminator is 39 bytes, and
// the public Microsoft XBOX-GDK-dotnet interop constants also publish 39. The
// synthetic value keeps that shape while naming this fixture.
constexpr std::size_t xsystem_console_id_bytes = 39;
constexpr char synthetic_console_id[] = "58325731.00000000.00000000.00000000.01";
static_assert(sizeof(synthetic_console_id) == xsystem_console_id_bytes);

// The title ID is the 32-bit value XGameGetXboxTitleId returns. 0x58325731 is
// the ASCII spelling X2W1 and is fixed for this fixture.
constexpr std::uint32_t synthetic_xbox_title_id = 0x58325731u;
static_assert(synthetic_xbox_title_id != 0);

constexpr std::size_t user_capacity = 8;
constexpr std::size_t provider_capacity = 4;
constexpr std::size_t container_store_capacity = 8;
constexpr std::size_t container_handle_capacity = 8;
constexpr std::size_t update_capacity = 4;
constexpr std::size_t blob_capacity = 4;
constexpr std::size_t container_name_capacity = 257;
constexpr std::size_t blob_name_capacity = 64;
constexpr std::size_t blob_bytes_capacity = 1024;
constexpr std::size_t package_identifier_capacity = 64;
constexpr std::size_t package_mount_capacity = 4;
constexpr wchar_t pls_directory_name[] = L"xbox-recomp-pls";
constexpr wchar_t synthetic_pls_app_name[] = L"x2w-synthetic-title-0001";
constexpr wchar_t package_directory_name[] = L"xbox-recomp-packages";
constexpr std::uint64_t synthetic_pls_available_free_bytes = 32ull * 1024 * 1024;
constexpr std::uint64_t synthetic_pls_total_free_bytes = 48ull * 1024 * 1024;
constexpr std::uint64_t synthetic_pls_total_bytes = 64ull * 1024 * 1024;
constexpr HRESULT e_gs_invalid_container_name = static_cast<HRESULT>(0x80830001L);
// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/errorcodes?view=gdk-2610
constexpr HRESULT e_gs_handle_expired = static_cast<HRESULT>(0x8083000DL);

// Synthetic identity and storage policy. There is exactly one XUser identity.
// XGameSave state lives only in this process. PLS reports a fixed path under
// the process temporary directory, and XPackage mounts map to a fixed
// subdirectory there. The shim creates empty directories; it does not write
// title data or secrets.

enum class FunctionState { stub, implemented_hr, implemented_void, implemented_value };

struct ThunkEntry {
    const char* name;
    const char* component;
    const char* function;
    FunctionState state;
};

constexpr bool same_text(const char* left, const char* right) noexcept
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

#define XGAMERUNTIME_COUNT_THUNK(name, component, return_type, parameters, state, args, implementation) +1
inline constexpr std::size_t thunk_count = 0 XGAMERUNTIME_THUNK_TABLE(XGAMERUNTIME_COUNT_THUNK);
#undef XGAMERUNTIME_COUNT_THUNK

#define XGAMERUNTIME_TABLE_ROW(name, component, return_type, parameters, state, args, implementation) \
    ThunkEntry{#name, #component, #name, FunctionState::state},
inline constexpr std::array<ThunkEntry, thunk_count> thunks = {
    XGAMERUNTIME_THUNK_TABLE(XGAMERUNTIME_TABLE_ROW)
};
#undef XGAMERUNTIME_TABLE_ROW

constexpr bool valid_thunk_table(const std::array<ThunkEntry, thunk_count>& table) noexcept
{
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (!table[i].name || !table[i].name[0] || !table[i].component ||
            !table[i].component[0] || !table[i].function || !table[i].function[0]) {
            return false;
        }
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            if (same_text(table[i].name, table[j].name)) return false;
        }
    }
    return true;
}

constexpr bool has_component(const std::array<ThunkEntry, thunk_count>& table,
                             const char* component) noexcept
{
    for (const auto& entry : table) {
        if (same_text(entry.component, component)) return true;
    }
    return false;
}

constexpr bool has_implemented(const std::array<ThunkEntry, thunk_count>& table,
                               const char* component) noexcept
{
    for (const auto& entry : table) {
        if (same_text(entry.component, component) && entry.state != FunctionState::stub) return true;
    }
    return false;
}

constexpr bool has_stub(const std::array<ThunkEntry, thunk_count>& table, const char* name) noexcept
{
    for (const auto& entry : table) {
        if (same_text(entry.name, name)) return entry.state == FunctionState::stub;
    }
    return false;
}

// A malformed table fails at compile time. The async functions stay explicit
// stubs: no result structure or completion queue is fabricated.
static_assert(valid_thunk_table(thunks), "XGameRuntime thunk table has an empty or duplicate name");
static_assert(has_component(thunks, "XUser"));
static_assert(has_component(thunks, "XGameSave"));
static_assert(has_component(thunks, "XPackage"));
static_assert(has_component(thunks, "XSystem"));
static_assert(has_component(thunks, "XGame"));
static_assert(has_component(thunks, "XAsync"));
static_assert(has_component(thunks, "XTaskQueue"));
static_assert(has_implemented(thunks, "XUser"));
static_assert(has_implemented(thunks, "XGameSave"));
static_assert(has_implemented(thunks, "XPersistentLocalStorage"));
static_assert(has_implemented(thunks, "XPackage"));
static_assert(has_implemented(thunks, "XSystem"));
static_assert(has_implemented(thunks, "XGame"));
static_assert(has_stub(thunks, "XUserAddAsync"));
static_assert(has_stub(thunks, "XGameSaveInitializeProviderAsync"));
static_assert(has_stub(thunks, "XPackageMountWithUiAsync"));

struct XUserHandleStorage {
    std::uint64_t id = 0;
    std::uint32_t references = 0;
    bool live = false;
};

struct BlobSlot {
    bool live = false;
    char name[blob_name_capacity]{};
    std::uint32_t size = 0;
    std::array<std::uint8_t, blob_bytes_capacity> data{};
};

struct XGameSaveProviderStorage {
    bool live = false;
    std::uint64_t scope = 0;
};

struct ContainerStore {
    bool live = false;
    std::uint64_t id = 0;
    std::uint64_t provider_scope = 0;
    char name[container_name_capacity]{};
    std::array<BlobSlot, blob_capacity> blobs{};
};

struct XGameSaveContainerStorage {
    bool live = false;
    std::uint64_t id = 0;
    XGameSaveProviderStorage* provider = nullptr;
    std::uint64_t provider_scope = 0;
    std::uint64_t store_id = 0;
    char name[container_name_capacity]{};
};

struct XGameSaveUpdateStorage {
    bool live = false;
    std::uint64_t container_handle_id = 0;
    std::uint64_t provider_scope = 0;
    std::uint64_t store_id = 0;
    char display_name[container_name_capacity]{};
    std::array<BlobSlot, blob_capacity> staged{};
};

struct XPackageMountStorage {
    bool live = false;
    std::uint64_t id = 0;
    char identifier[package_identifier_capacity]{};
};

struct RuntimeState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::uint64_t next_provider_scope = 1;
    std::uint64_t next_store_id = 1;
    std::uint64_t next_container_handle_id = 1;
    std::uint64_t next_package_mount_id = 1;
    std::array<XUserHandleStorage, user_capacity> users{};
    std::array<XGameSaveProviderStorage, provider_capacity> providers{};
    std::array<ContainerStore, container_store_capacity> container_stores{};
    std::array<XGameSaveContainerStorage, container_handle_capacity> containers{};
    std::array<XGameSaveUpdateStorage, update_capacity> updates{};
    std::array<XPackageMountStorage, package_mount_capacity> package_mounts{};
};

RuntimeState runtime_state;

template <typename T, std::size_t N>
T* find_live(std::array<T, N>& slots, const void* pointer) noexcept
{
    for (auto& slot : slots) {
        if (slot.live && &slot == pointer) return &slot;
    }
    return nullptr;
}

bool copy_text(char* destination, std::size_t capacity, const char* source) noexcept
{
    if (!destination || !source) return false;
    const std::size_t length = std::strlen(source);
    if (length == 0 || length >= capacity) return false;
    std::memcpy(destination, source, length + 1);
    return true;
}

bool to_utf8(const std::wstring& source, std::string& output) noexcept
{
    if (source.empty()) return false;
    const int length = static_cast<int>(source.size());
    const int needed = WideCharToMultiByte(CP_UTF8, 0, source.data(), length, nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return false;
    output.resize(static_cast<std::size_t>(needed));
    return WideCharToMultiByte(CP_UTF8, 0, source.data(), length, output.data(), needed,
                               nullptr, nullptr) == needed;
}

bool temp_directory(std::wstring& directory) noexcept
{
    wchar_t buffer[MAX_PATH + 1]{};
    constexpr std::size_t buffer_capacity = sizeof(buffer) / sizeof(buffer[0]);
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer_capacity), buffer);
    if (length == 0 || length >= buffer_capacity) return false;
    directory.assign(buffer, length);
    if (directory.empty() || directory.back() != L'\\') directory.push_back(L'\\');
    return true;
}

bool ensure_directory(const std::wstring& path) noexcept
{
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool pls_path_utf8(std::string& output) noexcept
{
    std::wstring root;
    if (!temp_directory(root)) return false;
    const std::wstring directory = root + pls_directory_name;
    const std::wstring app_directory = directory + L"\\" + synthetic_pls_app_name;
    if (!ensure_directory(directory) || !ensure_directory(app_directory)) return false;
    return to_utf8(app_directory, output);
}

bool valid_package_identifier(const char* identifier) noexcept
{
    if (!identifier) return false;
    const std::size_t length = std::strlen(identifier);
    if (length == 0 || length >= package_identifier_capacity) return false;
    for (const char* cursor = identifier; *cursor; ++cursor) {
        const char c = *cursor;
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || digit || c == '.' || c == '_' || c == '-')) return false;
    }
    if (std::strcmp(identifier, ".") == 0 || std::strcmp(identifier, "..") == 0) return false;
    if (std::strstr(identifier, "..") != nullptr) return false;
    return true;
}

bool package_path_utf8(const char* identifier, std::string& output) noexcept
{
    if (!valid_package_identifier(identifier)) return false;
    std::wstring root;
    if (!temp_directory(root)) return false;
    const std::wstring directory = root + package_directory_name;
    if (!ensure_directory(directory)) return false;
    std::wstring package_directory = directory + L"\\";
    for (const char* cursor = identifier; *cursor; ++cursor) {
        package_directory.push_back(static_cast<wchar_t>(*cursor));
    }
    if (!ensure_directory(package_directory)) return false;
    return to_utf8(package_directory, output);
}

bool valid_container_name(const char* name) noexcept
{
    if (!name) return false;
    const std::size_t length = std::strlen(name);
    if (length == 0 || length >= container_name_capacity) return false;
    const char* slash = std::strrchr(name, '/');
    const char* filename = slash ? slash + 1 : name;
    if (!*filename || filename[std::strlen(filename) - 1] == '.') return false;
    for (const char* cursor = name; *cursor; ++cursor) {
        const bool in_filename = cursor >= filename;
        const char c = *cursor;
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (in_filename) {
            if (!(alpha || digit || c == '_' || c == '.' || c == '-')) return false;
        } else if (!(alpha || digit || c == '_' || c == '/')) {
            return false;
        }
    }
    for (const char* cursor = filename; cursor[0] && cursor[1]; ++cursor) {
        if (cursor[0] == '.' && cursor[1] == '.') return false;
    }
    return true;
}

bool valid_blob_name(const char* name) noexcept
{
    if (!name) return false;
    const std::size_t length = std::strlen(name);
    return length > 0 && length < blob_name_capacity;
}

BlobSlot* find_blob(std::array<BlobSlot, blob_capacity>& blobs, const char* name) noexcept
{
    for (auto& blob : blobs) {
        if (blob.live && std::strcmp(blob.name, name) == 0) return &blob;
    }
    return nullptr;
}

BlobSlot* free_blob(std::array<BlobSlot, blob_capacity>& blobs) noexcept
{
    for (auto& blob : blobs) {
        if (!blob.live) return &blob;
    }
    return nullptr;
}

ContainerStore* find_store_by_name(std::uint64_t provider_scope, const char* name) noexcept
{
    for (auto& store : runtime_state.container_stores) {
        if (store.live && store.provider_scope == provider_scope &&
            std::strcmp(store.name, name) == 0) {
            return &store;
        }
    }
    return nullptr;
}

ContainerStore* find_store_by_id(std::uint64_t store_id) noexcept
{
    for (auto& store : runtime_state.container_stores) {
        if (store.live && store.id == store_id) return &store;
    }
    return nullptr;
}

XGameSaveContainerStorage* find_container_by_id(std::uint64_t handle_id) noexcept
{
    for (auto& handle : runtime_state.containers) {
        if (handle.live && handle.id == handle_id) return &handle;
    }
    return nullptr;
}

bool set_blob(BlobSlot& blob, const char* name, const std::uint8_t* data,
              std::size_t byte_count) noexcept
{
    if (!valid_blob_name(name) || byte_count > blob_bytes_capacity ||
        (byte_count != 0 && !data)) {
        return false;
    }
    blob = BlobSlot{};
    std::memcpy(blob.name, name, std::strlen(name) + 1);
    blob.size = static_cast<std::uint32_t>(byte_count);
    if (byte_count) std::memcpy(blob.data.data(), data, byte_count);
    blob.live = true;
    return true;
}

HRESULT user_find_by_id(std::uint64_t user_id, XUserHandle* handle) noexcept
{
    if (!handle) return E_POINTER;
    *handle = nullptr;
    if (user_id != synthetic_user_id) return E_INVALIDARG;
    AcquireSRWLockExclusive(&runtime_state.lock);
    for (auto& slot : runtime_state.users) {
        if (!slot.live) {
            slot = XUserHandleStorage{synthetic_user_id, 1, true};
            *handle = &slot;
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return E_OUTOFMEMORY;
}

HRESULT user_get_id(XUserHandle user, std::uint64_t* user_id) noexcept
{
    if (!user_id) return E_POINTER;
    *user_id = 0;
    AcquireSRWLockShared(&runtime_state.lock);
    const auto found = find_live(runtime_state.users, user);
    if (found) *user_id = found->id;
    ReleaseSRWLockShared(&runtime_state.lock);
    return found ? S_OK : E_HANDLE;
}

HRESULT user_duplicate_handle(XUserHandle handle, XUserHandle* duplicated_handle) noexcept
{
    if (!duplicated_handle) return E_POINTER;
    *duplicated_handle = nullptr;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto source = find_live(runtime_state.users, handle);
    if (!source) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    for (auto& slot : runtime_state.users) {
        if (!slot.live) {
            slot = XUserHandleStorage{source->id, 1, true};
            *duplicated_handle = &slot;
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return E_OUTOFMEMORY;
}

HRESULT user_close_handle(XUserHandle user) noexcept
{
    if (!user) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.users, user);
    if (!found) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    if (found->references > 1) {
        --found->references;
    } else {
        *found = XUserHandleStorage{};
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return S_OK;
}

HRESULT xsystem_get_console_id(std::size_t console_id_size, char* console_id,
                               std::size_t* console_id_used) noexcept
{
    if (console_id_used) *console_id_used = 0;
    if (!console_id) return E_POINTER;
    if (console_id_size < xsystem_console_id_bytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    std::memcpy(console_id, synthetic_console_id, sizeof(synthetic_console_id));
    if (console_id_used) *console_id_used = sizeof(synthetic_console_id);
    return S_OK;
}

XSystemDeviceType xsystem_get_device_type() noexcept
{
    return XSystemDeviceType::Pc;
}

HRESULT xgame_get_xbox_title_id(std::uint32_t* title_id) noexcept
{
    if (!title_id) return E_POINTER;
    // This fixture always models a configured MicrosoftGame.config title, so
    // the documented missing-title ERROR_NOT_FOUND case is not reachable.
    *title_id = synthetic_xbox_title_id;
    return S_OK;
}

HRESULT pls_get_path_size(std::size_t* path_size) noexcept
{
    if (!path_size) return E_POINTER;
    *path_size = 0;
    std::string path;
    if (!pls_path_utf8(path)) return E_FAIL;
    *path_size = path.size() + 1;
    return S_OK;
}

HRESULT pls_get_path(std::size_t path_size, char* path, std::size_t* path_used) noexcept
{
    if (path_used) *path_used = 0;
    if (!path) return E_POINTER;
    std::string expected;
    if (!pls_path_utf8(expected)) return E_FAIL;
    const std::size_t required = expected.size() + 1;
    if (path_size < required) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(path, expected.c_str(), required);
    if (path_used) *path_used = required;
    return S_OK;
}

HRESULT pls_get_space_info(XPersistentLocalStorageSpaceInfo* info) noexcept
{
    if (!info) return E_POINTER;
    std::string path;
    if (!pls_path_utf8(path)) return E_FAIL;
    *info = XPersistentLocalStorageSpaceInfo{synthetic_pls_available_free_bytes,
                                              synthetic_pls_total_free_bytes,
                                              synthetic_pls_total_bytes - synthetic_pls_total_free_bytes,
                                              synthetic_pls_total_bytes};
    return S_OK;
}

HRESULT xpackage_mount(const char* package_identifier, XPackageMountHandle* mount_handle) noexcept
{
    if (!mount_handle) return E_POINTER;
    *mount_handle = nullptr;
    if (!valid_package_identifier(package_identifier)) return E_INVALIDARG;
    std::string path;
    if (!package_path_utf8(package_identifier, path)) return E_FAIL;
    AcquireSRWLockExclusive(&runtime_state.lock);
    for (auto& slot : runtime_state.package_mounts) {
        if (!slot.live) {
            slot = XPackageMountStorage{};
            slot.live = true;
            slot.id = runtime_state.next_package_mount_id++;
            if (!copy_text(slot.identifier, sizeof(slot.identifier), package_identifier)) {
                slot = XPackageMountStorage{};
                ReleaseSRWLockExclusive(&runtime_state.lock);
                return E_INVALIDARG;
            }
            *mount_handle = &slot;
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return E_OUTOFMEMORY;
}

HRESULT xpackage_get_mount_path_size(XPackageMountHandle mount_handle,
                                     std::size_t* path_size) noexcept
{
    if (!path_size) return E_POINTER;
    *path_size = 0;
    char identifier[package_identifier_capacity]{};
    AcquireSRWLockShared(&runtime_state.lock);
    const auto found = find_live(runtime_state.package_mounts, mount_handle);
    const bool copied = found && copy_text(identifier, sizeof(identifier), found->identifier);
    ReleaseSRWLockShared(&runtime_state.lock);
    if (!found) return E_HANDLE;
    if (!copied) return E_FAIL;
    std::string path;
    if (!package_path_utf8(identifier, path)) return E_FAIL;
    *path_size = path.size() + 1;
    return S_OK;
}

HRESULT xpackage_get_mount_path(XPackageMountHandle mount_handle, std::size_t path_size,
                                char* path) noexcept
{
    if (!path) return E_POINTER;
    char identifier[package_identifier_capacity]{};
    AcquireSRWLockShared(&runtime_state.lock);
    const auto found = find_live(runtime_state.package_mounts, mount_handle);
    const bool copied = found && copy_text(identifier, sizeof(identifier), found->identifier);
    ReleaseSRWLockShared(&runtime_state.lock);
    if (!found) return E_HANDLE;
    if (!copied) return E_FAIL;
    std::string expected;
    if (!package_path_utf8(identifier, expected)) return E_FAIL;
    const std::size_t required = expected.size() + 1;
    if (path_size < required) return E_INVALIDARG;
    std::memcpy(path, expected.c_str(), required);
    return S_OK;
}

HRESULT xpackage_close_mount_handle(XPackageMountHandle mount_handle) noexcept
{
    if (!mount_handle) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.package_mounts, mount_handle);
    if (found) *found = XPackageMountStorage{};
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return found ? S_OK : E_HANDLE;
}

HRESULT game_save_initialize_provider(XUserHandle requesting_user, const char* configuration_id,
                                      bool sync_on_demand, XGameSaveProviderHandle* provider) noexcept
{
    (void)sync_on_demand;
    if (!provider) return E_POINTER;
    *provider = nullptr;
    if (!configuration_id || !configuration_id[0]) return E_INVALIDARG;
    AcquireSRWLockExclusive(&runtime_state.lock);
    if (!find_live(runtime_state.users, requesting_user)) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    for (auto& slot : runtime_state.providers) {
        if (!slot.live) {
            slot = XGameSaveProviderStorage{};
            slot.live = true;
            slot.scope = runtime_state.next_provider_scope++;
            *provider = &slot;
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return E_OUTOFMEMORY;
}

HRESULT game_save_create_container(XGameSaveProviderHandle provider, const char* container_name,
                                   XGameSaveContainerHandle* container) noexcept
{
    if (!container) return E_POINTER;
    *container = nullptr;
    if (!valid_container_name(container_name)) return e_gs_invalid_container_name;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found_provider = find_live(runtime_state.providers, provider);
    if (!found_provider) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    XGameSaveContainerStorage* free_handle = nullptr;
    for (auto& handle : runtime_state.containers) {
        if (!handle.live) {
            free_handle = &handle;
            break;
        }
    }
    if (!free_handle) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_OUTOFMEMORY;
    }
    auto* store = find_store_by_name(found_provider->scope, container_name);
    if (!store) {
        for (auto& candidate : runtime_state.container_stores) {
            if (!candidate.live) {
                store = &candidate;
                break;
            }
        }
        if (!store) {
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return E_OUTOFMEMORY;
        }
        *store = ContainerStore{};
        store->live = true;
        store->id = runtime_state.next_store_id++;
        store->provider_scope = found_provider->scope;
        if (!copy_text(store->name, sizeof(store->name), container_name)) {
            *store = ContainerStore{};
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return E_INVALIDARG;
        }
    }
    *free_handle = XGameSaveContainerStorage{};
    free_handle->live = true;
    free_handle->id = runtime_state.next_container_handle_id++;
    free_handle->provider = found_provider;
    free_handle->provider_scope = found_provider->scope;
    free_handle->store_id = store->id;
    if (!copy_text(free_handle->name, sizeof(free_handle->name), container_name)) {
        *free_handle = XGameSaveContainerStorage{};
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_INVALIDARG;
    }
    *container = free_handle;
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return S_OK;
}

HRESULT game_save_create_update(XGameSaveContainerHandle container, const char* display_name,
                                XGameSaveUpdateHandle* update) noexcept
{
    if (!update) return E_POINTER;
    *update = nullptr;
    if (!display_name || std::strlen(display_name) >= container_name_capacity) return E_INVALIDARG;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found_container = find_live(runtime_state.containers, container);
    if (!found_container) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    for (auto& slot : runtime_state.updates) {
        if (!slot.live) {
            slot = XGameSaveUpdateStorage{};
            slot.live = true;
            slot.container_handle_id = found_container->id;
            slot.provider_scope = found_container->provider_scope;
            slot.store_id = found_container->store_id;
            if (!copy_text(slot.display_name, sizeof(slot.display_name), display_name)) {
                slot = XGameSaveUpdateStorage{};
                ReleaseSRWLockExclusive(&runtime_state.lock);
                return E_INVALIDARG;
            }
            *update = &slot;
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return E_OUTOFMEMORY;
}

HRESULT game_save_submit_blob_write(XGameSaveUpdateHandle update, const char* blob_name,
                                    const std::uint8_t* data, std::size_t byte_count) noexcept
{
    if (!update) return E_HANDLE;
    if (!valid_blob_name(blob_name) || (byte_count != 0 && !data) ||
        byte_count > blob_bytes_capacity) {
        return E_INVALIDARG;
    }
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.updates, update);
    if (!found) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    auto destination = find_blob(found->staged, blob_name);
    if (!destination) destination = free_blob(found->staged);
    const bool copied = destination && set_blob(*destination, blob_name, data, byte_count);
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return copied ? S_OK : E_OUTOFMEMORY;
}

HRESULT game_save_submit_update(XGameSaveUpdateHandle update) noexcept
{
    if (!update) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.updates, update);
    if (!found) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return E_HANDLE;
    }
    const auto container_handle = find_container_by_id(found->container_handle_id);
    if (!container_handle || container_handle->provider_scope != found->provider_scope ||
        container_handle->store_id != found->store_id) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return e_gs_handle_expired;
    }
    const auto store = find_store_by_id(found->store_id);
    if (!store || store->provider_scope != found->provider_scope) {
        ReleaseSRWLockExclusive(&runtime_state.lock);
        return e_gs_handle_expired;
    }
    auto next = store->blobs;
    for (const auto& staged : found->staged) {
        if (!staged.live) continue;
        auto destination = find_blob(next, staged.name);
        if (!destination) destination = free_blob(next);
        if (!destination) {
            ReleaseSRWLockExclusive(&runtime_state.lock);
            return E_OUTOFMEMORY;
        }
        *destination = staged;
    }
    store->blobs = next;
    found->staged = {};
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return S_OK;
}

// This subset reads an explicit non-empty name list into a caller-owned buffer.
// It does not implement the null-list enumeration variant from the full GDK API.
// A null entry in the list is an argument error, as a null list is.
HRESULT game_save_read_blob_data(XGameSaveContainerHandle container, const char** blob_names,
                                 std::uint32_t* count_of_blobs, std::size_t blobs_size,
                                 XGameSaveBlob* blob_data) noexcept
{
    if (!count_of_blobs) return E_POINTER;
    const std::uint32_t requested = *count_of_blobs;
    *count_of_blobs = 0;
    if (!container || !blob_names || !blob_data || requested == 0) return E_INVALIDARG;
    for (std::uint32_t i = 0; i < requested; ++i) {
        if (!blob_names[i]) return E_INVALIDARG;
    }
    AcquireSRWLockShared(&runtime_state.lock);
    const auto found_container = find_live(runtime_state.containers, container);
    if (!found_container) {
        ReleaseSRWLockShared(&runtime_state.lock);
        return E_HANDLE;
    }
    const auto provider = find_live(runtime_state.providers, found_container->provider);
    const auto store = find_store_by_id(found_container->store_id);
    if (!provider || provider->scope != found_container->provider_scope || !store ||
        store->provider_scope != found_container->provider_scope) {
        ReleaseSRWLockShared(&runtime_state.lock);
        return e_gs_handle_expired;
    }
    std::size_t total = requested * sizeof(XGameSaveBlob);
    for (std::uint32_t i = 0; i < requested; ++i) {
        const auto blob = find_blob(store->blobs, blob_names[i]);
        if (!blob) {
            ReleaseSRWLockShared(&runtime_state.lock);
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        total += blob->size;
    }
    if (total > blobs_size) {
        ReleaseSRWLockShared(&runtime_state.lock);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    auto cursor = reinterpret_cast<std::uint8_t*>(blob_data + requested);
    for (std::uint32_t i = 0; i < requested; ++i) {
        const auto blob = find_blob(store->blobs, blob_names[i]);
        blob_data[i].info.name = blob->name;
        blob_data[i].info.size = blob->size;
        blob_data[i].data = cursor;
        if (blob->size) std::memcpy(cursor, blob->data.data(), blob->size);
        cursor += blob->size;
    }
    *count_of_blobs = requested;
    ReleaseSRWLockShared(&runtime_state.lock);
    return S_OK;
}

HRESULT game_save_close_provider(XGameSaveProviderHandle provider) noexcept
{
    if (!provider) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.providers, provider);
    if (found) {
        const auto scope = found->scope;
        *found = XGameSaveProviderStorage{};
        for (auto& store : runtime_state.container_stores) {
            if (store.live && store.provider_scope == scope) store = ContainerStore{};
        }
        for (auto& handle : runtime_state.containers) {
            if (handle.live && handle.provider_scope == scope) handle = XGameSaveContainerStorage{};
        }
    }
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return found ? S_OK : E_HANDLE;
}

HRESULT game_save_close_container(XGameSaveContainerHandle container) noexcept
{
    if (!container) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.containers, container);
    if (found) *found = XGameSaveContainerStorage{};
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return found ? S_OK : E_HANDLE;
}

HRESULT game_save_close_update(XGameSaveUpdateHandle update) noexcept
{
    if (!update) return E_HANDLE;
    AcquireSRWLockExclusive(&runtime_state.lock);
    const auto found = find_live(runtime_state.updates, update);
    if (found) *found = XGameSaveUpdateStorage{};
    ReleaseSRWLockExclusive(&runtime_state.lock);
    return found ? S_OK : E_HANDLE;
}

}  // namespace xgameruntime_shim

// The handle implementations above are intentionally small and explicit. The
// table wrappers below add the bounded log without duplicating public names.
namespace xgameruntime_shim {
constexpr std::size_t log_capacity = 64;

struct LogState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<XGameRuntimeShimLogEntry, log_capacity> entries{};
    std::size_t first = 0;
    std::size_t count = 0;
    std::uint32_t next_sequence = 1;
};

LogState log_state;

HRESULT record_result(const char* component, const char* function, HRESULT result) noexcept
{
    AcquireSRWLockExclusive(&log_state.lock);
    if (log_state.count == log_state.entries.size()) {
        log_state.first = (log_state.first + 1) % log_state.entries.size();
        --log_state.count;
    }
    const auto index = (log_state.first + log_state.count) % log_state.entries.size();
    log_state.entries[index] = XGameRuntimeShimLogEntry{
        log_state.next_sequence++, result, component, function};
    ++log_state.count;
    ReleaseSRWLockExclusive(&log_state.lock);
    return result;
}

HRESULT record_stub(const char* component, const char* function) noexcept
{
    return record_result(component, function, not_implemented);
}

std::size_t copy_log(XGameRuntimeShimLogEntry* output, std::size_t capacity) noexcept
{
    AcquireSRWLockShared(&log_state.lock);
    const auto copied = output && capacity < log_state.count ? capacity : log_state.count;
    const auto actual_copies = output ? copied : 0;
    for (std::size_t i = 0; i < actual_copies; ++i) {
        output[i] = log_state.entries[(log_state.first + i) % log_state.entries.size()];
    }
    const auto total = log_state.count;
    ReleaseSRWLockShared(&log_state.lock);
    return total;
}

void clear_log() noexcept
{
    AcquireSRWLockExclusive(&log_state.lock);
    log_state.first = 0;
    log_state.count = 0;
    log_state.next_sequence = 1;
    ReleaseSRWLockExclusive(&log_state.lock);
}
}  // namespace xgameruntime_shim

#define XGAMERUNTIME_DEFINE_THUNK(name, component, return_type, parameters, state, args, implementation) \
    XGAMERUNTIME_DEFINE_THUNK_##state(name, component, return_type, parameters, args, implementation)

#define XGAMERUNTIME_DEFINE_THUNK_stub(name, component, return_type, parameters, args, implementation) \
    extern "C" __declspec(dllexport) return_type WINAPI name parameters noexcept \
    { \
        return ::xgameruntime_shim::record_stub(#component, #name); \
    }

#define XGAMERUNTIME_DEFINE_THUNK_implemented_hr(name, component, return_type, parameters, args, implementation) \
    extern "C" __declspec(dllexport) return_type WINAPI name parameters noexcept \
    { \
        const HRESULT x2w_result = ::xgameruntime_shim::implementation args; \
        return ::xgameruntime_shim::record_result(#component, #name, x2w_result); \
    }

#define XGAMERUNTIME_DEFINE_THUNK_implemented_void(name, component, return_type, parameters, args, implementation) \
    extern "C" __declspec(dllexport) void WINAPI name parameters noexcept \
    { \
        const HRESULT x2w_result = ::xgameruntime_shim::implementation args; \
        ::xgameruntime_shim::record_result(#component, #name, x2w_result); \
    }

#define XGAMERUNTIME_DEFINE_THUNK_implemented_value(name, component, return_type, parameters, args, implementation) \
    extern "C" __declspec(dllexport) return_type WINAPI name parameters noexcept \
    { \
        const return_type x2w_result = ::xgameruntime_shim::implementation args; \
        ::xgameruntime_shim::record_result(#component, #name, S_OK); \
        return x2w_result; \
    }

XGAMERUNTIME_THUNK_TABLE(XGAMERUNTIME_DEFINE_THUNK)
#undef XGAMERUNTIME_DEFINE_THUNK_implemented_value
#undef XGAMERUNTIME_DEFINE_THUNK_implemented_void
#undef XGAMERUNTIME_DEFINE_THUNK_implemented_hr
#undef XGAMERUNTIME_DEFINE_THUNK_stub
#undef XGAMERUNTIME_DEFINE_THUNK

extern "C" __declspec(dllexport) std::size_t WINAPI XGameRuntimeShimGetLog(
    XGameRuntimeShimLogEntry* entries, std::size_t capacity) noexcept
{
    return xgameruntime_shim::copy_log(entries, capacity);
}

extern "C" __declspec(dllexport) void WINAPI XGameRuntimeShimClearLog() noexcept
{
    xgameruntime_shim::clear_log();
}
