#pragma once

#include "game/GameData.h"

namespace ww {

// Advances one entity's movement state by dt. `targetPos` is the player's
// position (used by dive patterns). `path` supplies control points for
// Bezier/Waypoint patterns (may be null). `scrollSpeed` drifts ground units
// down. The result is deterministic for a given sequence of dt values
// (unit-testable).
Vec2 advanceMovement(MoveState& m, Vec2 spawnPos, Vec2 targetPos,
                     const PathDef* path, float scrollSpeed, float dt);

// Builds a fresh MoveState from an enemy definition plus overrides.
MoveState makeMoveState(const EnemyDef& def, float speedMul, int pathRef);

// Cubic Bezier evaluation (exposed for tests).
Vec2 cubicBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t);

// Position along a polyline at arc-length `dist` (wraps clamped at ends).
Vec2 polylineAt(const std::vector<Vec2>& pts, float dist);
float polylineLength(const std::vector<Vec2>& pts);

}  // namespace ww
