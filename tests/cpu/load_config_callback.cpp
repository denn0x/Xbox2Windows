// Calls both entries of the read-only callback table in load_config_callback.asm (#903). Compiled with
// /guard:cf, so each call goes through __guard_dispatch_icall_fptr with the target in RAX. The second entry
// is null, so it makes no call.
#pragma runtime_checks("", off)  // CRT-free: no /RTC helpers in Debug builds

extern "C" {
typedef int (*startup_callback)(int, int, int, void*);
extern const startup_callback callback_table[2];
extern unsigned long long marker;

int run_callbacks()
{
    int result = 0;
    if (callback_table[1])
        result = callback_table[1](1, 2, 3, nullptr);
    return result + callback_table[0](0x11, 0x22, 0x33, &marker);
}
}
