#include "game/LevelScript.h"

#include <algorithm>

#include "game/Formation.h"

namespace ww {

void LevelDirector::load(const StageDef& stage) {
    stage_ = &stage;
    time_ = 0.0f;
    scrollY_ = 0.0f;
    scrollMul_ = 1.0f;
    bossDue_ = false;
    nextFormationId_ = 1;
    events_ = stage.events;
    for (auto& e : events_) e.fired = false;
}

void LevelDirector::update(float dt, std::vector<SpawnRequest>& out) {
    if (!stage_) return;
    time_ += dt;
    scrollY_ += scrollRate() * dt;

    for (auto& ev : events_) {
        if (ev.fired || ev.trigger > time_) continue;
        ev.fired = true;

        if (ev.bossRef >= 0) {
            bossDue_ = true;
            continue;
        }

        const FormationShape shape = formationShapeFromName(ev.formation);
        const bool grouped = shape != FormationShape::Single && ev.count > 1;
        const int formation = grouped ? nextFormationId_++ : -1;
        const auto offsets = formationOffsets(shape, ev.count, ev.spacing);

        for (int i = 0; i < ev.count; ++i) {
            SpawnRequest req;
            req.time = ev.trigger + static_cast<float>(i) * ev.delayBetween;
            req.enemyRef = ev.enemyRef;
            // Grouped members carry the ANCHOR position; the formation
            // controller applies the per-member offsets. Baking them in
            // here too used to double-apply offsets[0] and shift whole
            // formations (partly) off-screen.
            req.pos = grouped
                ? Vec2{ev.x, ev.y}
                : Vec2{ev.x + offsets[static_cast<std::size_t>(i)].x,
                       ev.y + offsets[static_cast<std::size_t>(i)].y};
            req.spacing = ev.spacing;
            req.formation = formation;
            req.shape = shape;
            req.pathRef = ev.pathRef;
            req.slot = i;
            req.hpMul = ev.hpMul;
            req.rateMul = ev.rateMul;
            req.speedMul = ev.speedMul;
            req.forceDrop = ev.forceDrop;
            out.push_back(req);
        }
    }

    if (!bossDue_ && !stage_->boss.empty() && time_ >= stage_->bossTime) {
        bossDue_ = true;
        SpawnRequest req;
        req.time = time_;
        req.bossRef = stage_->bossRef;
        req.pos = {192.0f, -80.0f};
        out.push_back(req);
    }
}

}  // namespace ww
