// XCL-11: a title graphics allocation used as a buffer window at its console address.
//
// On the console a graphics allocation's CPU pointer is also its GPU virtual address. On desktop
// D3D12 the CPU pointer and the driver-chosen GPU virtual address differ, so era_gpu_alias opens
// the allocation as an existing heap and era_title_va keeps the window-relative offset. This
// fixture checks two 64 KiB pages and an offset that is not page-aligned, copies bytes in both
// directions, and rejects an address outside the window, a page run that is not 64 KiB aligned,
// and a page that is reserved but not committed.
#include "bvh_fixture.h"
#include "era_gpu_alias.h"
#include "era_heap.h"
#include "era_title_va.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace bvh_fixture;

namespace {
// XALLOC_ATTRIBUTES: memoryType 1 (graphics) at bit 29, pageSize 1 (64 KB) at bit 14,
// alignment 16 (64 KB) at bit 24.
constexpr std::uint64_t graphics_64k = (1ull << 29) | (1ull << 14) | (16ull << 24);
constexpr std::uint64_t page = 64 * 1024;
constexpr std::uint64_t window_pages = 4;
constexpr std::uint64_t window_bytes = window_pages * page;
// Page 1, offset 0x1234. The span ends at 0x2f234, in page 2, so it covers exactly two pages.
constexpr std::uint64_t copy_offset = page + 0x1234;
constexpr std::uint64_t copy_bytes = 2 * page - 0x2000;
constexpr std::uint64_t guard_bytes = 64;

const char* hresult_name(HRESULT hr)
{
    if (hr == S_OK) return "S_OK";
    if (hr == E_BOUNDS) return "E_BOUNDS";
    if (hr == E_INVALIDARG) return "E_INVALIDARG";
    if (hr == E_POINTER) return "E_POINTER";
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS)) return "ERROR_INVALID_ADDRESS";
    return "unexpected";
}

void expect(bool condition, const char* what)
{
    if (!condition) throw std::runtime_error(what);
}

void expect_hr(HRESULT actual, HRESULT expected, const char* what)
{
    if (actual == expected) return;
    std::fprintf(stderr, "%s: expected %s (0x%08lx), got %s (0x%08lx)\n", what, hresult_name(expected),
                 static_cast<unsigned long>(expected), hresult_name(actual),
                 static_cast<unsigned long>(actual));
    throw std::runtime_error(what);
}

// Every 64 KiB page touched by [address, address + size) must be committed and belong to base's
// allocation. A span outside the window is E_BOUNDS; a reserved or foreign page is
// ERROR_INVALID_ADDRESS. This is fixture policy for the desktop window, not a console memory API.
HRESULT validate_span(const std::uint8_t* base, std::uint64_t bytes, std::uint64_t address,
                      std::uint64_t size)
{
    if (!base) return E_POINTER;
    if (size == 0) size = 1;
    const auto begin = reinterpret_cast<std::uint64_t>(base);
    if (address < begin) return E_BOUNDS;
    const std::uint64_t offset = address - begin;
    if (offset > bytes || size > bytes - offset) return E_BOUNDS;
    const std::uint64_t end = offset + size;
    for (std::uint64_t at = offset & ~(page - 1); at < end; at += page) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(base + at, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
            info.AllocationBase != static_cast<const void*>(base)) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        }
    }
    return S_OK;
}

struct Window {
    const std::uint8_t* base;
    std::uint64_t bytes;
    std::uint64_t gpu;
    const era_title_va::Table& table;

    // A byte span at any offset. The one-past-the-end span is rejected, and the returned GPU
    // address always has the same window-relative offset as the CPU address.
    HRESULT resolve(std::uint64_t address, std::uint64_t size, std::uint64_t* gpu_address) const
    {
        if (!gpu_address) return E_POINTER;
        *gpu_address = 0;
        const HRESULT span = validate_span(base, bytes, address, size);
        if (FAILED(span)) return span;
        const std::uint64_t translated = table.translate(address, size);
        if (!translated) return E_FAIL;
        *gpu_address = translated;
        return S_OK;
    }

