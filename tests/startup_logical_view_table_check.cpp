// Checks src/startup_stubs/logical_view_table.h without a device: the window of the title's
// descriptor memory that a root table handle resolves to, and the records a draw may read from it.
#include "startup_stubs/logical_view_table.h"

#include <climits>
#include <cstdint>
#include <cstdio>

namespace {
using u64 = std::uint64_t;
int failures;

void table(const char* name, u64 gpu, u64 cpu, unsigned count, unsigned stride, u64 handle, u64 address, u64 bytes)
{
    const x2w_startup::ViewTable got = x2w_startup::resolve_view_table(gpu, cpu, count, stride, handle);
    if (got.address != address || got.bytes != bytes) {
        std::printf("FAIL %s: address 0x%llx bytes 0x%llx, expected 0x%llx bytes 0x%llx\n", name,
                    static_cast<unsigned long long>(got.address), static_cast<unsigned long long>(got.bytes),
                    static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        ++failures;
    }
}

void rejected(const char* name, u64 gpu, u64 cpu, unsigned count, unsigned stride, u64 handle)
{
    table(name, gpu, cpu, count, stride, handle, 0, 0);
}

void record(const char* name, const x2w_startup::ViewTable& window, u64 offset, u64 size, u64 expected)
{
    const u64 got = window.record(offset, size);
    if (got != expected) {
        std::printf("FAIL %s: 0x%llx, expected 0x%llx\n", name, static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(expected));
        ++failures;
    }
}
}  // namespace

int main()
{
    // A logical heap larger than the desktop limit, with the 32-byte records a translated draw reads.
    constexpr u64 gpu = 0x10000000000, cpu = 0x20000000000;
    constexpr unsigned count = 1245184, stride = 32;
    constexpr u64 bytes = u64{count} * stride;

    // The window runs from the handle's record to the logical end.
    table("first record", gpu, cpu, count, stride, gpu, cpu, bytes);
    table("second record", gpu, cpu, count, stride, gpu + stride, cpu + stride, bytes - stride);
    table("last record", gpu, cpu, count, stride, gpu + bytes - stride, cpu + bytes - stride, stride);
    table("one record heap", gpu, cpu, 1, stride, gpu, cpu, stride);
    table("stride 48", gpu, cpu, 4, 48, gpu + 96, cpu + 96, 96);
    // The size is a 64-bit product of two 32-bit values.
    constexpr u64 largest = u64{UINT_MAX} * UINT_MAX;
    table("largest heap, last record", 0x1000, 0x2000, UINT_MAX, UINT_MAX, 0x1000 + largest - UINT_MAX,
          0x2000 + largest - UINT_MAX, UINT_MAX);
    // Memory may end exactly at the top of the address space.
    table("memory ending at UINT64_MAX", gpu, UINT64_MAX - bytes, count, stride, gpu, UINT64_MAX - bytes, bytes);

    // Handles outside the heap or between records give no window.
    rejected("one past the last record", gpu, cpu, count, stride, gpu + bytes);
    rejected("far past the last record", gpu, cpu, count, stride, gpu + 2 * bytes);
    rejected("inside the second record", gpu, cpu, count, stride, gpu + stride + 8);
    rejected("inside a stride 48 record", gpu, cpu, 4, 48, gpu + 64);
    rejected("one record below the heap", gpu, cpu, count, stride, gpu - stride);
    // Below the heap, even where the unsigned difference would land on a record.
    rejected("below a heap at the top of the address space", UINT64_MAX - 63, cpu, 16, stride, 0xC0);
    // Missing heap fields.
    rejected("null GPU start", 0, cpu, count, stride, stride);
    rejected("null CPU start", gpu, 0, count, stride, gpu);
    rejected("zero stride", gpu, cpu, count, 0, gpu);
    rejected("zero count", gpu, cpu, 0, stride, gpu);
    // Memory that would wrap past the top of the address space.
    rejected("memory ending past UINT64_MAX", gpu, UINT64_MAX - bytes + 1, count, stride, gpu);
    rejected("largest heap past UINT64_MAX", 0x1000, UINT64_MAX - largest + 1, UINT_MAX, UINT_MAX, 0x1000);

    // Records inside a four-record window.
    const x2w_startup::ViewTable window{cpu + stride, 4 * stride};
    record("first record", window, 0, stride, cpu + stride);
    record("last record", window, 3 * stride, stride, cpu + 4 * stride);
    record("whole window", window, 0, 4 * stride, cpu + stride);
    record("record crossing the end", window, 3 * stride + 1, stride, 0);
    record("record one byte too long", window, 3 * stride, stride + 1, 0);
    record("window one byte too long", window, 0, 4 * stride + 1, 0);
    record("record at the end", window, 4 * stride, stride, 0);
    record("empty record past the end", window, 4 * stride + 1, 0, 0);
    record("offset at UINT64_MAX", window, UINT64_MAX, stride, 0);
    record("size at UINT64_MAX", window, 0, UINT64_MAX, 0);
    // An unresolved table gives no record at any offset.
    record("empty table, offset 0", x2w_startup::ViewTable{}, 0, stride, 0);
    record("empty table, offset 64", x2w_startup::ViewTable{}, 2 * stride, stride, 0);
    record("empty table, empty record", x2w_startup::ViewTable{}, 2 * stride, 0, 0);

    // Through both: the final logical record, and nothing beyond it.
    const auto last = x2w_startup::resolve_view_table(gpu, cpu, count, stride, gpu + bytes - stride);
    record("last logical record", last, 0, stride, cpu + bytes - stride);
    record("past the last logical record", last, stride, stride, 0);
    record("last logical record, one byte too long", last, 0, stride + 1, 0);

    if (failures) return 1;
    std::printf("logical view table: windows and records exact\n");
    return 0;
}
