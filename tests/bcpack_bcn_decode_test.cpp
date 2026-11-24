// Synthetic BCPack BC3/BC4/BC5 vectors for x2w_bcpack::decode_bcpack (no title data).
// The streams and expected blocks come from the encoder in tests/bcpack_bcn_check.py
// (seed 7, 4 blocks per format); that test checks the Python reference decoder against the
// same bytes, so both implementations stay in step. Exit code 0 = pass.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/startup_stubs/bcpack_decoder.h"

namespace {

struct Vector {
    unsigned format;
    const char* stream;
    const char* expected;
};

const Vector kVectors[] = {
    { 3,
      "161f1b1010181f18050706090909130705011b0101a301018801200000010065943056f5f3dcff88a924b9a56124db5bc1ffbc744452ff37dc161b1e1b0709130f782501035b0700f231ddfffb74154f9b596bffe137e40cfffed924eddefffd33f8600bff59b629d8ff074404ffcec7ffa2fff1c2ffb43052bdffe4ff03e9f2ffbd04c492ff7ef48cb1f218d1ffb0000054ffbc00d8ff00bd00000000",
      "7e7d878883583f75118cf16338605daf7e7f876c275223349dfce007372de15a7c7fda48642d767b6ef3e9b7544521267b7e529254939b816f74d153cbbcbb9a" },
    { 4,
      "1e191c1b101e1b1506050407060b090608020e92010108042446724900008a02fe0478742cfffffed0316b2950d4e0a6e540ad16e8ffff05c8d9131f1116060b0606050102976f003dc3ffffc4df08d625a4faff14fb1351e0ffff1b1ced11d1d5fff3a0000000ffcd49bbe8f6edff960000ee00000000",
      "1f159241e8177588814b20d64f9d6809814b4e5c8e6a9c4a1d6239643b911aa1" },
    { 5,
      "1f1e1f1f191214130f0711110a0909090101010101021c0301010202eef0cc13ff30ffffcf24ff39f2240302f94781a0ffffffff67411a97f105021a181f150d0d1309031b01032eec00acd3ffff785f81dccbe9a5ff87ffff016af4aeddfff10204f4fedd2afeffffffffd51ce5eefbf101be01d7ff20ffa72105b57dee14f19d02dda9ffff3001fff991fae9030205cbff98ffff8b17a51e09cf3673ff55ff0054c4208dfff6f10106ff2c0000000000f40000000000",
      "7e7d20f48b3a441c8080c4faa9132d81817fa31aa7d2e7e5807e90ed446a84a6327fecf847cbbbf7817b3ea78d44d6627d7cffaf1c1433199b83c264885fed48" },
};

std::vector<unsigned char> hex(const char* s) {
    std::vector<unsigned char> out;
    auto nibble = [](char c) -> unsigned {
        return c <= '9' ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'a' + 10);
    };
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        out.push_back(static_cast<unsigned char>(nibble(s[i]) << 4 | nibble(s[i + 1])));
    }
    return out;
}

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

}  // namespace

int main() {
    for (const Vector& v : kVectors) {
        const std::vector<unsigned char> in = hex(v.stream);
        const std::vector<unsigned char> want = hex(v.expected);
        const std::string tag = "BC" + std::to_string(v.format);
        const size_t block = v.format == 4 ? 8 : 16;

        std::vector<unsigned char> out(want.size(), 0xCD);
        size_t produced = 0;
        bool ok = x2w_bcpack::decode_bcpack(in.data(), in.size(), v.format, out.data(), out.size(), &produced);
        check(ok && produced == want.size() && out == want, tag + " decodes the expected blocks");

        // Zero padding after the stream is accepted (archive intervals are padded).
        std::vector<unsigned char> padded = in;
        padded.resize(in.size() + 16, 0);
        ok = x2w_bcpack::decode_bcpack(padded.data(), padded.size(), v.format, out.data(), out.size(), &produced);
        check(ok && out == want, tag + " accepts trailing zero padding");

        // Negative controls: truncated stream, non-zero trailing byte, wrong block count, size
        // that is not a whole number of blocks.
        ok = x2w_bcpack::decode_bcpack(in.data(), in.size() - 1, v.format, out.data(), out.size(), &produced);
        check(!ok, tag + " rejects a truncated stream");
        std::vector<unsigned char> junk = in;
        junk.push_back(1);
        ok = x2w_bcpack::decode_bcpack(junk.data(), junk.size(), v.format, out.data(), out.size(), &produced);
        check(!ok, tag + " rejects non-zero trailing data");
        std::vector<unsigned char> big(want.size() + block);
        ok = x2w_bcpack::decode_bcpack(in.data(), in.size(), v.format, big.data(), big.size(), &produced);
        check(!ok, tag + " rejects one block more than encoded");
        ok = x2w_bcpack::decode_bcpack(in.data(), in.size(), v.format, out.data(), out.size() - 1, &produced);
        check(!ok, tag + " rejects a size that is not a block multiple");
    }

    // BC6H has a verified lane layout but no block reconstruction: the decoder must refuse it.
    const std::vector<unsigned char> any = hex(kVectors[2].stream);
    std::vector<unsigned char> out(64);
    size_t produced = 0;
    check(!x2w_bcpack::decode_bcpack(any.data(), any.size(), 6, out.data(), out.size(), &produced),
          "BC6H (format 6) is refused");

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
