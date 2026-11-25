// LOCAL memcut (out/gamerun/memcut_work/NOTES.md): X2W_MEMCUT_ALIAS_SLABS=1. Default off.
// Device slot 133 refuses to alias a buffer whose title memory is mapped slab pages (only private allocations can be
// opened with OpenExistingHeapFromAddress) and creates a committed desktop copy instead. Run 94: 138 such buffers,
// 755 MiB, all 64 KiB-aligned UAV buffers (x2w-d3d12 slot133 lines, "not aliased"). With the slab pool
// (X2W_XMEM_GPU_POOL=all) every slab is a long-lived pool section that section_va already opens as a desktop heap
// for title GPU addresses. Here such a buffer is placed on that heap at the pages' section offset instead, when all
// its pages are consecutive in ONE pool section (xmem x2w_xmem_pool_run, which never moves a slab and refuses
// file-backed and fresh slabs). The buffer then IS the title's memory, as on the console and as the startup-block
// buffers already are: GPU writes become visible to the title (today they land in a copy nobody reads).
// Risk: a wrongly translated shader that writes through such a buffer now corrupts title memory.
namespace {
bool memcut_alias_slabs()
{
    static const bool on = x2w_local::env_on("X2W_MEMCUT_ALIAS_SLABS");
    return on;
}
LONG memcut_alias_done, memcut_alias_refused;
LONG64 memcut_alias_bytes, memcut_alias_refused_bytes;

ID3D12Heap* memcut_slab_heap(void* address, UINT64 bytes, UINT64* offset)
{
    if (!memcut_alias_slabs() || !bytes) return nullptr;
    using PoolRun = BOOL(WINAPI*)(const void*, SIZE_T, HANDLE*, ULONG64*);
    static const PoolRun run = [] {
        const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
        const auto f = xmem ? reinterpret_cast<PoolRun>(GetProcAddress(xmem, "x2w_xmem_pool_run")) : nullptr;
        log("x2w-memcut alias slabs: X2W_MEMCUT_ALIAS_SLABS=1, xmem x2w_xmem_pool_run %s", f ? "found" : "MISSING (off)");
        return f;
    }();
    HANDLE section = nullptr;
    ULONG64 at = 0;
    ID3D12Heap* heap = nullptr;
    if (run && run(address, static_cast<SIZE_T>(bytes), &section, &at)) heap = section_heap(section);
    if (heap && at + bytes > heap->GetDesc().SizeInBytes) heap = nullptr;
    const LONG n = heap ? InterlockedIncrement(&memcut_alias_done) : InterlockedIncrement(&memcut_alias_refused);
    InterlockedAdd64(heap ? &memcut_alias_bytes : &memcut_alias_refused_bytes, static_cast<LONG64>(bytes));
    if (x2w_local::first_of(n, 8) || n % 256 == 0)
        log("x2w-memcut alias slabs: buffer %p width 0x%llx %s (aliased %ld = %lld MiB, refused %ld = %lld MiB)", address, bytes,
            heap ? "placed on its pool section" : "not one pool section run: copied as before", memcut_alias_done, memcut_alias_bytes >> 20,
            memcut_alias_refused, memcut_alias_refused_bytes >> 20);
    if (heap) *offset = at;
    return heap;
}
}  // namespace
