#pragma once

#include <vector>

#include "game/GameData.h"

namespace ww {

// A concrete spawn the director wants the simulation to perform.
struct SpawnRequest {
    float time = 0.0f;       // absolute stage time to execute
    int enemyRef = -1;
    Vec2 pos{};
    int formation = -1;      // formation instance id (-1 = solo)
    FormationShape shape = FormationShape::Single;
    int pathRef = -1;        // optional named-path override
    int slot = 0;            // index within formation
    float hpMul = 1.0f;
    float rateMul = 1.0f;
    float speedMul = 1.0f;
    int forceDrop = -1;
    int bossRef = -1;
};

// Turns a StageDef's timeline into concrete spawn requests. Keeps all
// "when does something appear" logic in one place (unit-testable).
class LevelDirector {
public:
    void load(const StageDef& stage);

    // Advances the clock; appends newly-due spawn requests to `out`.
    void update(float dt, std::vector<SpawnRequest>& out);

    float time() const { return time_; }
    float scrollY() const { return scrollY_; }
    bool bossDue() const { return bossDue_; }
    const StageDef* stageDef() const { return stage_; }
    int nextFormationId() { return nextFormationId_++; }

private:
    const StageDef* stage_ = nullptr;
    float time_ = 0.0f;
    float scrollY_ = 0.0f;
    bool bossDue_ = false;
    int nextFormationId_ = 1;
    std::vector<SpawnEvent> events_;  // local copy with fired flags
};

}  // namespace ww
