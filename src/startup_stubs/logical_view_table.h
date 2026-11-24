#pragma once
#include <cstdint>

namespace x2w_startup {
// The translated draw reads a bounded part of a title-owned table and creates its
// own physical views. The logical GPU handle is never submitted to the driver.
struct ViewTable {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;

    std::uint64_t record(std::uint64_t offset, std::uint64_t size) const
    {
        return address && offset <= bytes && size <= bytes - offset ? address + offset : 0;
    }
};

inline ViewTable resolve_view_table(std::uint64_t gpu, std::uint64_t cpu, unsigned count,
                                    unsigned stride, std::uint64_t handle)
{
    const std::uint64_t bytes = std::uint64_t{count} * stride;
    if (!gpu || !cpu || !stride || cpu > UINT64_MAX - bytes || handle < gpu) return {};
    const auto offset = handle - gpu;
    if (offset >= bytes || offset % stride) return {};
    return {cpu + offset, bytes - offset};
}
}  // namespace x2w_startup
