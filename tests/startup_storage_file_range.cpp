#include "startup_stubs/dstorage_file_read.h"

#include <cstdio>

int main()
{
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"x2w", 0, path)) return 1;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); return 1; }
    const unsigned char source[]{0x11, 0x32, 0x53, 0x74, 0x95};
    DWORD written = 0;
    bool ok = WriteFile(file, source, sizeof(source), &written, nullptr) && written == sizeof(source);
    std::vector<unsigned char> staging;
    ok = ok && x2w_startup::read_storage_file(file, 2, 3, staging) &&
         staging == std::vector<unsigned char>{0x53, 0x74, 0x95};
    const auto previous = staging;
    const auto capacity = staging.capacity();
    struct Range { std::uint64_t offset; DWORD count; };
    for (const Range bad : {Range{4, 2}, Range{5, 1}, Range{0, 64u << 20}, Range{UINT64_MAX - 1, 4}}) {
        if (x2w_startup::read_storage_file(file, bad.offset, bad.count, staging) ||
            staging != previous || staging.capacity() != capacity) {
            std::fprintf(stderr, "invalid offset %llu count %lu changed staging or returned success\n",
                         static_cast<unsigned long long>(bad.offset), bad.count);
            ok = false;
        }
    }
    CloseHandle(file);
    if (x2w_startup::read_storage_file(INVALID_HANDLE_VALUE, 0, 1, staging) ||
        staging != previous || staging.capacity() != capacity) ok = false;
    return ok ? 0 : 1;
}
