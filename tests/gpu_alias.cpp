// A title graphics allocation (era_heap, memory type 1) opened as a D3D12 heap with
// era_gpu_alias, addressed through era_title_va. A compute shader reads and writes the
// allocation at translated GPU addresses; the CPU then checks the results through the title's
// own pointer, with no copy or readback buffer in between.
#include "bvh_fixture.h"
#include "era_gpu_alias.h"
#include "era_heap.h"
#include "era_title_va.h"
#include "AliasTransform.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace bvh_fixture;

namespace {
// XALLOC_ATTRIBUTES: memoryType 1 (graphics) at bit 29, pageSize 1 (64 KB) at bit 14,
// alignment 16 (64 KB) at bit 24.
constexpr std::uint64_t graphics_64k = (1ull << 29) | (1ull << 14) | (16ull << 24);
constexpr std::size_t allocation_bytes = 256 * 1024;
constexpr std::size_t source_offset = 0x1000;
constexpr std::size_t destination_offset = 0x20000;
constexpr Word count = 4096;
constexpr Word guard_words = 64;
constexpr Word sentinel = 0xdeadbeef;

struct TitleAllocation {
    void* base = era_heap::allocate(allocation_bytes, graphics_64k);
    ~TitleAllocation() { era_heap::release_with_attributes(base, graphics_64k); }
};

Word source_word(Word i) { return (i * 2654435761u) ^ 0x13579bdfu; }
Word expected_word(Word i) { return (source_word(i) ^ 0xa5a5a5a5u) + i; }

void dispatch(ID3D12Device* device, const Kernel& kernel, ID3D12Resource* src_buffer,
              std::uint64_t source, ID3D12Resource* dst_buffer, std::uint64_t destination)
{
    ComputeCommands cmd(device, kernel.pipeline.Get());
    auto* list = cmd.list.Get();
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(list, src_buffer, common, uav);
    if (dst_buffer != src_buffer) transition(list, dst_buffer, common, uav);
    list->SetComputeRootSignature(kernel.root.Get());
    list->SetComputeRootUnorderedAccessView(0, source);
    list->SetComputeRootUnorderedAccessView(1, destination);
    list->SetComputeRoot32BitConstant(2, count, 0);
    list->Dispatch(count / 64, 1, 1);
    transition(list, src_buffer, uav, common);
    if (dst_buffer != src_buffer) transition(list, dst_buffer, uav, common);
    cmd.submit_and_wait(device);
}

void check_address_width(ID3D12Device* device, const Kernel& kernel, unsigned char* low_cpu,
                         const era_gpu_alias::Alias& low_alias)
{
    TitleAllocation high;
    era_gpu_alias::Alias high_alias;
    if (!high.base) throw std::runtime_error("second graphics allocation failed");
    check(era_gpu_alias::open(device, high.base, allocation_bytes, high_alias), "Open second title allocation");

    // Synthetic guest addresses let every host exercise a collision in exactly the same bit.
    constexpr std::uint64_t low = 0x20000000;
    constexpr std::uint64_t high_bit = 1ull << 40;
    constexpr std::uint64_t high_address = high_bit | low;
    era_title_va::Table table;
    if (!table.add(low, allocation_bytes, low_alias.buffer->GetGPUVirtualAddress()) ||
        !table.add(high_address, allocation_bytes, high_alias.buffer->GetGPUVirtualAddress()))
        throw std::runtime_error("failed to register both guest ranges");

    auto* low_dst = reinterpret_cast<Word*>(low_cpu + destination_offset);
    auto* high_dst = reinterpret_cast<Word*>(static_cast<unsigned char*>(high.base) + destination_offset);
    const auto source = table.translate(low + source_offset, count * sizeof(Word));
    const auto destination = high_address + destination_offset;
    const auto masked = destination & (high_bit - 1);
    for (bool truncate : {false, true}) {
        for (Word i = 0; i < count + guard_words; ++i) low_dst[i] = high_dst[i] = sentinel;
        low_dst[-1] = high_dst[-1] = sentinel;
        const auto gpu_va = table.translate(truncate ? masked : destination, count * sizeof(Word));
        const auto& target = truncate ? low_alias : high_alias;
        if (!source || gpu_va != target.buffer->GetGPUVirtualAddress() + destination_offset)
            throw std::runtime_error("destination lookup selected the wrong GPU range");
        dispatch(device, kernel, low_alias.buffer.Get(), source, target.buffer.Get(), gpu_va);

        equal(low_dst[-1], sentinel, 0, 2);
        equal(high_dst[-1], sentinel, 0, 3);
        for (Word i = 0; i < count + guard_words; ++i) {
            equal(low_dst[i], truncate && i < count ? expected_word(i) : sentinel, i, 0);
            equal(high_dst[i], !truncate && i < count ? expected_word(i) : sentinel, i, 1);
        }
    }
    if (!table.remove(high_address) || table.translate(destination) ||
        table.translate(masked) != low_alias.buffer->GetGPUVirtualAddress() + destination_offset)
        throw std::runtime_error("removing one range disturbed its low-bit neighbor");
    std::printf("gpu_alias high_address_bit=40 words=%u truncation_writes_low_range=1\n", count);
}
}

