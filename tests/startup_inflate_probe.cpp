// Input for tests/startup_inflate_check.py: runs the DirectStorage stand-in's zlib decoder
// (src/startup_stubs/inflate.h) on one stream file and writes the decoded bytes.
//
// usage: startup_inflate_probe zlib|raw <stream> <capacity> <output>
// "zlib" runs inflate_zlib on a zlib stream; "raw" runs inflate_raw on bare deflate data, so a
// rejection cannot come from the zlib header or the Adler-32 check. Prints "inflated <size>" and
// writes the output file when the decoder accepts the stream. Otherwise it prints "rejected", and in
// raw mode the number of bytes decoded before the rejection. 16 guard bytes follow the destination;
// a write past the capacity is reported whether or not the decoder accepted the stream.
#include "startup_stubs/inflate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    const bool raw = argc == 5 && std::strcmp(argv[1], "raw") == 0;
    if (argc != 5 || (!raw && std::strcmp(argv[1], "zlib") != 0)) {
        std::fprintf(stderr, "usage: startup_inflate_probe zlib|raw <stream> <capacity> <output>\n");
        return 2;
    }
    std::ifstream file(argv[2], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    const std::vector<unsigned char> stream{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const std::size_t capacity = std::strtoull(argv[3], nullptr, 10);
    constexpr std::size_t guard = 16;
    std::vector<unsigned char> destination(capacity + guard, 0xA5);
    std::size_t produced = 0;
    bool ok;
    if (raw) {
        x2w_inflate::Stream s{stream.data(), stream.size()};
        s.out = destination.data();
        s.out_size = capacity;
        ok = x2w_inflate::inflate_raw(s);
        produced = s.out_pos;
    } else {
        ok = x2w_inflate::inflate_zlib(stream.data(), stream.size(), destination.data(), capacity, &produced);
    }
    for (std::size_t i = capacity; i < destination.size(); ++i) {
        if (destination[i] != 0xA5) {
            std::printf("wrote past the destination at +%zu\n", i - capacity);
            return 3;
        }
    }
    if (!ok) {
        if (raw)
            std::printf("rejected after %zu bytes\n", produced);
        else
            std::printf("rejected\n");
        return 1;
    }
    std::ofstream out(argv[4], std::ios::binary);
    out.write(reinterpret_cast<const char*>(destination.data()), static_cast<std::streamsize>(produced));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[4]);
        return 2;
    }
    std::printf("inflated %zu\n", produced);
    return 0;
}
