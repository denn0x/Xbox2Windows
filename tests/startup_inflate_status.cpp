#include "startup_stubs/inflate.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using x2w_inflate::InflateStatus;
using x2w_inflate::inflate_zlib;
using x2w_inflate::inflate_zlib_ex;

bool check_ok(const char* name, const std::uint8_t* stream, std::size_t stream_size,
              const std::uint8_t* expected, std::size_t expected_size)
{
    std::vector<std::uint8_t> out(expected_size, 0xAA);
    std::size_t out_size = 999;
    const InflateStatus status = inflate_zlib_ex(stream, stream_size, out.data(), out.size(), out_size);
    if (status != InflateStatus::ok || out_size != expected_size ||
        std::memcmp(out.data(), expected, expected_size) != 0) {
        std::fprintf(stderr, "%s: expected ok/%zu, got status=%d out_size=%zu\n", name, expected_size,
                     static_cast<int>(status), out_size);
        return false;
    }
    std::vector<std::uint8_t> compat_out(expected_size, 0x55);
    std::size_t compat_size = 0;
    if (!inflate_zlib(stream, stream_size, compat_out.data(), compat_out.size(), &compat_size) ||
        compat_size != expected_size || std::memcmp(compat_out.data(), expected, expected_size) != 0) {
        std::fprintf(stderr, "%s: inflate_zlib wrapper mismatch\n", name);
        return false;
    }
    return true;
}

bool check_insufficient(const char* name, const std::uint8_t* stream, std::size_t stream_size, std::size_t capacity)
{
    std::vector<std::uint8_t> out(capacity + 1, 0xCC);
    std::size_t out_size = 999;
    const InflateStatus status = inflate_zlib_ex(stream, stream_size, out.data(), capacity, out_size);
    if (status != InflateStatus::insufficient_output || status == InflateStatus::corrupt_stream ||
        status == InflateStatus::bad_checksum) {
        std::fprintf(stderr, "%s: expected insufficient_output, got status=%d\n", name, static_cast<int>(status));
        return false;
    }
    if (out[capacity] != 0xCC) {
        std::fprintf(stderr, "%s: wrote past capacity=%zu\n", name, capacity);
        return false;
    }
    std::size_t compat_size = 999;
    if (inflate_zlib(stream, stream_size, out.data(), capacity, &compat_size) || compat_size != 0) {
        std::fprintf(stderr, "%s: inflate_zlib wrapper should return false with produced=0\n", name);
        return false;
    }
    return true;
}

