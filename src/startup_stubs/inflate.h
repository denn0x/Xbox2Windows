// Experimental (docs/startup-runtime.md): a small zlib (RFC 1950) / deflate (RFC 1951)
// decoder for the DirectStorage stand-in. Written from the RFCs with canonical-code decoding one bit
// at a time; correct but slow, to be replaced by a real decoder if loading time matters.
#pragma once

#include <cstddef>
#include <cstdint>

namespace x2w_inflate {

struct Stream {
    const unsigned char* in;
    std::size_t in_size;
    std::size_t in_pos = 0;
    unsigned char* out;
    std::size_t out_size;
    std::size_t out_pos = 0;
    std::uint32_t bit_buffer = 0;
    int bit_count = 0;
    bool failed = false;
    bool overflow = false;
};

inline std::uint32_t bits(Stream& s, int need) noexcept
{
    std::uint32_t value = s.bit_buffer;
    while (s.bit_count < need) {
        if (s.in_pos == s.in_size) {
            s.failed = true;
            return 0;
        }
        value |= static_cast<std::uint32_t>(s.in[s.in_pos++]) << s.bit_count;
        s.bit_count += 8;
    }
    s.bit_buffer = value >> need;
    s.bit_count -= need;
    return value & ((std::uint32_t{1} << need) - 1);
}

struct Huffman {
    std::uint16_t count[16];
    std::uint16_t symbol[288];
};

// Builds canonical decoding tables from code lengths; returns false for an over-subscribed set.
inline bool construct(Huffman& h, const std::uint8_t* lengths, int n) noexcept
{
    for (auto& c : h.count) c = 0;
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
    return true;
}

inline int decode(Stream& s, const Huffman& h) noexcept
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= static_cast<int>(bits(s, 1));
        if (s.failed) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

inline bool codes(Stream& s, const Huffman& lencode, const Huffman& distcode) noexcept
{
    static const std::uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const std::uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const std::uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                                769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const std::uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (s.out_pos == s.out_size) {
                s.overflow = true;
                return false;
            }
            s.out[s.out_pos++] = static_cast<unsigned char>(symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            const std::size_t length = length_base[symbol] + bits(s, length_extra[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30) return false;
            const std::size_t distance = dist_base[symbol] + bits(s, dist_extra[symbol]);
            if (s.failed || distance > s.out_pos) return false;
            if (length > s.out_size - s.out_pos) {
                s.overflow = true;
                return false;
            }
            for (std::size_t i = 0; i < length; ++i, ++s.out_pos) s.out[s.out_pos] = s.out[s.out_pos - distance];
        }
    }
}

inline bool stored(Stream& s) noexcept
{
    s.bit_buffer = 0;
    s.bit_count = 0;
    if (s.in_size - s.in_pos < 4) return false;
    const unsigned length = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
    const unsigned check = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
    s.in_pos += 4;
    if ((length ^ check) != 0xFFFF || length > s.in_size - s.in_pos) return false;
    if (length > s.out_size - s.out_pos) {
        s.overflow = true;
        return false;
    }
    for (unsigned i = 0; i < length; ++i) s.out[s.out_pos++] = s.in[s.in_pos++];
    return true;
}

struct FixedTables {
    Huffman lencode;
    Huffman distcode;
};

inline FixedTables make_fixed_tables() noexcept
{
    FixedTables tables{};
    std::uint8_t lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    construct(tables.lencode, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    construct(tables.distcode, lengths, 30);
    return tables;
}

inline bool fixed_block(Stream& s) noexcept
{
    static const FixedTables tables = make_fixed_tables();
    return codes(s, tables.lencode, tables.distcode);
}

inline bool dynamic_block(Stream& s) noexcept
{
    static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = static_cast<int>(bits(s, 5)) + 257;
    const int ndist = static_cast<int>(bits(s, 5)) + 1;
    const int ncode = static_cast<int>(bits(s, 4)) + 4;
    if (s.failed || nlen > 286 || ndist > 30) return false;
    std::uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<std::uint8_t>(bits(s, 3));
    Huffman lencode{}, distcode{};
    if (s.failed || !construct(lencode, lengths, 19)) return false;
    for (auto& l : lengths) l = 0;
    int index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t value = 0;
        int repeat;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            repeat = 3 + static_cast<int>(bits(s, 2));
        } else if (symbol == 17) {
            repeat = 3 + static_cast<int>(bits(s, 3));
        } else {
            repeat = 11 + static_cast<int>(bits(s, 7));
        }
        if (s.failed || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;
    // Incomplete distance or literal sets with a single code are legal; over-subscribed ones are not.
    if (!construct(lencode, lengths, nlen) || !construct(distcode, lengths + nlen, ndist)) return false;
    return codes(s, lencode, distcode);
}

// Raw deflate. Returns true when the final block ended; *produced gets the output size.
inline bool inflate_raw(Stream& s) noexcept
{
    for (;;) {
        const bool last = bits(s, 1) != 0;
        const std::uint32_t type = bits(s, 2);
        if (s.failed) return false;
        const bool ok = type == 0 ? stored(s) : type == 1 ? fixed_block(s) : type == 2 ? dynamic_block(s) : false;
        if (!ok || s.failed) return false;
        if (last) return true;
    }
}

inline std::uint32_t adler32(const unsigned char* data, std::size_t size) noexcept
{
    std::uint32_t a = 1, b = 0;
    while (size) {
        std::size_t chunk = size < 5552 ? size : 5552;
        size -= chunk;
        while (chunk--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

enum class InflateStatus {
    ok,
    insufficient_output,
    corrupt_stream,
    bad_checksum,
};

// zlib stream (2-byte header, deflate data, Adler-32). Distinguishes output capacity overflow
// and Adler-32 mismatch from corrupt headers/deflate data.
inline InflateStatus inflate_zlib_ex(const std::uint8_t* input, std::size_t input_size, std::uint8_t* output,
                                     std::size_t capacity, std::size_t& out_size) noexcept
{
    out_size = 0;
    if (!input || (!output && capacity > 0) || input_size < 6 || (input[0] & 0x0F) != 8 ||
        ((input[0] << 8) | input[1]) % 31 != 0 || (input[1] & 0x20)) {
        return InflateStatus::corrupt_stream;
    }
    Stream s{input + 2, input_size - 2, 0, output, capacity};
    if (!inflate_raw(s)) {
        return s.overflow ? InflateStatus::insufficient_output : InflateStatus::corrupt_stream;
    }
    out_size = s.out_pos;
    if (s.in_size - s.in_pos < 4) return InflateStatus::corrupt_stream;
    const unsigned char* tail = s.in + s.in_pos;
    const std::uint32_t expected = (std::uint32_t{tail[0]} << 24) | (std::uint32_t{tail[1]} << 16) |
                                   (std::uint32_t{tail[2]} << 8) | tail[3];
    return adler32(output, s.out_pos) == expected ? InflateStatus::ok : InflateStatus::bad_checksum;
}

// zlib stream (2-byte header, deflate data, Adler-32). The checksum must match.
inline bool inflate_zlib(const unsigned char* in, std::size_t in_size, unsigned char* out, std::size_t out_size,
                         std::size_t* produced) noexcept
{
    std::size_t out_size_tmp = 0;
    const InflateStatus status = inflate_zlib_ex(in, in_size, out, out_size, out_size_tmp);
    if (produced) *produced = out_size_tmp;
    return status == InflateStatus::ok;
}

}  // namespace x2w_inflate
