// Experimental (docs/startup-runtime.md): a performance-counter reading in microseconds, the unit
// of the GameInput stand-in's timestamps.
//
// counter * 1000000 does not fit in 64 bits once the counter passes 18,446,744,073,709 ticks. At
// the usual 10 MHz that is 21.35 days of counting; from then on the single product gave timestamps
// that started again near zero. Whole seconds and the rest of a second are converted separately.
#pragma once

#include <cstdint>

namespace x2w_startup {
// floor(counter * 1000000 / frequency). The rest of a second is below frequency, so its product
// fits for every frequency up to 18,446,744,073,709 Hz.
inline std::uint64_t qpc_microseconds(std::uint64_t counter, std::uint64_t frequency)
{
    return counter / frequency * 1000000 + counter % frequency * 1000000 / frequency;
}
}  // namespace x2w_startup
