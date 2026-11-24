// Checks src/startup_stubs/qpc_microseconds.h: a performance-counter reading becomes microseconds
// exactly, also where counter * 1000000 no longer fits in 64 bits.
//   --mutate product   the conversion with one 64-bit product; must fail (negative control)
#include "startup_stubs/qpc_microseconds.h"

#include <cstdio>
#include <cstring>

namespace {
int failures;
bool mutate;

std::uint64_t microseconds(std::uint64_t counter, std::uint64_t frequency)
{
    if (!mutate) return x2w_startup::qpc_microseconds(counter, frequency);
    // The behaviour before: one product, which wraps above 18,446,744,073,709 ticks.
    return counter * 1000000 / frequency;
}

void expect(const char* name, std::uint64_t counter, std::uint64_t frequency, std::uint64_t wanted)
{
    const std::uint64_t got = microseconds(counter, frequency);
    if (got != wanted) {
        std::printf("FAIL %s: counter %llu at %llu Hz gave %llu, expected %llu\n", name, static_cast<unsigned long long>(counter),
                    static_cast<unsigned long long>(frequency), static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(wanted));
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    mutate = argc == 3 && !std::strcmp(argv[1], "--mutate") && !std::strcmp(argv[2], "product");
    if (argc != 1 && !mutate) return 2;
    // 10 MHz, the usual frequency: ten ticks a microsecond.
    const std::uint64_t ten_mhz = 10000000;
    expect("zero", 0, ten_mhz, 0);
    expect("below one microsecond", 9, ten_mhz, 0);
    expect("one microsecond", 10, ten_mhz, 1);
    expect("one second", 10000000, ten_mhz, 1000000);
    expect("about 20 minutes", 12345678901, ten_mhz, 1234567890);
    // 18,446,744,073,709 is the last counter whose product with 1000000 fits: 21.35 days at 10 MHz.
    expect("last counter one product holds", 18446744073709, ten_mhz, 1844674407370);
    expect("first counter past one product", 18446744073710, ten_mhz, 1844674407371);
    expect("about 23 days", 20000000000000, ten_mhz, 2000000000000);
    expect("one tick before 30 days", 25919999999999, ten_mhz, 2591999999999);
    expect("30 days", 25920000000000, ten_mhz, 2592000000000);
    expect("largest signed counter", 0x7FFFFFFFFFFFFFFF, ten_mhz, 922337203685477580);
    // Frequencies that do not divide 1000000: the rest of a second is rounded down.
    expect("3,579,545 Hz with a rest", 20000000000000, 3579545, 5587302296800);
    expect("3,579,545 Hz, one tick before 70 days", 21649088159999, 3579545, 6047999999999);
    expect("3,579,545 Hz, 70 days", 21649088160000, 3579545, 6048000000000);
    expect("2,435,886 Hz, 400 days and 1,234,567 ticks", 84184221394567, 2435886, 34560000506824);
    // A counter that already is in microseconds, and one coarser than a microsecond.
    expect("1 MHz, largest signed counter", 0x7FFFFFFFFFFFFFFF, 1000000, 0x7FFFFFFFFFFFFFFF);
    expect("1 kHz", 1999, 1000, 1999000);
    if (failures) {
        std::printf("startup_qpc_microseconds: %d FAILED\n", failures);
        return 1;
    }
    std::printf("startup_qpc_microseconds: PASS (17 conversions)\n");
    return 0;
}
