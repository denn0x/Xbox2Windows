// LOCAL (video): command-list slot 15, CopyBufferRegion(dest, dest offset, source, source offset, bytes).
// Included inside d3d12_xs_device.cpp's anonymous namespace, before the slot tables are filled.
//
// The slot is a no-op otherwise (x2w-noop "CommandList::slot15" in run 77). The title fills some
// constant buffers only through it: from an upload buffer into a placed buffer whose own memory the
// CPU never writes. Translated draws bind a record-time copy of the buffer's title memory, so they
// read whatever that memory held before. Seen in run 77: pipeline 342e9584749d47da (the last pass
// before the present, an upscale with dither) bound 208 bytes at 0xd4940fc80 that hold CPU data
// (a hash constant, an executable address, a stack address), not shader constants.
//
// X2W_D3D12_COPY_BUFFER: unset or 0 = the old no-op; "log" = no-op, but the first copies are
// logged with their title addresses; 1 = copy title memory to title memory at record time (the
// copy completes before later draws in the same frame take their record-time copies), and log.
// LOCAL SHORTCUT: the copy happens when the title records it, not when the GPU would run it; a
// copy from a buffer that GPU work fills in the same submission copies the older contents, and
// a draw that already took its frame copy of the destination this frame keeps that copy.

LONG buffer_copies_done, buffer_copies_skipped;

int copy_buffer_mode()
{
    static const int mode = [] {
        char value[8]{};
        const DWORD saved = GetLastError();
        const DWORD length = GetEnvironmentVariableA("X2W_D3D12_COPY_BUFFER", value, sizeof(value));
        SetLastError(saved);
        if (!length || value[0] == '0') return 0;
        return value[0] == '1' ? 2 : (value[0] == 'l' || value[0] == 'L') ? 1 : (value[0] == 'g' || value[0] == 'G') ? 3 : 0;  // 3: LOCAL draw-rt
    }();
    return mode;
}

bool copy_title_bytes(unsigned char* dest, const unsigned char* source, std::size_t bytes) noexcept
{
    __try {
        std::memmove(dest, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __stdcall list_copy_buffer(Wrapper* self, Wrapper* dest, u64 dest_offset, Wrapper* source, u64 source_offset, u64 bytes)
{
    (void)self;
    const int mode = copy_buffer_mode();
    auto* const to = dest && dest->own().kind == resource_kind ? static_cast<unsigned char*>(dest->own().title_memory) : nullptr;
    auto* const from = source && source->own().kind == resource_kind ? static_cast<const unsigned char*>(source->own().title_memory) : nullptr;
    const char* outcome = "copied";
    bool done = false;
    if (!to || !from) {
        outcome = !to ? "skipped: destination has no title memory" : "skipped: source has no title memory";
    } else if (!bytes || bytes > (u64{256} << 20) || dest_offset > (u64{1} << 40) || source_offset > (u64{1} << 40)) {
        outcome = "skipped: size or offset out of range";
    } else if (!x2w_local::readable(from + source_offset, static_cast<std::size_t>(bytes))) {
        outcome = "skipped: source unreadable";
    } else if (mode < 2) {
        outcome = "not copied (X2W_D3D12_COPY_BUFFER=log)";
    } else if (!copy_title_bytes(to + dest_offset, from + source_offset, static_cast<std::size_t>(bytes))) {
        outcome = "skipped: destination not writable";
    } else {
        done = true;
    }
    InterlockedIncrement(done ? &buffer_copies_done : &buffer_copies_skipped);
    copy_watch_note("record-time", to ? reinterpret_cast<u64>(to) + dest_offset : 0, from ? reinterpret_cast<u64>(from) + source_offset : 0,
                    bytes, outcome);
    // The first copy to each destination title address; copies of up to 0x100 bytes (constant
    // blocks) show the whole source, longer ones its first 16 bytes. Small and long copies have
    // separate budgets of 256 lines each: in run 84, world-streaming copies of 0x404 and 0x1000 bytes
    // spent a single budget within 37 s of setup.
    static SRWLOCK seen_lock = SRWLOCK_INIT;
    static std::unordered_set<u64>* seen = new std::unordered_set<u64>;
    const u64 where = to ? reinterpret_cast<u64>(to) + dest_offset : reinterpret_cast<u64>(dest) ^ (dest_offset << 1);
    AcquireSRWLockExclusive(&seen_lock);
    const bool fresh = seen->size() < 4096 && seen->insert(where).second;
    ReleaseSRWLockExclusive(&seen_lock);
    static LONG shown_small, shown_long;
    if (fresh && first_calls(bytes && bytes <= 0x100 ? shown_small : shown_long, 256)) {
        const std::size_t dumped = bytes && bytes <= 0x100 ? static_cast<std::size_t>(bytes) : 16;
        char head[3 * 0x100 + 1] = "";
        if (from && x2w_local::readable(from + source_offset, dumped)) hex_text(from + source_offset, dumped, head, sizeof(head));
        log("x2w-copy CopyBufferRegion dest %p+0x%llx (title %p) source %p+0x%llx (title %p) bytes 0x%llx: %s; source [%s]",
            static_cast<void*>(dest), dest_offset, to ? static_cast<void*>(to + dest_offset) : nullptr, static_cast<void*>(source),
            source_offset, from ? static_cast<const void*>(from + source_offset) : nullptr, bytes, outcome, head);
    }
    static LONG reports;
    const LONG total = buffer_copies_done + buffer_copies_skipped;
    if (total % 1000 == 0 && first_calls(reports, 40))
        log("x2w-copy CopyBufferRegion totals: copied %ld, skipped %ld", buffer_copies_done, buffer_copies_skipped);
}

// Installs slot 15 when the switch asks for it; one line says which mode is in effect.
void install_copy_buffer(ListTyped* typed)
{
    const int mode = copy_buffer_mode();
    if (!mode) return;
    typed[15] = mode == 3 ? reinterpret_cast<ListTyped>(&list_copy_buffer_gpu) : reinterpret_cast<ListTyped>(&list_copy_buffer);  // LOCAL draw-rt
    log("x2w-copy X2W_D3D12_COPY_BUFFER=%s: command-list slot 15 (CopyBufferRegion) %s", mode == 3 ? "gpu" : mode == 2 ? "1" : "log",
        mode == 3 ? "runs on the GPU in list order (record-time copy as fallback)" : mode == 2 ? "copies title memory at record time" : "is logged only");
}
