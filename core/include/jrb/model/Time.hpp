#pragma once

#include <cstdint>

namespace jrb {

/// Musical time. See docs/03-audio-engine.md §3 and ADR-008.
using Tick = std::int64_t;

/// 6720 = 2^6 * 3 * 5 * 7: triplets, quintuplets, septuplets and straight
/// notes down to 1/256 are all exact integers.
inline constexpr Tick kTicksPerQuarter = 6720;

} // namespace jrb
