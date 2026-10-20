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
    float spacing = 46.0f;   // formation member spacing
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
    // Difficulty-adjusted world scroll rate. THE single source of truth:
    // terrain rendering (scrollY) and ground-unit drift must both use it,
    // or installations visibly slide over the terrain. Frozen during a
    // settled boss fight (see setScrollFrozen) so the battle takes place
    // over one fixed location.
    void setScrollMul(float m) { scrollMul_ = m; }
    void setScrollFrozen(bool f) { scrollFrozen_ = f; }
    float scrollRate() const {
        if (scrollFrozen_) return 0.0f;
        return (stage_ ? stage_->scrollSpeed : 60.0f) * scrollMul_;
    }
    bool bossDue() const { return bossDue_; }
    const StageDef* stageDef() const { return stage_; }
    // Debug: fire this stage's boss at the given stage-clock time instead of
    // the scripted bossTime (WWII_BOSS_NOW). Applies to the currently loaded
    // stage only; the next stage's load() restores the scripted timing.
    void setBossTimeOverride(float t) { bossTimeOverride_ = t; }
    int nextFormationId() { return nextFormationId_++; }

private:
    const StageDef* stage_ = nullptr;
    float time_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollMul_ = 1.0f;
    bool scrollFrozen_ = false;
    bool bossDue_ = false;
    float bossTimeOverride_ = -1.0f;
    int nextFormationId_ = 1;
    std::vector<SpawnEvent> events_;  // local copy with fired flags
};

}  // namespace ww
