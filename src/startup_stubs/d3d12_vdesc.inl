// Local experiment (never upstream): vdesc, the gfx10 descriptor words of heap views (agent meshrt).
// Included inside d3d12_xs_device.cpp's anonymous namespace, right after d3d12_compute.inl.
//
// A translated program that reads the WORDS of a resource-heap descriptor as numbers (manifest `heapwords 1`,
// mesh_work/RUNTIME_NEEDS.md 6.3): word 3 of a buffer V# (format, swizzle) for typed buffer loads through a heap
// descriptor, T# words 1-3 (width, height, mips) for image programs. The heap slots hold view tokens (X2WV + id),
// so the words come from a buffer indexed by the same view id as vinfo: 32 bytes per id (8 dwords, the words
// gfx10_view_words.h synthesises; view_record_words in d3d12_compute.inl, shared with `udsrc descword`), zeros
// for ids without words. Bound as root SRV t2 space1 (create_draw_root, after heapmem t0 and vinfo t1).
//
// Inert unless a manifest asks: the buffer (view_ids_max x 32 bytes = 2.9 MB, upload heap) is created by the
// first bound `heapwords 1` translation and then filled for every view fill_view_kinds fills (draw_lock held).

ID3D12Resource* vdesc_buffer;
std::uint32_t* vdesc_words;
LONG vdesc_image, vdesc_buffer_views, vdesc_none;  // ids filled, by kind (totals)

// fill_view_kinds (d3d12_compute.inl), for every view record it fills; draw_lock held.
void vdesc_fill(const ViewRecord& record)
{
    if (!vdesc_words || record.id >= view_ids_max) return;
    std::uint32_t out[8];
    bool image = false, unmapped = false;
    const bool made = view_record_words(record, out, &image, &unmapped);  // out is zero when !made
    // RUNTIME_NEEDS 6.3.3: programs treat word 1 == 0 as a null view, so a live image view never has it: set
    // MIN_LOD bit 8 (no program reads it). Only here; descword keeps the plain words.
    if (made && image && out[1] == 0) out[1] = 1u << 8;
    std::memcpy(vdesc_words + u64{record.id} * 8, out, 32);
    InterlockedIncrement(!made ? &vdesc_none : image ? &vdesc_image : &vdesc_buffer_views);
    static LONG shown;
    if (made && first_calls(shown, 4))
        log("x2w-vdesc view %u (slot %u): %s words %08x %08x %08x %08x %08x %08x %08x %08x%s", record.id, record.slot, image ? "T#" : "V#",
            out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], unmapped ? " (format has no gfx10 code: FORMAT 0)" : "");
}

// draw_translated (d3d12_draw.inl), a `heapwords 1` translation, after fill_views; draw_lock held. Creates the
// buffer on first use and fills the views filled so far. False: no buffer (the draw / mesh call is skipped).
bool ensure_vdesc()
{
    static bool failed;
    if (vdesc_words || failed) return vdesc_words != nullptr;
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = u64{view_ids_max} * 32;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = device_of(the_device)->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                       IID_PPV_ARGS(&vdesc_buffer));
    std::uint32_t* words = nullptr;
    if (SUCCEEDED(hr) && FAILED(vdesc_buffer->Map(0, nullptr, reinterpret_cast<void**>(&words)))) words = nullptr;
    if (!words) {
        failed = true;
        log("x2w-vdesc buffer (%llu bytes) -> 0x%08lx: heapwords translations are skipped", static_cast<unsigned long long>(rd.Width),
            static_cast<unsigned long>(hr));
        return false;
    }
    std::memset(words, 0, static_cast<std::size_t>(rd.Width));
    vdesc_words = words;
    for (UINT i = 0; i < views_filled; ++i) {
        AcquireSRWLockShared(&note_lock);
        const ViewRecord record = view_records()[i];
        ReleaseSRWLockShared(&note_lock);
        vdesc_fill(record);
    }
    log("x2w-vdesc buffer %u ids x 32 bytes at desktop 0x%llx; %u views filled so far (T# %ld, V# %ld, none %ld)", view_ids_max,
        static_cast<unsigned long long>(vdesc_buffer->GetGPUVirtualAddress()), views_filled, vdesc_image, vdesc_buffer_views, vdesc_none);
    return true;
}
