// era_title_gpu end to end. Graphics allocations are tracked before and after a device is
// attached. Blocks that are not the base of an allocation are tracked but cannot be aliased. The registry's shader view then drives TitleLookup (tests/gpu_alias_lookup.hlsl) on
// the GPU, and untrack, detach and re-attach are checked against the same translations.
#include "bvh_fixture.h"
#include "era_heap.h"
#include "era_query.h"
#include "era_title_gpu.h"
#include "LookupTitleAddresses.h"

#include <malloc.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace bvh_fixture;

namespace {
// XALLOC_ATTRIBUTES: memoryType 1 (graphics), pageSize 1 (64 KB), alignment 16 (64 KB).
constexpr std::uint64_t graphics_64k = (1ull << 29) | (1ull << 14) | (16ull << 24);
constexpr UINT descriptor_count = 8;  // matches Aliases[8] in tests/gpu_alias_lookup.hlsl
constexpr Word miss = 0xdeadbeef;

struct Graphics {
    explicit Graphics(std::size_t bytes) : base(era_heap::allocate(bytes, graphics_64k)), size(bytes)
    {
        if (!base) throw std::runtime_error("era_heap graphics allocation failed");
    }
    ~Graphics() { era_heap::release_with_attributes(base, graphics_64k); }
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;
    std::uint64_t begin() const { return reinterpret_cast<std::uint64_t>(base); }
    void* base;
    std::size_t size;
};

void expect(bool condition, const char* what)
{
    if (!condition) throw std::runtime_error(what);
}

// era_query's tail flag: set for regions registered as GPU-intended.
bool registered(const void* address)
{
    unsigned char buffer[64]{};
    return era_query::query(GetCurrentProcess(), address, buffer, sizeof(buffer)) && (buffer[0x30] & 1);
}

std::uint64_t address(const void* pointer) { return reinterpret_cast<std::uint64_t>(pointer); }

// Commits the highest free 64 KB block below `limit`. Walks down region by region and skips each
// allocation through its AllocationBase, so a large reservation just below `limit` (a driver's, for
// example) does not end the search. A block taken by another thread meanwhile is skipped.
void* commit_below(std::uint64_t limit)
{
    constexpr std::uint64_t granule = 0x10000;
    for (std::uint64_t top = limit & ~(granule - 1); top >= 2 * granule;) {
        const std::uint64_t start = top - granule;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(start), &info, sizeof(info)) != sizeof(info)) return nullptr;
        if (info.State != MEM_FREE) {
            top = address(info.AllocationBase) & ~(granule - 1);
            continue;
        }
        if (info.RegionSize >= granule)
            if (void* block = VirtualAlloc(reinterpret_cast<void*>(start), granule, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE))
                return block;
        top = start;
    }
    return nullptr;
}

void fill(const Graphics& g, Word seed)
{
    auto* words = static_cast<Word*>(g.base);
    for (std::size_t i = 0; i < g.size / sizeof(Word); ++i) words[i] = seed ^ static_cast<Word>(i * 2654435761u);
}

// The translation era_title_gpu reports agrees with the buffer and offset it locates.
void expect_aliased(std::uint64_t title, const char* what)
{
    ID3D12Resource* buffer = nullptr;
    std::uint64_t offset = 0;
    expect(era_title_gpu::locate(title, 4, &buffer, &offset), what);
    const auto gpu = buffer->GetGPUVirtualAddress() + offset;
    buffer->Release();
    expect(era_title_gpu::translate(title, 4) == gpu, what);
}