    // A page run: both the start and the length must be multiples of 64 KiB.
    HRESULT resolve_pages(std::uint64_t address, std::uint64_t size, std::uint64_t* gpu_address) const
    {
        if (!gpu_address) return E_POINTER;
        *gpu_address = 0;
        const auto begin = reinterpret_cast<std::uint64_t>(base);
        if (size == 0 || address < begin || (address - begin) % page != 0 || size % page != 0)
            return E_INVALIDARG;
        return resolve(address, size, gpu_address);
    }

    std::uint64_t cpu_for(std::uint64_t gpu_address) const
    {
        return reinterpret_cast<std::uint64_t>(base) + (gpu_address - gpu);
    }
};

struct TitleAllocation {
    TitleAllocation() = default;
    void* base = era_heap::allocate(window_bytes, graphics_64k);
    ~TitleAllocation() { era_heap::release_with_attributes(base, graphics_64k); }
    TitleAllocation(const TitleAllocation&) = delete;
    TitleAllocation& operator=(const TitleAllocation&) = delete;
};

struct ReservedRange {
    explicit ReservedRange(std::uint64_t pages)
        : base(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pages * page, MEM_RESERVE, PAGE_READWRITE)))
    {
    }
    std::uint8_t* base;
    ~ReservedRange()
    {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
};

void fill(std::uint8_t* bytes, std::uint64_t count, std::uint32_t seed)
{
    for (std::uint64_t i = 0; i < count; ++i)
        bytes[i] = static_cast<std::uint8_t>((seed ^ static_cast<std::uint32_t>(i * 37u)) >> 3);
}

bool all_bytes(const std::uint8_t* bytes, std::uint64_t count, std::uint8_t value)
{
    for (std::uint64_t i = 0; i < count; ++i) {
        if (bytes[i] != value) return false;
    }
    return true;
}

void expect_bytes(const std::uint8_t* actual, const std::uint8_t* expected, std::uint64_t count,
                  const char* what)
{
    if (std::memcmp(actual, expected, static_cast<std::size_t>(count)) != 0)
        throw std::runtime_error(what);
}

void* map(ID3D12Resource* resource, const D3D12_RANGE& range, const char* what)
{
    void* mapped = nullptr;
    check(resource->Map(0, &range, &mapped), what);
    return mapped;
}
}  // namespace

