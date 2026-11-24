#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

namespace x2w_startup {
inline bool read_storage_file(HANDLE file, std::uint64_t offset, DWORD count,
                              std::vector<unsigned char>& staging)
{
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) return false;
    const auto size = static_cast<std::uint64_t>(length.QuadPart);
    if (offset > size || count > size - offset) return false;

    staging.resize(count);
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    return ReadFile(file, staging.data(), count, &read, &position) && read == count;
}
}
