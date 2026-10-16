#include "test_framework.h"

#include "core/Collision.h"

using namespace ww;

void test_collision() {
    Rect a{0, 0, 10, 10};
    Rect b{9, 9, 10, 10};
    Rect c{20, 20, 5, 5};
    CHECK(overlaps(a, b));
    CHECK(!overlaps(a, c));
    CHECK(overlaps(rectFromCenter({5, 5}, 10, 10), a));

    SpatialGrid grid(384, 512, 64);
    grid.insert(1, kCatPlayer, Rect{100, 100, 10, 10});
    grid.insert(2, kCatEnemyProjectile, Rect{104, 104, 5, 5});
    grid.insert(3, kCatEnemyProjectile, Rect{300, 300, 5, 5});

    int pairs = 0;
    grid.queryPairs(kCatAllMask(), kCatAllMask(),
                    [&](const Collider&, const Collider&) { ++pairs; });
    // Only colliders 1&2 overlap.
    CHECK(pairs == 1);

    // Category masks filter pairs.
    int airPairs = 0;
    grid.queryPairs(kCatPlayer, kCatEnemyAir,
                    [&](const Collider&, const Collider&) { ++airPairs; });
    CHECK(airPairs == 0);

    int hits = 0;
    grid.queryRect(Rect{95, 95, 20, 20}, kCatEnemyProjectile,
                   [&](const Collider& col) {
                       if (col.entityId == 2) ++hits;
                   });
    CHECK(hits == 1);

    grid.clear();
    CHECK(grid.colliderCount() == 0);
}
