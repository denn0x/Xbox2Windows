// Desktop xg_xs.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xg_xs.dll"
#define X2W_TRAP_MODULE_ID 5
#include "startup_stubs/trap.h"

#include <array>
#include <cstdio>
#include <utility>

namespace {
bool permissive()
{
    char value[4]{};
    return GetEnvironmentVariableA("X2W_D3D12_PERMISSIVE", value, sizeof(value)) && value[0] == '1';
}

// Texture computer stand-in. Observed use: slot 8 per subresource as (dest, 0, subresource, src,
// row pitch, slice pitch), a copy from linear data into the console texture layout, and slot 1 at the
// end. The destination is title memory that the desktop GPU does not read, so nothing is copied.
template <int N>
__declspec(noinline) std::uint64_t computer_slot(std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 1) {
        char name[48];
        std::snprintf(name, sizeof(name), "TextureComputer::slot%d(no-op)", N);
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, N, rcx, rdx, r8, r9, _ReturnAddress());
    }
    return 0;
}

template <std::size_t... I>
constexpr auto computer_table(std::index_sequence<I...>)
{
    return std::array<void*, sizeof...(I)>{reinterpret_cast<void*>(&computer_slot<static_cast<int>(I)>)...};
}
}  // namespace

// LOCAL EXPERIMENT ONLY (local/startup-stubs, never pushed). XGCreateTextureComputer(desc, &computer)
// -> HRESULT (the caller does not check it). With X2W_D3D12_PERMISSIVE=1 returns the stand-in above;
// otherwise traps.
extern "C" __declspec(noinline) long XGCreateTextureComputer(std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8,
                                                             std::uint64_t r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGCreateTextureComputer", 0, rcx, rdx, r8, r9, _ReturnAddress());
    static const auto table = computer_table(std::make_index_sequence<32>{});
    static const void* const computer = table.data();
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 3)
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGCreateTextureComputer(stand-in)", 0, rcx, rdx, r8,
                         r9, _ReturnAddress());
    if (!rdx) return static_cast<long>(0x80004003);  // E_POINTER
    *reinterpret_cast<const void* const**>(rdx) = &computer;
    return 0;
}

// LOCAL EXPERIMENT ONLY (local/startup-stubs, never pushed). Call shape from the title's resource
// description builder: XGSuggestSwizzleMode(desc, 0, &swizzle_field, 1) -> HRESULT, after the
// title stored a default swizzle (0x100 or 0x102) in that field. Desktop resources do not use
// console swizzle modes, so with X2W_D3D12_PERMISSIVE=1 the title's default is kept and S_OK
// returned; otherwise this traps.
extern "C" __declspec(noinline) long XGSuggestSwizzleMode(std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8,
                                                          std::uint64_t r9)
{
    char value[4]{};
    if (!(GetEnvironmentVariableA("X2W_D3D12_PERMISSIVE", value, sizeof(value)) && value[0] == '1'))
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGSuggestSwizzleMode", 1, rcx, rdx, r8, r9, _ReturnAddress());
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 3)
        x2w_trap::record("x2w-xg", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XGSuggestSwizzleMode(keep default, S_OK)", 1, rcx,
                         rdx, r8, r9, _ReturnAddress());
    return 0;
}