bool check_status(const char* name, const std::uint8_t* stream, std::size_t stream_size, std::size_t capacity,
                  InflateStatus expected_status)
{
    std::vector<std::uint8_t> out(capacity, 0xDD);
    std::size_t out_size = 999;
    const InflateStatus status = inflate_zlib_ex(stream, stream_size, out.data(), capacity, out_size);
    if (status != expected_status) {
        std::fprintf(stderr, "%s: expected status=%d, got status=%d\n", name, static_cast<int>(expected_status),
                     static_cast<int>(status));
        return false;
    }
    std::size_t compat_size = 999;
    if (inflate_zlib(stream, stream_size, out.data(), capacity, &compat_size)) {
        std::fprintf(stderr, "%s: inflate_zlib wrapper unexpectedly succeeded\n", name);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> dynamic_payload()
{
    std::uint32_t x = 0x12345678u;
    auto next_u8 = [&x](std::uint32_t mod) -> std::uint8_t {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return static_cast<std::uint8_t>((x >> 16) % mod);
    };
    std::vector<std::uint8_t> data;
    data.reserve(600);
    for (int i = 0; i < 300; ++i) data.push_back(next_u8(256));
    for (int i = 0; i < 300; ++i) data.push_back(next_u8(32));
    return data;
}

bool check_fixed_multithreaded(const std::uint8_t* stream, std::size_t stream_size, const std::uint8_t* expected,
                               std::size_t expected_size)
{
    constexpr int thread_count = 8;
    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};
    std::array<std::uint8_t, thread_count> thread_ok{};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&, t]() {
            std::vector<std::uint8_t> out(expected_size, 0xAA);
            std::size_t out_size = 999;
            ready_count.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const InflateStatus status = inflate_zlib_ex(stream, stream_size, out.data(), out.size(), out_size);
            thread_ok[static_cast<std::size_t>(t)] =
                (status == InflateStatus::ok && out_size == expected_size &&
                 std::memcmp(out.data(), expected, expected_size) == 0)
                    ? 1
                    : 0;
        });
    }

    while (ready_count.load(std::memory_order_acquire) < thread_count) std::this_thread::yield();
    go.store(true, std::memory_order_release);

    for (auto& w : workers) w.join();

    for (int t = 0; t < thread_count; ++t) {
        if (!thread_ok[static_cast<std::size_t>(t)]) {
            std::fprintf(stderr, "fixed_lit_multithreaded: thread %d failed\n", t);
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    // 1. Stored block: "Hello, stored!" (14 bytes)
    static const std::uint8_t stored_stream[] = {
        0x78, 0x01, 0x01, 0x0e, 0x00, 0xf1, 0xff, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c,
        0x20, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x21, 0x25, 0x9d, 0x04, 0xf3,
    };
    static const std::uint8_t stored_expected[] = {'H', 'e', 'l', 'l', 'o', ',', ' ',
                                                   's', 't', 'o', 'r', 'e', 'd', '!'};

    // 2. Fixed Huffman literals only: "abcdefghij" (10 bytes)
    static const std::uint8_t fixed_lit_stream[] = {
        0x78, 0x9c, 0x4b, 0x4c, 0x4a, 0x4e, 0x49, 0x4d, 0x4b, 0xcf, 0xc8, 0xcc, 0x02, 0x00, 0x15, 0x86, 0x03, 0xf8,
    };
    static const std::uint8_t fixed_lit_expected[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};

    // 3. Fixed Huffman with LZ77 back-reference match-copy: "ABCD" * 5 (20 bytes)
    static const std::uint8_t fixed_match_stream[] = {
        0x78, 0x9c, 0x73, 0x74, 0x72, 0x76, 0x71, 0x44, 0xc3, 0x00, 0x36, 0x88, 0x05, 0x33,
    };
    static const std::uint8_t fixed_match_expected[] = {
        'A', 'B', 'C', 'D', 'A', 'B', 'C', 'D', 'A', 'B', 'C', 'D', 'A', 'B', 'C', 'D', 'A', 'B', 'C', 'D',
    };

    // 4. Dynamic Huffman (BTYPE=2) with literals + matches (600 bytes)
    static const std::uint8_t dynamic_stream[] = {
        0x78, 0x9c, 0x05, 0xc1, 0x59, 0x48, 0x14, 0x61, 0x00, 0x00, 0xe0, 0x99, 0xf9, 0x67, 0x67, 0xe7, 0x9f, 0x7b,
        0xfe, 0xd9, 0x39, 0xff, 0x9d, 0x19, 0xf5, 0xc1, 0xf2, 0xc1, 0x15, 0xa9, 0x87, 0x50, 0x84, 0x12, 0x12, 0x2a,
        0x43, 0x45, 0x8d, 0xca, 0xd2, 0x24, 0xa4, 0x03, 0x31, 0x37, 0xa8, 0x14, 0x3a, 0xd0, 0x68, 0x13, 0x71, 0x0b,
        0xc3, 0x0d, 0x59, 0x31, 0xc5, 0x88, 0x2c, 0xec, 0x61, 0xe9, 0xd2, 0x2e, 0xc9, 0xa2, 0x40, 0x93, 0xa2, 0x12,
        0xca, 0x30, 0x03, 0x45, 0x02, 0xb3, 0xa8, 0x07, 0xa1, 0xcb, 0xfa, 0xbe, 0xee, 0xaa, 0xb4, 0x9b, 0x79, 0x83,
        0xe3, 0x56, 0x53, 0x6a, 0x54, 0x7e, 0x54, 0xf7, 0x6c, 0xba, 0x23, 0x39, 0xf6, 0xb4, 0x62, 0xee, 0xf7, 0x96,
        0xc4, 0x70, 0x84, 0x3e, 0xb7, 0xf4, 0x1a, 0xbc, 0xfd, 0xd4, 0x7a, 0xb4, 0xbb, 0xb0, 0x72, 0x4f, 0xdd, 0x97,
        0xfa, 0xdc, 0x87, 0x77, 0x9b, 0x27, 0x5e, 0x95, 0xaf, 0x38, 0xbd, 0xbc, 0x25, 0x64, 0xa1, 0x8e, 0xae, 0x44,
        0xbc, 0x43, 0x59, 0xe9, 0x9d, 0x4c, 0x3f, 0x1d, 0x49, 0x0e, 0x6c, 0x6a, 0x8c, 0xe3, 0x8c, 0xd8, 0xa1, 0xf9,
        0xe4, 0x42, 0x97, 0x5a, 0x53, 0x50, 0x50, 0x54, 0xd8, 0x5c, 0x3d, 0xd6, 0x59, 0x1f, 0x5a, 0x77, 0xc9, 0x58,
        0xdc, 0xd9, 0xc8, 0xcd, 0xb6, 0x5d, 0x8e, 0x46, 0xc6, 0x67, 0xef, 0xb4, 0x5f, 0xcf, 0x29, 0x5e, 0x6c, 0x2f,
        0x3f, 0x58, 0xda, 0xb6, 0x18, 0x9b, 0xe8, 0xad, 0xad, 0xc8, 0xfe, 0x43, 0x17, 0xb4, 0xef, 0xd8, 0xb6, 0x94,
        0x16, 0xdd, 0xee, 0xf6, 0x35, 0xdd, 0xfa, 0x71, 0xf8, 0xeb, 0x89, 0x51, 0x73, 0xff, 0x42, 0xf5, 0x15, 0x60,
        0x70, 0xd1, 0x37, 0x5b, 0x07, 0xfd, 0x0b, 0x33, 0x19, 0x5e, 0x69, 0xff, 0xc0, 0xa9, 0xe5, 0x93, 0xd7, 0xb2,
        0xbf, 0x9d, 0x99, 0x2c, 0x23, 0x72, 0x53, 0xcb, 0x0b, 0x71, 0xb5, 0x27, 0xbf, 0x65, 0x6a, 0xe6, 0x6f, 0xce,
        0x78, 0xe7, 0xcb, 0x7d, 0xb3, 0x53, 0xb1, 0xdb, 0xd5, 0x59, 0x1f, 0x22, 0xa9, 0xcf, 0xc1, 0x86, 0xb2, 0xcc,
        0x44, 0x51, 0xc3, 0x8d, 0xf4, 0x5f, 0x13, 0xf9, 0xff, 0x76, 0xbd, 0x58, 0x4d, 0xb7, 0xe4, 0x7d, 0xbf, 0xd8,
        0x37, 0x42, 0xac, 0xf1, 0xdf, 0xf5, 0x93, 0x3f, 0x8b, 0x9f, 0xaf, 0xad, 0x39, 0x8f, 0x9f, 0xf4, 0xcc, 0xc7,
        0x36, 0x9b, 0x8f, 0x87, 0xf2, 0xa7, 0x93, 0xe2, 0xbd, 0xd6, 0xb9, 0x92, 0xb3, 0x95, 0x9d, 0x23, 0xef, 0xe3,
        0x8d, 0x99, 0xe2, 0x4c, 0x74, 0xe3, 0xd0, 0xfa, 0xf9, 0x23, 0xbb, 0x0f, 0xdc, 0xcf, 0x8e, 0x36, 0x0c, 0x84,
        0x3f, 0x6a, 0xc5, 0xe2, 0xb1, 0x07, 0xab, 0x86, 0xab, 0xc8, 0xda, 0xba, 0xbd, 0x25, 0x99, 0xc2, 0xd5, 0xe3,
        0x09, 0x71, 0x83, 0x4a, 0x03, 0x27, 0xa4, 0xb2, 0x98, 0x91, 0x14, 0x46, 0xd4, 0x78, 0x9b, 0x17, 0xa0, 0xc1,
        0x4b, 0x96, 0xa8, 0x39, 0x01, 0xc9, 0x53, 0x5c, 0x57, 0xf3, 0x29, 0xdf, 0xb1, 0x0c, 0xc8, 0x5b, 0x34, 0x70,
        0x81, 0x4c, 0x41, 0xe0, 0xd8, 0xa6, 0x42, 0x43, 0x59, 0xe5, 0xc4, 0xa0, 0x83, 0xc3, 0xa6, 0xad, 0x31, 0x2e,
        0xf0, 0x20, 0x19, 0x74, 0x49, 0x20, 0xf1, 0x0e, 0xf4, 0x6c, 0x91, 0x50, 0x11, 0x21, 0x08, 0x04, 0x03, 0x7c,
        0x6c, 0xd1, 0x90, 0xe3, 0x6c, 0x43, 0x61, 0x15, 0x48, 0xb0, 0x61, 0x85, 0x32, 0x91, 0x0b, 0x18, 0xd6, 0xf3,
        0x15, 0x9e, 0x60, 0x42, 0x2e, 0x8b, 0x25, 0x86, 0xa5, 0x34, 0x43, 0xa1, 0xe5, 0xa0, 0xc3, 0xbb, 0x84, 0xed,
        0x59, 0x41, 0x45, 0xc0, 0x2a, 0x94, 0x48, 0x46, 0x17, 0x80, 0xea, 0x03, 0x99, 0x44, 0x80, 0x66, 0xa0, 0x8d,
        0x11, 0x1f, 0x60, 0x38, 0x0a, 0xdb, 0x1c, 0xa5, 0xa9, 0xa6, 0xee, 0xf9, 0x3a, 0x11, 0xb6, 0x15, 0x33, 0xa0,
        0x68, 0xa4, 0x48, 0x51, 0x4e, 0xd0, 0x62, 0x05, 0x42, 0xb2, 0x25, 0x3b, 0x64, 0x22, 0x96, 0x42, 0x28, 0xa4,
        0x63, 0xd1, 0xc5, 0x72, 0x40, 0x63, 0x45, 0x5a, 0x34, 0x81, 0x29, 0x5b, 0x80, 0x84, 0x14, 0xd2, 0x64, 0xec,
        0x7b, 0x3a, 0x44, 0xc8, 0xf0, 0x14, 0x03, 0x79, 0x96, 0x28, 0xcb, 0x3a, 0x16, 0x4c, 0xdb, 0x45, 0x94, 0x6e,
        0x22, 0xce, 0x61, 0x05, 0x13, 0x07, 0x0c, 0xd5, 0x16, 0x5d, 0x08, 0x69, 0x95, 0x0f, 0xf3, 0x2c, 0x70, 0x61,
        0x98, 0x50, 0xb0, 0xce, 0x91, 0x1a, 0x45, 0xfd, 0x07, 0x3f, 0x85, 0xa3, 0x6c,
    };
    const std::vector<std::uint8_t> dyn_expected = dynamic_payload();

    bool ok = true;

    // Multi-threaded fixed Huffman stress test (run before single-threaded fixed_block calls)
    ok = ok && check_fixed_multithreaded(fixed_lit_stream, sizeof(fixed_lit_stream), fixed_lit_expected,
                                         sizeof(fixed_lit_expected));

    // Exact-size buffer -> ok
    ok = ok && check_ok("stored", stored_stream, sizeof(stored_stream), stored_expected, sizeof(stored_expected));
    ok = ok && check_ok("fixed_lit", fixed_lit_stream, sizeof(fixed_lit_stream), fixed_lit_expected,
                        sizeof(fixed_lit_expected));
    ok = ok && check_ok("fixed_match", fixed_match_stream, sizeof(fixed_match_stream), fixed_match_expected,
                        sizeof(fixed_match_expected));
    ok = ok && check_ok("dynamic", dynamic_stream, sizeof(dynamic_stream), dyn_expected.data(), dyn_expected.size());

    // capacity == exact - 1 across stored, literal emit, match-copy, and dynamic -> insufficient_output
    ok = ok && check_insufficient("stored_short", stored_stream, sizeof(stored_stream), sizeof(stored_expected) - 1);
    ok = ok && check_insufficient("fixed_lit_short", fixed_lit_stream, sizeof(fixed_lit_stream),
                                  sizeof(fixed_lit_expected) - 1);
    ok = ok && check_insufficient("fixed_match_short", fixed_match_stream, sizeof(fixed_match_stream),
                                  sizeof(fixed_match_expected) - 1);
    ok = ok && check_insufficient("dynamic_short", dynamic_stream, sizeof(dynamic_stream), dyn_expected.size() - 1);

    // Corrupted Adler-32 trailer -> bad_checksum
    std::vector<std::uint8_t> bad_adler(fixed_match_stream, fixed_match_stream + sizeof(fixed_match_stream));
    bad_adler.back() ^= 0x01;
    ok = ok && check_status("bad_adler32", bad_adler.data(), bad_adler.size(), sizeof(fixed_match_expected),
                            InflateStatus::bad_checksum);

    // Invalid zlib headers -> corrupt_stream
    static const std::uint8_t bad_cmf[] = {0x79, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const std::uint8_t bad_fcheck[] = {0x78, 0x9d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const std::uint8_t fdict_set[] = {0x78, 0xbb, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const std::uint8_t too_short[] = {0x78, 0x9c, 0x03, 0x00, 0x00};
    ok = ok && check_status("bad_cmf", bad_cmf, sizeof(bad_cmf), 16, InflateStatus::corrupt_stream);
    ok = ok && check_status("bad_fcheck", bad_fcheck, sizeof(bad_fcheck), 16, InflateStatus::corrupt_stream);
    ok = ok && check_status("fdict_set", fdict_set, sizeof(fdict_set), 16, InflateStatus::corrupt_stream);
    ok = ok && check_status("too_short", too_short, sizeof(too_short), 16, InflateStatus::corrupt_stream);

    // Truncated stream (missing trailer and truncated deflate) -> corrupt_stream
    ok = ok && check_status("truncated_trailer", fixed_lit_stream, sizeof(fixed_lit_stream) - 1,
                            sizeof(fixed_lit_expected), InflateStatus::corrupt_stream);
    ok = ok && check_status("truncated_deflate", fixed_lit_stream, sizeof(fixed_lit_stream) - 6,
                            sizeof(fixed_lit_expected), InflateStatus::corrupt_stream);

    // Invalid block type (BTYPE=11: bits 1, 1, 1 -> 0x07) -> corrupt_stream
    static const std::uint8_t bad_btype[] = {0x78, 0x9c, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01};
    ok = ok && check_status("bad_btype", bad_btype, sizeof(bad_btype), 16, InflateStatus::corrupt_stream);

    // Invalid dynamic Huffman header (hlit=31 -> nlen=288 > 286) -> corrupt_stream
    // bits: bfinal=1(1), btype=10(2), hlit=31(5) -> 0b11111_10_1 = 0xFD
    static const std::uint8_t bad_dyn_huffman[] = {0x78, 0x9c, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x01};
    ok = ok && check_status("bad_dyn_huffman", bad_dyn_huffman, sizeof(bad_dyn_huffman), 16,
                            InflateStatus::corrupt_stream);

    // Out-of-window back-reference in fixed Huffman (bfinal=1, btype=01, len=257(3), dist=0(1), eob=256)
    // Deflate Huffman codes are MSB-first: 257 = 0000001, dist 0 = 00000, 256 = 0000000 -> 0x03, 0x02, 0x00
    static const std::uint8_t bad_backref[] = {0x78, 0x9c, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    ok = ok && check_status("bad_backref_roomy", bad_backref, sizeof(bad_backref), 16, InflateStatus::corrupt_stream);
    ok = ok && check_status("bad_backref_zero_cap", bad_backref, sizeof(bad_backref), 0, InflateStatus::corrupt_stream);

    return ok ? 0 : 1;
}
