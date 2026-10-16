#pragma once

#include <cmath>
#include <cstdint>

namespace ww {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 r) const { return {x + r.x, y + r.y}; }
    constexpr Vec2 operator-(Vec2 r) const { return {x - r.x, y - r.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 r) { x += r.x; y += r.y; return *this; }
    Vec2& operator-=(Vec2 r) { x -= r.x; y -= r.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(Vec2 r) const { return x == r.x && y == r.y; }
};

constexpr Vec2 operator*(float s, Vec2 v) { return v * s; }

inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float lengthSq(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline float distance(Vec2 a, Vec2 b) { return length(b - a); }
inline float distanceSq(Vec2 a, Vec2 b) { return lengthSq(b - a); }

inline Vec2 normalize(Vec2 v) {
    const float len = length(v);
    if (len < 1e-6f) return {0.0f, 0.0f};
    return v / len;
}

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

// Angle measured like standard math (0 = +x, counter-clockwise).
inline float angleOf(Vec2 v) { return std::atan2(v.y, v.x); }
inline Vec2 fromAngle(float a, float len = 1.0f) {
    return {std::cos(a) * len, std::sin(a) * len};
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;
inline float degToRad(float d) { return d * (kPi / 180.0f); }

}  // namespace ww