bool check_gpu_alias_window(ID3D12Device* device)
{
    TitleAllocation title;
    if (!title.base) throw std::runtime_error("era_heap graphics allocation failed");
    auto* const base = static_cast<std::uint8_t*>(title.base);

    era_gpu_alias::Alias alias;
    HRESULT hr = era_gpu_alias::open(device, base, window_bytes, alias);
    if (hr == E_NOTIMPL) {
        std::puts("existing heaps are not supported by this adapter");
        return false;
    }
    check(hr, "Open the title window as a D3D12 heap");

    era_title_va::Table table;
    const std::uint64_t cpu_base = reinterpret_cast<std::uint64_t>(base);
    const std::uint64_t gpu_base = alias.buffer->GetGPUVirtualAddress();
    if (!table.add(cpu_base, window_bytes, gpu_base)) throw std::runtime_error("table rejected the window");
    const Window window{base, window_bytes, gpu_base, table};
    std::printf("window cpu=0x%llx gpu=0x%llx cpu_page_aligned=%d gpu_page_aligned=%d\n",
                static_cast<unsigned long long>(cpu_base), static_cast<unsigned long long>(gpu_base),
                (cpu_base & (page - 1)) == 0, (gpu_base & (page - 1)) == 0);
    expect((cpu_base & (page - 1)) == 0 && (gpu_base & (page - 1)) == 0,
           "the CPU and GPU window bases are 64 KiB aligned");

    // CPU and GPU addresses name the same page and the same byte inside it. The absolute bases
    // differ because desktop D3D12 chooses the GPU virtual address.
    for (std::uint64_t index = 0; index <= 2; ++index) {
        const std::uint64_t cpu_page = cpu_base + index * page;
        std::uint64_t gpu_page = 0;
        check(window.resolve_pages(cpu_page, page, &gpu_page), "Resolve a window page");
        expect(gpu_page == gpu_base + index * page, "the GPU page keeps the window-relative offset");
        expect((gpu_page & (page - 1)) == (cpu_page & (page - 1)),
               "the CPU and GPU page offsets are identical");
        expect(window.cpu_for(gpu_page) == cpu_page, "the GPU page round-trips to the same CPU page");
    }
    std::uint64_t span_gpu = 0;
    check(window.resolve(cpu_base + copy_offset, copy_bytes, &span_gpu), "Resolve the unaligned window span");
    expect(span_gpu == gpu_base + copy_offset, "the unaligned GPU span keeps the window offset");
    const std::uint64_t span_offset = span_gpu - gpu_base;
    expect(span_offset == copy_offset, "the GPU span is the aliased buffer at the requested offset");
    expect((span_gpu & (page - 1)) == (copy_offset & (page - 1)),
           "the unaligned CPU and GPU page offsets are identical");
    expect(window.cpu_for(span_gpu) == cpu_base + copy_offset, "the unaligned span round-trips");
    expect(window.cpu_for(span_gpu + copy_bytes - 1) == cpu_base + copy_offset + copy_bytes - 1,
           "the end of the unaligned span round-trips");
    const std::uint64_t first_page = copy_offset / page;
    const std::uint64_t last_page = (copy_offset + copy_bytes - 1) / page;
    expect(last_page - first_page + 1 == 2, "the unaligned span covers exactly two 64 KiB pages");

    // CPU write, GPU copy, CPU readback: the GPU reads the title bytes through the aliased window.
    std::vector<std::uint8_t> first(copy_bytes);
    fill(first.data(), copy_bytes, 0x51u);
    std::memcpy(base + copy_offset, first.data(), first.size());
    {
        const auto back = make_buffer(device, guard_bytes + copy_bytes + guard_bytes,
                                      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_RANGE all{0, guard_bytes + copy_bytes + guard_bytes};
        auto* mapped = static_cast<std::uint8_t*>(map(back.Get(), all, "Map the window readback"));
        std::memset(mapped, 0xcd, static_cast<std::size_t>(all.End));
        back->Unmap(0, nullptr);

        ComputeCommands cmd(device, nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto* list = cmd.list.Get();
        transition(list, alias.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(back.Get(), guard_bytes, alias.buffer.Get(), span_offset, copy_bytes);
        transition(list, alias.buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        cmd.submit_and_wait(device);

        const D3D12_RANGE read_range{0, guard_bytes + copy_bytes + guard_bytes};
        mapped = static_cast<std::uint8_t*>(map(back.Get(), read_range, "Read the window readback"));
        expect_bytes(mapped + guard_bytes, first.data(), copy_bytes,
                     "the GPU read of the CPU-written window differs");
        expect(all_bytes(mapped, guard_bytes, 0xcd) &&
                   all_bytes(mapped + guard_bytes + copy_bytes, guard_bytes, 0xcd),
               "the readback guard bytes changed");
        const D3D12_RANGE no_write{0, 0};
        back->Unmap(0, &no_write);
    }

    // CPU upload, GPU copy into the window, CPU read through the title pointer. This is the
    // GPU-write direction and checks the title's own bytes, not a separate destination buffer.
    std::vector<std::uint8_t> second(copy_bytes);
    fill(second.data(), copy_bytes, 0xc3u);
    std::memset(base + copy_offset - guard_bytes, 0xcd, guard_bytes);
    std::memset(base + copy_offset, 0xcd, copy_bytes);
    std::memset(base + copy_offset + copy_bytes, 0xcd, guard_bytes);
    {
        const auto upload = make_buffer(device, copy_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        const D3D12_RANGE no_read{0, 0};
        auto* mapped = static_cast<std::uint8_t*>(map(upload.Get(), no_read, "Map the window upload"));
        std::memcpy(mapped, second.data(), second.size());
        upload->Unmap(0, nullptr);

        ComputeCommands cmd(device, nullptr, D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto* list = cmd.list.Get();
        transition(list, alias.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(alias.buffer.Get(), span_offset, upload.Get(), 0, copy_bytes);
        transition(list, alias.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cmd.submit_and_wait(device);
    }
    expect_bytes(base + copy_offset, second.data(), copy_bytes,
                 "the title bytes after the GPU copy differ");
    expect(all_bytes(base + copy_offset - guard_bytes, guard_bytes, 0xcd) &&
               all_bytes(base + copy_offset + copy_bytes, guard_bytes, 0xcd),
           "the title guard bytes changed");

    // Negative controls. Each one is resolved before any command list is recorded, so none can
    // produce a readback; the sentinel buffer below must stay untouched.
    const auto negative_readback = make_buffer(device, 64, D3D12_HEAP_TYPE_READBACK,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_RANGE sentinel_range{0, 64};
    auto* sentinel = static_cast<std::uint8_t*>(map(negative_readback.Get(), sentinel_range,
                                                    "Map the negative-control readback"));
    std::memset(sentinel, 0xcd, 64);
    negative_readback->Unmap(0, nullptr);

    std::uint64_t rejected = ~0ull;
    hr = window.resolve(cpu_base + window_bytes, page, &rejected);
    expect_hr(hr, E_BOUNDS, "an address outside the window is rejected");
    std::printf("negative outside=%s (0x%08lx) gpu=0x%llx submitted=0\n", hresult_name(hr),
                static_cast<unsigned long>(hr), static_cast<unsigned long long>(rejected));

    hr = window.resolve_pages(cpu_base + 0x1000, page, &rejected);
    expect_hr(hr, E_INVALIDARG, "a page run that is not 64 KiB aligned is rejected");
    std::printf("negative alignment=%s (0x%08lx) gpu=0x%llx submitted=0\n", hresult_name(hr),
                static_cast<unsigned long>(hr), static_cast<unsigned long long>(rejected));

    ReservedRange reserved(2);
    if (!reserved.base) throw std::runtime_error("reserve-only VirtualAlloc failed");
    if (!VirtualAlloc(reserved.base, page, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("committing the first reserved page failed");
    hr = validate_span(reserved.base, 2 * page, reinterpret_cast<std::uint64_t>(reserved.base + page), page);
    expect_hr(hr, HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS), "a reserved page is not mapped");
    std::printf("negative uncommitted=%s (0x%08lx) submitted=0\n", hresult_name(hr),
                static_cast<unsigned long>(hr));

    ReservedRange reserve_only(1);
    if (!reserve_only.base) throw std::runtime_error("reserve-only VirtualAlloc failed");
    era_gpu_alias::Alias uncommitted;
    hr = era_gpu_alias::open(device, reserve_only.base, page, uncommitted);
    expect_hr(hr, E_INVALIDARG, "era_gpu_alias rejects a reserve-only page");
    expect(!uncommitted.heap && !uncommitted.buffer, "a rejected uncommitted page produced an alias");
    std::printf("negative uncommitted_open=%s (0x%08lx) alias=empty\n", hresult_name(hr),
                static_cast<unsigned long>(hr));

    sentinel = static_cast<std::uint8_t*>(map(negative_readback.Get(), sentinel_range,
                                              "Re-read the negative-control readback"));
    expect(all_bytes(sentinel, 64, 0xcd), "a negative control changed the readback");
    const D3D12_RANGE no_write{0, 0};
    negative_readback->Unmap(0, &no_write);

    std::printf("gpu_alias_window pages=2 offset=0x%llx bytes=0x%llx relative_identity=yes "
                "roundtrip=yes negative_readback=unchanged mismatches=0\n",
                static_cast<unsigned long long>(copy_offset), static_cast<unsigned long long>(copy_bytes));
    return true;
}
