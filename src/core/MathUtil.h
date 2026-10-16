#pragma once

#include <algorithm>
#include <cmath>

#include "core/Vec2.h"

namespace ww {

template <typename T>
constexpr T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Frame-rate independent exponential smoothing factor.
inline float smoothFactor(float rate, float dt) {
    return 1.0f - std::exp(-rate * dt);
}

// Move `v` toward `target` by at most `maxDelta`.
constexpr float approach(float v, float target, float maxDelta) {
    if (v < target) return std::min(v + maxDelta, target);
    return std::max(v - maxDelta, target);
}

// Wrap angle into [-pi, pi].
inline float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

}  // namespace ww
