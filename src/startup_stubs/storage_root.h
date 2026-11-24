// Experimental (docs/startup-runtime.md): the persistent-local-storage root handed to a console
// executable, made from X2W_STORAGE_PATH.
//
// The executable appends names to the root as it is ("settings\settings.xml", "cloudpers\"), so
// the root must end in a separator: without one, "D:\saves" became "D:\savessettings\settings.xml".
#pragma once

#include <cstddef>
#include <cstring>

namespace x2w_startup {
// The longest root accepted, without the terminating NUL (MAX_PATH - 1, with room for a separator).
constexpr std::size_t storage_root_capacity = 260;

// Writes `value` with one trailing separator into out[storage_root_capacity]. A value that
// already ends in '\' or '/' is copied as it is. Returns false, leaving out alone, for a null or
// empty value and for one that does not fit with its separator.
inline bool storage_root(const char* value, char* out)
{
    const std::size_t length = value ? std::strlen(value) : 0;
    if (!length) return false;
    const bool separated = value[length - 1] == '\\' || value[length - 1] == '/';
    if (length + (separated ? 0 : 1) >= storage_root_capacity) return false;
    std::memcpy(out, value, length);
    if (!separated) out[length] = '\\';
    out[length + (separated ? 0 : 1)] = '\0';
    return true;
}
}  // namespace x2w_startup
