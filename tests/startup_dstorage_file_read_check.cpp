// Checks src/startup_stubs/dstorage_file_read.h at file offsets that need the high 32 bits of the
// OVERLAPPED position. The file is a sparse temporary file of 4 GiB + 20 bytes, so only the four
// written runs take space on disk. Without sparse-file support the check exits 77 before it writes
// anything, since a plain file would be zero-filled up to 4 GiB.
// --mutate alias: expect the bytes of the run 4 GiB lower at the high offset (a negative control).
#include "startup_stubs/dstorage_file_read.h"

#include <winioctl.h>

#include <cstdio>
#include <cstring>

namespace {
using Bytes = std::vector<unsigned char>;
int failures;

// The four runs. The positions are written as literal OVERLAPPED halves, not from a 64-bit offset.
struct Run { DWORD low, high; unsigned char bytes[4]; };
constexpr Run low_alias{0x00000010, 0, {0xA1, 0xA2, 0xA3, 0xA4}};   // 0x0'0000'0010
constexpr Run above_2g{0x80000010, 0, {0xD1, 0xD2, 0xD3, 0xD4}};    // 0x0'8000'0010
constexpr Run across_4g{0xFFFFFFFE, 0, {0xC1, 0xC2, 0xC3, 0xC4}};   // 0x0'FFFF'FFFE .. 0x1'0000'0001
constexpr Run above_4g{0x00000010, 1, {0xB1, 0xB2, 0xB3, 0xB4}};    // 0x1'0000'0010, ends the file
constexpr std::uint64_t file_size = 0x100000014;

bool write_run(HANDLE file, const Run& run)
{
    OVERLAPPED position{};
    position.Offset = run.low;
    position.OffsetHigh = run.high;
    DWORD written = 0;
    return WriteFile(file, run.bytes, sizeof(run.bytes), &written, &position) && written == sizeof(run.bytes);
}

void read(const char* name, HANDLE file, std::uint64_t offset, const unsigned char (&expected)[4])
{
    Bytes staging;
    if (!x2w_startup::read_storage_file(file, offset, 4, staging)) {
        std::printf("FAIL %s: read at 0x%llx failed\n", name, static_cast<unsigned long long>(offset));
        ++failures;
    } else if (staging != Bytes(expected, expected + 4)) {
        std::printf("FAIL %s: read at 0x%llx gave %zu bytes %02x %02x %02x %02x, expected %02x %02x %02x %02x\n",
                    name, static_cast<unsigned long long>(offset), staging.size(),
                    staging.size() > 0 ? staging[0] : 0, staging.size() > 1 ? staging[1] : 0,
                    staging.size() > 2 ? staging[2] : 0, staging.size() > 3 ? staging[3] : 0,
                    expected[0], expected[1], expected[2], expected[3]);
        ++failures;
    }
}

void rejected(const char* name, HANDLE file, std::uint64_t offset, DWORD count)
{
    Bytes staging{0x5A, 0x5B};
    const auto capacity = staging.capacity();
    if (x2w_startup::read_storage_file(file, offset, count, staging) || staging != Bytes{0x5A, 0x5B} ||
        staging.capacity() != capacity) {
        std::printf("FAIL %s: offset 0x%llx count %lu returned success or changed staging\n", name,
                    static_cast<unsigned long long>(offset), count);
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    const bool alias = argc > 2 && std::strcmp(argv[1], "--mutate") == 0 && std::strcmp(argv[2], "alias") == 0;
    if (argc > 1 && !alias) {
        std::printf("usage: startup_dstorage_file_read_check [--mutate alias]\n");
        return 2;
    }

    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"x2w", 0, path)) return 1;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); return 1; }
    DWORD returned = 0;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) ||
        !GetFileInformationByHandle(file, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE)) {
        std::printf("SKIP: the temporary folder does not take sparse files (error %lu)\n", GetLastError());
        CloseHandle(file);
        return 77;
    }
    if (!write_run(file, low_alias) || !write_run(file, above_2g) || !write_run(file, across_4g) ||
        !write_run(file, above_4g)) {
        std::printf("FAIL: could not write the sparse runs (error %lu)\n", GetLastError());
        CloseHandle(file);
        return 1;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || static_cast<std::uint64_t>(length.QuadPart) != file_size) {
        std::printf("FAIL: file size 0x%llx, expected 0x%llx\n", static_cast<unsigned long long>(length.QuadPart),
                    static_cast<unsigned long long>(file_size));
        CloseHandle(file);
        return 1;
    }

    read("low run", file, 0x10, low_alias.bytes);
    read("run above 2 GiB", file, 0x80000010, above_2g.bytes);
    read("run across 4 GiB", file, 0xFFFFFFFE, across_4g.bytes);
    // The last four bytes of the file, at the offset 4 GiB above the low run.
    read("run above 4 GiB, ending at EOF", file, 0x100000010, alias ? low_alias.bytes : above_4g.bytes);

    // The range check compares 64-bit values.
    rejected("one byte past EOF above 4 GiB", file, 0x100000011, 4);
    rejected("offset past EOF above 4 GiB", file, 0x100000015, 1);

    CloseHandle(file);
    if (failures) return 1;
    std::printf("dstorage file read: offsets above 2 and 4 GiB exact\n");
    return 0;
}
