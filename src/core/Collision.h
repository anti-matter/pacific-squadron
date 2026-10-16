#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Vec2.h"

namespace ww {

// Axis-aligned rectangle in world space.
struct Rect {
    float x = 0.0f;  // left
    float y = 0.0f;  // top
    float w = 0.0f;
    float h = 0.0f;

    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
};

constexpr Rect rectFromCenter(Vec2 c, float w, float h) {
    return {c.x - w * 0.5f, c.y - h * 0.5f, w, h};
}

inline bool overlaps(Rect a, Rect b) {
    return a.x < b.right() && b.x < a.right() &&
           a.y < b.bottom() && b.y < a.bottom();
}

// Collision categories. Broadphase filters pairs by these bits; the game
// layer decides what each matched pair means.
enum CollisionBits : std::uint32_t {
    kCatNone             = 0,
    kCatPlayer           = 1u << 0,
    kCatPlayerProjectile = 1u << 1,
    kCatEnemyAir         = 1u << 2,
    kCatEnemyGround      = 1u << 3,
    kCatEnemyProjectile  = 1u << 4,
    kCatPowerup          = 1u << 5,
    kCatBoss             = 1u << 6,
};

inline constexpr std::uint32_t kCatAllMask() {
    return kCatPlayer | kCatPlayerProjectile | kCatEnemyAir | kCatEnemyGround |
           kCatEnemyProjectile | kCatPowerup | kCatBoss;
}

// A lightweight collider registered with the spatial grid.
struct Collider {
    int entityId = -1;
    std::uint32_t category = kCatNone;
    Rect rect{};
};

// Uniform-grid broad-phase. Small enough to keep simple, structured so a
// different broad-phase could be dropped in later without touching callers.
class SpatialGrid {
public:
    SpatialGrid(float worldW, float worldH, float cellSize);

    void clear();
    int insert(int entityId, std::uint32_t category, Rect rect);

    // Calls fn(a, b) for each overlapping pair whose categories match the
    // masks (a.category & b.mask && b.category & a.mask). Each unordered
    // pair is reported once.
    using PairFn = std::function<void(const Collider&, const Collider&)>;
    void queryPairs(std::uint32_t maskA, std::uint32_t maskB, const PairFn& fn) const;

    // Calls fn for each collider overlapping `rect` with (category & mask).
    using HitFn = std::function<void(const Collider&)>;
    void queryRect(Rect rect, std::uint32_t mask, const HitFn& fn) const;

    int colliderCount() const { return static_cast<int>(colliders_.size()); }
    const Collider& colliderAt(int index) const {
        return colliders_[static_cast<std::size_t>(index)];
    }

private:
    int cellsX_ = 1;
    int cellsY_ = 1;
    float cellSize_ = 64.0f;
    float invCell_ = 1.0f / 64.0f;
    std::vector<Collider> colliders_;
    std::vector<std::vector<int>> cells_;

    void cellRange(Rect r, int& x0, int& y0, int& x1, int& y1) const;
};

}  // namespace ww
