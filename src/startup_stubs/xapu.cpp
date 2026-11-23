// Desktop xapu.dll trap stubs.
// Every export traps; see trap.h. Local scaffolding, not an ABI claim.
#define X2W_TRAP_MODULE "xapu.dll"
#define X2W_TRAP_MODULE_ID 3
#include "startup_stubs/trap.h"
#include "startup_stubs/local_log.h"

// LOCAL EXPERIMENT ONLY (local/startup-v2, never pushed). With X2W_AUDIO_PERMISSIVE=1 the exports
// stand in for the audio command queue; otherwise they trap. Call shapes from the title:
//   XApuConnect(input*, output*, handle*) -> HRESULT (the handle is later passed to the others)
//   XApuGetResultCount(handle, 0) -> count
//   XApuDequeueResult(handle, result*) -> HRESULT; the caller zeroes 0x58 bytes and reads a type at
//     +0 (1..5 seen in its switch), a slot index at +4 and a value at +0x34
//   XApuEnqueueCommand(handle, command*, ...) -> HRESULT
//   XApuDisconnect(handle)
// Nothing is processed: commands are accepted and logged, and no result is ever produced.
namespace {
using u64 = std::uint64_t;
bool permissive() noexcept { return x2w_local::env_on("X2W_AUDIO_PERMISSIVE"); }
int connection;  // its address is the handle

void hex(const void* data, std::size_t bytes, char* out, std::size_t size)
{
    out[0] = 0;
    if (!data || !x2w_local::readable(data, bytes)) return;
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t used = 0;
    for (std::size_t i = 0; i < bytes && used + 4 < size; ++i) used += std::snprintf(out + used, size - used, "%02x ", p[i]);
}
}  // namespace

extern "C" __declspec(noinline) long XApuConnect(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuConnect", 0, rcx, rdx, r8, r9, _ReturnAddress());
    char input[3 * 0x18 + 1], output[3 * 0x10 + 1];
    hex(reinterpret_cast<const void*>(rcx), 0x18, input, sizeof(input));
    hex(reinterpret_cast<const void*>(rdx), 0x10, output, sizeof(output));
    if (r8) *reinterpret_cast<void**>(r8) = &connection;
    x2w_local::logf("x2w-audio XApuConnect input=[%s] output(before)=[%s] -> handle %p, S_OK", input, output,
                    static_cast<void*>(&connection));
    return r8 ? 0 : static_cast<long>(0x80004003);
}

extern "C" __declspec(noinline) std::uint32_t XApuGetResultCount(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuGetResultCount", 4, rcx, rdx, r8, r9, _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 2)) x2w_local::logf("x2w-audio XApuGetResultCount handle=0x%llx rdx=0x%llx -> 0", rcx, rdx);
    return 0;
}

extern "C" __declspec(noinline) long XApuDequeueResult(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuDequeueResult", 1, rcx, rdx, r8, r9, _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 2)) x2w_local::logf("x2w-audio XApuDequeueResult handle=0x%llx -> E_FAIL (no results)", rcx);
    return static_cast<long>(0x80004005);
}

extern "C" __declspec(noinline) long XApuEnqueueCommand(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuEnqueueCommand", 3, rcx, rdx, r8, r9, _ReturnAddress());
    static LONG calls;
    if (x2w_local::first_calls(calls, 24)) {
        char command[3 * 0x60 + 1];
        hex(reinterpret_cast<const void*>(rdx), 0x60, command, sizeof(command));
        const auto offset = reinterpret_cast<u64>(_ReturnAddress()) - reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        x2w_local::logf("x2w-audio XApuEnqueueCommand r8=0x%llx r9=0x%llx ret=+0x%llx command=[%s] -> S_OK", r8, r9, offset,
                        command);
    }
    return 0;
}

extern "C" __declspec(noinline) long XApuDisconnect(u64 rcx, u64 rdx, u64 r8, u64 r9)
{
    if (!permissive())
        x2w_trap::fire(X2W_TRAP_MODULE, X2W_TRAP_MODULE_ID, "XApuDisconnect", 2, rcx, rdx, r8, r9, _ReturnAddress());
    x2w_local::logf("x2w-audio XApuDisconnect handle=0x%llx", rcx);
    return 0;
}
