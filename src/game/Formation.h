#pragma once

#include <vector>

#include "game/GameData.h"

namespace ww {

// Member positions relative to a formation anchor for `count` members.
// Pure function so unit tests can verify geometry.
std::vector<Vec2> formationOffsets(FormationShape shape, int count, float spacing = 46.0f);

// Movement state for the formation anchor itself, built from the event or
// enemy def that created the group.
MoveState formationLeadState(const EnemyDef& def, FormationShape shape,
                             Vec2 anchor, float speedMul);

}  // namespace ww
