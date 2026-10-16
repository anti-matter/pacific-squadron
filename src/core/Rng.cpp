#include "core/Rng.h"

namespace ww {

std::uint64_t Rng::nextU64() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint32_t Rng::nextU32() {
    return static_cast<std::uint32_t>(nextU64() >> 32);
}

float Rng::nextFloat() {
    // 24 bits of mantissa precision.
    return static_cast<float>(nextU32() >> 8) / 16777216.0f;
}

int Rng::rangeInt(int lo, int hi) {
    if (hi <= lo) return lo;
    const std::uint32_t span = static_cast<std::uint32_t>(hi - lo + 1);
    return lo + static_cast<int>(nextU32() % span);
}

}  // namespace ww
