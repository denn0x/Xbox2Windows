// Desktop chad.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "chad.dll"
#define X2W_TRAP_MODULE_ID 2
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed). With X2W_AUDIO_PERMISSIVE=1 the exports
// stand in for the DSP stream interface; otherwise they trap. Seen:
//   XDspConnectWithMaximumStreamLimit(queue memory, 0x60000, 30, 0x80, 1, &handle) -> 0 on success.
// The others have not been seen yet; they succeed (0) and log their first calls with registers.
// No stream is processed.
namespace {
using u64 = std::uint64_t;
bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }
int connection;

long accept(const char* name, unsigned index, u64 rcx, u64 rdx, u64 r8, u64 r9, void* return_address, LONG& calls)
{
    if (!permissive()) x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, index, rcx, rdx, r8, r9, return_address);
    if (x2w_local::first_calls(calls, 6))
        x2w_trap::record("x2w-audio", X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, name, index, rcx, rdx, r8, r9, return_address);
    return 0;
}
}  // namespace

// XDspActivate(connection, parameters, &state, &handle): the caller keeps both outputs next to each
// other in its effect object and later reads the state block (+0x4 compared with its own counter,
// +0xc tested for zero). Each activation gets its own zeroed 256-byte state block and a handle.
extern "C" __declspec(noinline) long XDspActivate(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    const long result = accept("XDspActivate", 0, rcx, rdx, r8, r9, _ReturnAddress(), calls);
    static unsigned char* blocks = static_cast<unsigned char*>(VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    static LONG used;
    const LONG index = InterlockedIncrement(&used) - 1;
    if (blocks && index < 4096) {
        void* state = blocks + static_cast<std::size_t>(index) * 256;
        if (r8 && x2w_local::writable(reinterpret_cast<void*>(r8), sizeof(void*))) *reinterpret_cast<void**>(r8) = state;
        if (r9 && x2w_local::writable(reinterpret_cast<void*>(r9), sizeof(u64))) *reinterpret_cast<u64*>(r9) = 0x58325344ull << 32 | (index + 1);
    }
    return result;
}

extern "C" __declspec(noinline) long XDspConnectWithMaximumStreamLimit(u64 rcx, u64 rdx, u64 r8, u64 r9, u64 flag, void** handle)
{
    static LONG calls;
    const long result = accept("XDspConnectWithMaximumStreamLimit", 1, rcx, rdx, r8, r9, _ReturnAddress(), calls);
    if (handle && x2w_local::writable(handle, sizeof(void*))) *handle = &connection;
    x2w_local::logf("x2w-audio XDspConnectWithMaximumStreamLimit memory=0x%llx bytes=0x%llx streams=%llu r9=0x%llx flag=%llu "
                    "handle_out=%p -> 0", rcx, rdx, r8, r9, flag, static_cast<void*>(handle));
    return result;
}

extern "C" __declspec(noinline) long XDspDeactivate(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    return accept("XDspDeactivate", 2, rcx, rdx, r8, r9, _ReturnAddress(), calls);
}

extern "C" __declspec(noinline) long XDspDisconnect(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    return accept("XDspDisconnect", 3, rcx, rdx, r8, r9, _ReturnAddress(), calls);
}

extern "C" __declspec(noinline) long XDspSubmitCommand(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    static LONG calls;
    return accept("XDspSubmitCommand", 4, rcx, rdx, r8, r9, _ReturnAddress(), calls);
}
