#pragma once

#include <cstdint>

namespace ww {

// Small deterministic RNG (splitmix64-based). Gameplay uses a seeded
// instance so runs can be reproduced and unit tests are stable.
class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ull) : state_(seed) {}

    std::uint64_t nextU64();
    std::uint32_t nextU32();
    // Uniform in [0, 1)
    float nextFloat();
    // Uniform in [lo, hi)
    float range(float lo, float hi) { return lo + nextFloat() * (hi - lo); }
    // Uniform integer in [lo, hi] inclusive.
    int rangeInt(int lo, int hi);
    bool chance(float p) { return nextFloat() < p; }

    void reseed(std::uint64_t seed) { state_ = seed; }

private:
    std::uint64_t state_;
};

}  // namespace ww
