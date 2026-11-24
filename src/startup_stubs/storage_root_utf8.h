// Windows environment strings are UTF-16; GDK narrow strings use UTF-8.
#pragma once
#include <windows.h>
#include "storage_root.h"

namespace x2w_startup {
// Preserve out on every refusal, including invalid UTF-16 and UTF-8 byte expansion.
inline bool storage_root_utf8(const wchar_t* value, char* out)
{
    if (!value || !*value) return false;
    char encoded[storage_root_capacity];
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                             encoded, sizeof(encoded), nullptr, nullptr)) return false;
    return storage_root(encoded, out);
}
}  // namespace x2w_startup
