// Source-owned outer control around the translated frame-plane and heap fixtures.
#include <algorithm>
#include <cstddef>
#include <cstdint>

using Word = std::uint64_t;
extern "C" Word group_init(Word, std::uint32_t);
extern "C" Word append_frame(Word, std::uint32_t, const std::uint32_t*, Word);
extern "C" Word heap_group(Word, Word);

struct Plane {
    Word destination;
    std::uint32_t enabled, pitch;
};
struct Frames {
    std::uint32_t count, width, height, small_width, small_height, reserved;
    Plane planes[3][5];
};
struct Context {
    char before_account[0xe0];
    Word accounted;
    char before_format[0x118 - 0xe8];
    std::uint32_t format;
    char before_owner[0x6a8 - 0x11c];
    Word owner;
};
static_assert(offsetof(Context, owner) == 0x6a8 && sizeof(Frames) == 264, "Context and Frames layout");

extern "C" __declspec(dllexport) std::uint32_t frame_allocate(Context* context, Frames* frames,
                                                              std::uint32_t alignment)
{
    if (context->owner) return 0;
    Word storage[8 + 2 * 32]{};
    const Word group = reinterpret_cast<Word>(storage);
    group_init(group, 32);
    std::uint32_t pitches[2]{frames->width, (frames->small_width + 15) & ~15u};
    // The synthetic comparison exercises one aligned format and one ordinary format.
    if (context->format == 0x6632424b) {
        alignment = (std::max)(64u, alignment);
        pitches[0] = (frames->width + alignment - 1) & ~(alignment - 1);
        pitches[1] = (frames->small_width + alignment - 1) & ~(alignment - 1);
    }
    if (frames->count == 3) frames->count = 2;
    for (unsigned i = 0; i < frames->count; ++i)
        append_frame(reinterpret_cast<Word>(frames), i, pitches, group);
    for (unsigned i = frames->count; i < 3; ++i)
        for (unsigned p = 0; p < 3; ++p) frames->planes[i][p].destination = 0;
    if (!static_cast<std::uint32_t>(storage[5])) return 1;
    context->accounted += sizeof(Frames) + storage[4];
    context->owner = heap_group(group, sizeof(Frames));
    return context->owner != 0;
}