std::vector<Word> lookup(ID3D12Device* device, const era_title_gpu::ShaderView& view,
                         const std::vector<std::uint64_t>& records)
{
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptor_count,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create registry descriptor heap");
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT slot = 0; slot < descriptor_count; ++slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        ID3D12Resource* resource = nullptr;
        if (slot < view.resources.size()) {
            resource = view.resources[slot].Get();
            uav.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / sizeof(Word));
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(slot) * stride;
        device->CreateUnorderedAccessView(resource, nullptr, &uav, cpu);
    }
    Kernel kernel(device, {LookupTitleAddresses, sizeof(LookupTitleAddresses)});
    auto ranges = upload(device, view.ranges);
    auto inputs = upload(device, records);
    const std::size_t bytes = records.size() * 2 * sizeof(Word);
    auto results = make_buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto back = make_buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ComputeCommands cmd(device, kernel.pipeline.Get());
    auto* list = cmd.list.Get();
    for (const auto& resource : view.resources)
        transition(list, resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, results.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(kernel.root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootShaderResourceView(1, ranges->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, inputs->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, results->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(4, static_cast<UINT>(view.ranges.size()), 0);
    list->SetComputeRoot32BitConstant(4, static_cast<UINT>(records.size()), 1);
    list->Dispatch(static_cast<UINT>((records.size() + 63) / 64), 1, 1);
    for (const auto& resource : view.resources)
        transition(list, resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(list, results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, results.Get(), 0, bytes);
    cmd.submit_and_wait(device);
    return read(back.Get(), records.size() * 2);
}
}

bool check_gpu_alias_registry(ID3D12Device* device)
{
    // A trackable block that is not the base of an allocation, so it can never be aliased. Its region
    // is placed below `first`, so attach meets this failure before an allocation it must still alias.
    // It is taken before `first`: without bottom-up ASLR, first-fit placement can leave no free block
    // below `first`, but then puts `first` above this one. With randomized placement, search instead.
    void* below = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Graphics first(256 * 1024), second(64 * 1024);
    if (below && address(below) > first.begin()) {
        VirtualFree(below, 0, MEM_RELEASE);
        below = commit_below(first.begin());
    }
    expect(below != nullptr && address(below) < first.begin(), "region below the first allocation");
    void* heap_block = static_cast<char*>(below) + 0x1000;

    // Tracking without a device registers the regions with era_query and translates nothing.
    expect(era_title_gpu::track(first.base, first.size), "track first");
    expect(era_title_gpu::track(second.base, second.size), "track second");
    expect(era_title_gpu::track(heap_block, 8192), "track the interior block");
    expect(!era_title_gpu::track(first.base, 16), "duplicate base rejected");
    expect(!era_title_gpu::track(static_cast<char*>(second.base) + 64, 16), "overlap rejected");
    expect(!era_title_gpu::track(nullptr, 16) && !era_title_gpu::track(first.base, 0), "null and empty rejected");
    expect(registered(first.base) && registered(heap_block), "tracked regions are GPU-intended for era_query");
    expect(era_title_gpu::translate(first.begin(), 4) == 0, "no translation before attach");
    auto counts = era_title_gpu::counts();
    expect(counts.tracked == 3 && counts.aliased == 0, "three tracked, none aliased");
    expect(era_title_gpu::attach(nullptr) == E_INVALIDARG, "null device rejected");

    // Attach aliases what exists. The interior block fails and is reported; the rest still succeed.
    const HRESULT attached = era_title_gpu::attach(device);
    if (attached == E_NOTIMPL) {
        era_title_gpu::untrack(first.base);
        era_title_gpu::untrack(second.base);
        era_title_gpu::untrack(heap_block);
        VirtualFree(below, 0, MEM_RELEASE);
        std::puts("existing heaps are not supported by this adapter");
        return false;
    }
    expect(attached == E_INVALIDARG, "attach reports the block it cannot alias");
    counts = era_title_gpu::counts();
    expect(counts.tracked == 3 && counts.aliased == 2, "two of three aliased");
    expect_aliased(first.begin() + 0x100, "first allocation aliased");
    expect_aliased(second.begin() + second.size - 4, "second allocation's last word aliased");
    ID3D12Resource* none = nullptr;
    std::uint64_t offset = 0;
    expect(era_title_gpu::translate(address(heap_block), 4) == 0 &&
           !era_title_gpu::locate(address(heap_block), 4, &none, &offset), "the interior block stays untranslated");
    expect(era_title_gpu::translate(second.begin() + second.size - 2, 4) == 0 &&
           !era_title_gpu::locate(second.begin() + second.size - 2, 4, &none, &offset),
           "a span past the end is neither translated nor located");

    // An allocation tracked after attach is aliased at once.
    Graphics third(128 * 1024);
    expect(era_title_gpu::track(third.base, third.size), "track third after attach");
    expect_aliased(third.begin() + 8, "third allocation aliased when tracked");
    expect(era_title_gpu::counts().aliased == 3, "three aliased");

    // A block tracked after attach that cannot be aliased stays tracked, untranslated.
    void* late_block = _aligned_malloc(4096, 4096);
    expect(late_block != nullptr, "CRT allocation failed");
    expect(era_title_gpu::track(late_block, 4096), "tracking an unaliasable block after attach succeeds");
    counts = era_title_gpu::counts();
    expect(counts.tracked == 5 && counts.aliased == 3 && registered(late_block) &&
           era_title_gpu::translate(address(late_block), 4) == 0, "the late block is tracked but not aliased");
    expect(era_title_gpu::untrack(late_block), "untrack the late block");
    _aligned_free(late_block);

    // Attaching the same device again retries only what is not aliased.
    const auto first_gpu = era_title_gpu::translate(first.begin(), 4);
    expect(era_title_gpu::attach(device) == E_INVALIDARG && era_title_gpu::translate(first.begin(), 4) == first_gpu,
           "re-attach keeps existing aliases");

    // The shader view drives TitleLookup on the GPU. Expected words come from the CPU.
    fill(first, 0x11110000u);
    fill(second, 0x22220000u);
    fill(third, 0x33330000u);
    era_title_gpu::ShaderView view;
    expect(era_title_gpu::shader_view(view) && view.ranges.size() == 3 && view.resources.size() == 3,
           "shader view has the three aliased allocations");
    std::vector<std::uint64_t> records;
    for (const Graphics* g : {&first, &second, &third})
        for (std::uint64_t offset_in : {std::uint64_t{0}, std::uint64_t{4}, g->size / 2, g->size - 4})
            records.push_back(g->begin() + offset_in);
    records.push_back(address(heap_block));        // tracked but not aliased
    records.push_back(address(heap_block) + 4096);
    records.push_back(0x10);                       // not tracked
    const auto actual = lookup(device, view, records);
    unsigned hits = 0;
    for (std::size_t r = 0; r < records.size(); ++r) {
        Word slot = 0;
        for (std::size_t s = 0; s < view.ranges.size(); ++s) {
            const auto begin = (std::uint64_t{view.ranges[s].begin_high} << 32) | view.ranges[s].begin_low;
            const auto end = (std::uint64_t{view.ranges[s].end_high} << 32) | view.ranges[s].end_low;
            if (records[r] >= begin && records[r] < end) slot = static_cast<Word>(s + 1);
        }
        const Word value = slot ? *reinterpret_cast<const Word*>(records[r]) : miss;
        equal(actual[r * 2], value, static_cast<unsigned>(r), 0);
        equal(actual[r * 2 + 1], slot, static_cast<unsigned>(r), 1);
        hits += slot ? 1 : 0;
    }
    expect(hits == 12, "twelve records hit aliased allocations");

    // Untrack removes the translation, the era_query registration and the slot.
    expect(era_title_gpu::untrack(second.base) && !era_title_gpu::untrack(second.base), "untrack once");
    expect(era_title_gpu::translate(second.begin(), 4) == 0 && !registered(second.base), "untracked region forgotten");
    expect(era_title_gpu::shader_view(view) && view.ranges.size() == 2, "shader view without the untracked allocation");

    // Detach drops every alias and keeps tracking; a full re-attach succeeds without the interior block.
    era_title_gpu::detach();
    counts = era_title_gpu::counts();
    expect(counts.tracked == 3 && counts.aliased == 0, "detach keeps tracking");
    expect(era_title_gpu::translate(first.begin(), 4) == 0 && registered(first.base), "detach drops translations only");
    expect(era_title_gpu::untrack(heap_block), "untrack the interior block");
    expect(era_title_gpu::attach(device) == S_OK, "attach succeeds when everything can be aliased");
    expect(era_title_gpu::counts().aliased == 2, "both remaining allocations aliased again");
    expect_aliased(first.begin(), "first allocation aliased again");

    era_title_gpu::untrack(first.base);
    era_title_gpu::untrack(third.base);
    era_title_gpu::detach();
    VirtualFree(below, 0, MEM_RELEASE);
    expect(era_title_gpu::counts().tracked == 0, "everything untracked");
    std::printf("gpu_alias_registry records=%zu hits=%u attach_partial=0x%08lx mismatches=0\n", records.size(), hits,
                static_cast<unsigned long>(attached));
    return true;
}
