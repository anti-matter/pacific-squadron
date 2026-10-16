#include "test_framework.h"

#include "game/Movement.h"

using namespace ww;

void test_movement() {
    // Straight movement integrates deterministically.
    EnemyDef def{};
    def.pattern = MovePattern::Straight;
    def.speed = 100.0f;
    def.angleDeg = 90.0f;  // down
    MoveState m = makeMoveState(def, 1.0f, -1);
    Vec2 pos{100.0f, 0.0f};
    for (int i = 0; i < 60; ++i)
        pos = advanceMovement(m, pos, {}, nullptr, 0.0f, 1.0f / 60.0f);
    CHECK_NEAR(pos.x, 100.0f, 0.01);
    CHECK_NEAR(pos.y, 100.0f, 0.05);  // ~1s at 100 px/s

    // SineDown oscillates horizontally while descending.
    EnemyDef def2{};
    def2.pattern = MovePattern::SineDown;
    def2.speed = 60.0f;
    def2.amp = 30.0f;
    def2.freq = 1.0f;
    MoveState m2 = makeMoveState(def2, 1.0f, -1);
    Vec2 p2{200.0f, 0.0f};
    float minX = 1e9f, maxX = -1e9f;
    for (int i = 0; i < 180; ++i) {
        p2 = advanceMovement(m2, p2, {}, nullptr, 0.0f, 1.0f / 60.0f);
        minX = minX < p2.x ? minX : p2.x;
        maxX = maxX > p2.x ? maxX : p2.x;
    }
    CHECK(maxX - minX > 40.0f);           // visibly oscillates
    CHECK(maxX - minX < 70.0f);           // bounded by amplitude
    CHECK_NEAR(p2.y, 180.0f, 0.5f);       // descended 3 s at 60 px/s

    // Static ground units drift with the world scroll.
    EnemyDef def3{};
    def3.pattern = MovePattern::Static;
    MoveState m3 = makeMoveState(def3, 1.0f, -1);
    Vec2 p3{50.0f, 10.0f};
    for (int i = 0; i < 60; ++i)
        p3 = advanceMovement(m3, p3, {}, nullptr, 40.0f, 1.0f / 60.0f);
    CHECK_NEAR(p3.y, 50.0f, 0.01);

    // Dive locks onto the player position at trigger time.
    EnemyDef def4{};
    def4.pattern = MovePattern::DiveAtPlayer;
    def4.speed = 100.0f;
    def4.diveDelay = 0.5f;
    MoveState m4 = makeMoveState(def4, 1.0f, -1);
    Vec2 p4{100.0f, 50.0f};
    Vec2 player{200.0f, 400.0f};
    for (int i = 0; i < 60; ++i)
        p4 = advanceMovement(m4, p4, player, nullptr, 0.0f, 1.0f / 60.0f);
    // After the lock the dive velocity keeps pointing at the original target.
    CHECK(m4.diveLocked);
    Vec2 dir = normalize(m4.diveVel);
    Vec2 want = normalize(player - Vec2{100.0f, 50.0f});
    CHECK_NEAR(dir.x, want.x, 0.15);
    CHECK_NEAR(dir.y, want.y, 0.15);

    // Cubic bezier endpoints.
    const Vec2 a{0, 0}, b{10, 0}, c{10, 10}, d{0, 10};
    Vec2 p0 = cubicBezier(a, b, c, d, 0.0f);
    Vec2 p1 = cubicBezier(a, b, c, d, 1.0f);
    CHECK_NEAR(p0.x, 0.0, 1e-4);
    CHECK_NEAR(p1.y, 10.0, 1e-4);

    // Polyline arc-length lookup.
    std::vector<Vec2> pts{{0, 0}, {100, 0}, {100, 100}};
    CHECK_NEAR(polylineLength(pts), 200.0, 1e-4);
    Vec2 q = polylineAt(pts, 150.0f);
    CHECK_NEAR(q.x, 100.0, 1e-4);
    CHECK_NEAR(q.y, 50.0, 1e-4);
}
