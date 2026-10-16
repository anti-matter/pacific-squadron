#include "core/Collision.h"

#include <algorithm>
#include <unordered_set>

#include "core/MathUtil.h"

namespace ww {

SpatialGrid::SpatialGrid(float worldW, float worldH, float cellSize)
    : cellSize_(cellSize), invCell_(1.0f / cellSize) {
    cellsX_ = std::max(1, static_cast<int>((worldW + cellSize * 2.0f) * invCell_));
    cellsY_ = std::max(1, static_cast<int>((worldH + cellSize * 2.0f) * invCell_));
    cells_.assign(static_cast<std::size_t>(cellsX_) * static_cast<std::size_t>(cellsY_), {});
}

void SpatialGrid::clear() {
    colliders_.clear();
    for (auto& cell : cells_) cell.clear();
}

void SpatialGrid::cellRange(Rect r, int& x0, int& y0, int& x1, int& y1) const {
    // Offsets allow entities slightly outside the world (spawn/despawn areas).
    const float pad = cellSize_;
    x0 = clamp(static_cast<int>((r.x + pad) * invCell_), 0, cellsX_ - 1);
    y0 = clamp(static_cast<int>((r.y + pad) * invCell_), 0, cellsY_ - 1);
    x1 = clamp(static_cast<int>((r.right() + pad) * invCell_), 0, cellsX_ - 1);
    y1 = clamp(static_cast<int>((r.bottom() + pad) * invCell_), 0, cellsY_ - 1);
}

int SpatialGrid::insert(int entityId, std::uint32_t category, Rect rect) {
    const int index = static_cast<int>(colliders_.size());
    colliders_.push_back(Collider{entityId, category, rect});
    int x0, y0, x1, y1;
    cellRange(rect, x0, y0, x1, y1);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            cells_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsX_) +
                   static_cast<std::size_t>(cx)]
                .push_back(index);
        }
    }
    return index;
}

void SpatialGrid::queryPairs(std::uint32_t maskA, std::uint32_t maskB,
                             const PairFn& fn) const {
    // Track reported pairs so overlapping cells don't report twice.
    std::unordered_set<std::uint64_t> reported;
    for (const auto& cell : cells_) {
        for (std::size_t i = 0; i < cell.size(); ++i) {
            const Collider& a = colliders_[static_cast<std::size_t>(cell[i])];
            for (std::size_t j = i + 1; j < cell.size(); ++j) {
                const Collider& b = colliders_[static_cast<std::size_t>(cell[j])];
                const bool ab = (a.category & maskB) && (b.category & maskA);
                const bool ba = (b.category & maskB) && (a.category & maskA);
                if (!ab && !ba) continue;
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(std::min(cell[i], cell[j])) << 32) |
                    static_cast<std::uint32_t>(std::max(cell[i], cell[j]));
                if (!reported.insert(key).second) continue;
                if (overlaps(a.rect, b.rect)) fn(a, b);
            }
        }
    }
}

void SpatialGrid::queryRect(Rect rect, std::uint32_t mask, const HitFn& fn) const {
    int x0, y0, x1, y1;
    cellRange(rect, x0, y0, x1, y1);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            for (int idx : cells_[static_cast<std::size_t>(cy) *
                                       static_cast<std::size_t>(cellsX_) +
                                   static_cast<std::size_t>(cx)]) {
                const Collider& c = colliders_[static_cast<std::size_t>(idx)];
                if ((c.category & mask) && overlaps(rect, c.rect)) fn(c);
            }
        }
    }
}

}  // namespace ww
