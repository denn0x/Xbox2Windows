// BC6H BCPack rebuild (src/startup_stubs/bcpack_bc6h.h) on synthetic lane symbols (no title data).
// 1. Exact path: 48 blocks over all 14 modes whose references stay inside the stream; expected bytes
//    from an independent Python model (bcpack_bc6h_vectors.inl).
// 2. Fallback path: the same stream with block 0 pointed at the dictionary; every block whose
//    endpoint chain starts there must be counted and must still be a legal block (mode, anchors).
// 3. Negative control: a mode code of 14 fails the stream.
// Exit code 0 = pass.
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/startup_stubs/bcpack_bc6h.h"

namespace {
#include "bcpack_bc6h_vectors.inl"

using namespace x2w_bcpack;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "pass" : "FAIL", what);
    failures += ok ? 0 : 1;
}

// Mode index of a packed block, or -1.
int mode_of(const uint8_t* b) {
    const uint32_t m2 = b[0] & 3u;
    if (m2 < 2) return static_cast<int>(m2);
    for (int k = 2; k < 14; ++k)
        if ((b[0] & 31u) == bc6h::MODES[k].code) return k;
    return -1;
}

}  // namespace

int main() {
    const size_t nb = BC6H_VEC_BLOCKS;
    std::vector<uint16_t> sym(BC6H_VEC_SYMBOLS, BC6H_VEC_SYMBOLS + 16 * 2 * nb);
    std::vector<uint8_t> out(16 * nb);
    bc6h::Stats st;
    bool ok = bc6h::decode_blocks(sym, nb, out.data(), out.size(), &st);
    check(ok, "exact stream decodes");
    check(st.exact == nb && st.endpoints_only == 0 && st.copied == 0 && st.constant == 0, "all blocks exact");
    size_t first = nb;
    for (size_t i = 0; i < nb && first == nb; ++i)
        if (std::memcmp(out.data() + 16 * i, BC6H_VEC_BLOCKS_OUT + 16 * i, 16) != 0) first = i;
    if (first != nb) std::printf("  first mismatch at block %zu\n", first);
    check(first == nb, "blocks match the independent model byte for byte");

    // Block 0: endpoint reference 1 and index reference 1 (> 0 = before the stream start: slot 0).
    std::vector<uint16_t> fb = sym;
    const size_t t = 2 * nb;
    fb[0 * t + 0] = 0; fb[0 * t + 1] = 0;  // d = 0 -> er = x
    fb[1 * t + 0] = 1; fb[1 * t + 1] = 0;  // x = 1
    ok = bc6h::decode_blocks(fb, nb, out.data(), out.size(), &st);
    check(ok, "dictionary-rooted stream decodes with fallback");
    std::printf("  exact %zu endpoints_only %zu copied %zu constant %zu\n", st.exact, st.endpoints_only, st.copied,
                st.constant);
    check(st.exact + st.endpoints_only + st.copied + st.constant == nb && st.exact < nb && st.copied + st.endpoints_only > 0,
          "fallback classes counted");
    bool legal = true;
    for (size_t i = 0; i < nb; ++i) legal &= mode_of(out.data() + 16 * i) >= 0;
    check(legal, "fallback blocks carry valid modes");

    // With a dictionary entry for slot 0 that equals the half-range start, block 0 behaves as ref 0.
    static uint16_t dep[255][4][3];
    static uint8_t dix[255][16];
    static bool known[255];
    for (auto& e : dep) for (auto& r : e) for (auto& v : r) v = 0x8000;
    for (auto& e : dix) for (auto& v : e) v = 8;
    known[0] = true;
    bc6h::Dictionary dict;
    dict.ep = dep; dict.ep_known = known; dict.ix = dix; dict.ix_known = known;
    std::vector<uint16_t> zero_ref = sym;
    // in the original vector block 0 already references 0; compare dictionary path to it
    std::vector<uint8_t> out2(16 * nb);
    ok = bc6h::decode_blocks(fb, nb, out2.data(), out2.size(), &st, &dict);
    std::vector<uint8_t> out3(16 * nb);
    std::vector<uint16_t> fb0 = fb;
    fb0[1 * t + 0] = 0;  // x = 0, er = 0
    bc6h::Stats st3;
    bool ok3 = bc6h::decode_blocks(fb0, nb, out3.data(), out3.size(), &st3);
    check(ok && ok3 && st.exact == nb && std::memcmp(out2.data(), out3.data(), out2.size()) == 0,
          "a known dictionary slot is used like a stream reference");

    std::vector<uint16_t> bad = sym;
    bad[15 * t + 2 * 5] = 14;
    check(!bc6h::decode_blocks(bad, nb, out.data(), out.size(), &st), "mode code 14 is rejected");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