bool check_gpu_alias(ID3D12Device* device)
{
    TitleAllocation title;
    if (!title.base) throw std::runtime_error("era_heap graphics allocation failed");
    auto* base = static_cast<unsigned char*>(title.base);

    era_gpu_alias::Alias alias;
    const HRESULT hr = era_gpu_alias::open(device, base, allocation_bytes, alias);
    if (hr == E_NOTIMPL) {
        std::puts("existing heaps are not supported by this adapter");
        return false;
    }
    check(hr, "Open the title allocation as a D3D12 heap");

    // Failure cases: interior pointer, bad arguments, oversized span, address-space wrap, and
    // uncommitted, PAGE_NOACCESS, or PAGE_GUARD tails. open must reject all of them before calling
    // D3D12, so the debug layer reports no error for these controls.
    era_gpu_alias::Alias interior;
    const HRESULT interior_hr = era_gpu_alias::open(device, base + 0x1000, allocation_bytes - 0x1000, interior);
    if (interior_hr != E_INVALIDARG || interior.heap || interior.buffer || interior.size != 0)
        throw std::runtime_error("an interior pointer was not rejected with E_INVALIDARG");
    if (era_gpu_alias::open(nullptr, base, allocation_bytes, interior) != E_INVALIDARG ||
        era_gpu_alias::open(device, nullptr, allocation_bytes, interior) != E_INVALIDARG ||
        era_gpu_alias::open(device, base, 0, interior) != E_INVALIDARG ||
        era_gpu_alias::open(device, base, allocation_bytes + 0x10000, interior) != E_INVALIDARG ||
        era_gpu_alias::open(device, base, (std::numeric_limits<std::uint64_t>::max)(), interior) != E_INVALIDARG) {
        throw std::runtime_error("invalid open arguments or oversized span were not rejected with E_INVALIDARG");
    }
    constexpr std::size_t partial_bytes = 128 * 1024;
    void* partial = VirtualAlloc(nullptr, partial_bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!partial) throw std::runtime_error("reserve-only VirtualAlloc failed");
    auto* partial_tail = static_cast<unsigned char*>(partial) + 64 * 1024;
    const bool partial_committed = VirtualAlloc(partial, 64 * 1024, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    const HRESULT partial_hr =
        partial_committed ? era_gpu_alias::open(device, partial, partial_bytes, interior) : E_FAIL;
    const bool noaccess_committed =
        partial_committed && VirtualAlloc(partial_tail, 64 * 1024, MEM_COMMIT, PAGE_NOACCESS) != nullptr;
    const HRESULT noaccess_hr =
        noaccess_committed ? era_gpu_alias::open(device, partial, partial_bytes, interior) : E_FAIL;
    DWORD old_protect = 0;
    const bool guard_protected =
        noaccess_committed && VirtualProtect(partial_tail, 64 * 1024, PAGE_READWRITE | PAGE_GUARD, &old_protect) != 0;
    const HRESULT guard_hr =
        guard_protected ? era_gpu_alias::open(device, partial, partial_bytes, interior) : E_FAIL;
    VirtualFree(partial, 0, MEM_RELEASE);
    if (!partial_committed || partial_hr != E_INVALIDARG || !noaccess_committed || noaccess_hr != E_INVALIDARG ||
        !guard_protected || guard_hr != E_INVALIDARG || interior.heap || interior.buffer || interior.size != 0) {
        throw std::runtime_error("uncommitted, PAGE_NOACCESS, or PAGE_GUARD span was not rejected with E_INVALIDARG");
    }

    era_title_va::Table table;
    const auto title_begin = reinterpret_cast<std::uint64_t>(base);
    const D3D12_GPU_VIRTUAL_ADDRESS gpu_begin = alias.buffer->GetGPUVirtualAddress();
    if (!table.add(title_begin, allocation_bytes, gpu_begin)) throw std::runtime_error("table rejected the alias");
    const std::uint64_t source = table.translate(title_begin + source_offset, count * sizeof(Word));
    const std::uint64_t destination = table.translate(title_begin + destination_offset, count * sizeof(Word));
    if (source != gpu_begin + source_offset || destination != gpu_begin + destination_offset)
        throw std::runtime_error("title address translation differs from the alias offset");

    auto* source_words = reinterpret_cast<Word*>(base + source_offset);
    auto* destination_words = reinterpret_cast<Word*>(base + destination_offset);
    for (Word i = 0; i < count; ++i) {
        source_words[i] = source_word(i);
        destination_words[i] = sentinel;
    }
    for (Word i = count; i < count + guard_words; ++i) destination_words[i] = sentinel;

    Kernel kernel(device, {AliasTransform, sizeof(AliasTransform)});
    dispatch(device, kernel, alias.buffer.Get(), source, alias.buffer.Get(), destination);

    for (Word i = 0; i < count; ++i) {
        equal(destination_words[i], expected_word(i), i, 0);
        equal(source_words[i], source_word(i), i, 1);
    }
    for (Word i = count; i < count + guard_words; ++i) equal(destination_words[i], sentinel, i, 2);
    check_address_width(device, kernel, base, alias);
    std::printf("gpu_alias bytes=%zu words=%u source=+0x%zx destination=+0x%zx guard_words=%u "
                "interior_open=0x%08lx mismatches=0\n",
                allocation_bytes, count, source_offset, destination_offset, guard_words,
                static_cast<unsigned long>(interior_hr));
    return true;
}
