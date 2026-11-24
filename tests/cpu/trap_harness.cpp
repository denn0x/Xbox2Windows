// Native-only SEH boundary for tests/cpu/trap.py; it is never translated. seh_call calls fn(a, b, c, state) inside
// __try/__except and stores the structured exception code in *code, or 0 when fn returned.
#include <windows.h>
#include <cstdint>

using Fixture = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t*);

extern "C" __declspec(dllexport) uint64_t seh_call(Fixture fn, uint64_t a, uint64_t b, uint64_t c, uint64_t* state,
                                                   uint32_t* code)
{
    uint64_t result = 0;
    *code = 0;
    __try {
        result = fn(a, b, c, state);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
    }
    return result;
}
