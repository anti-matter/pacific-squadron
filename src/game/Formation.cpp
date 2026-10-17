#include "game/Formation.h"

#include <cmath>

#include "core/MathUtil.h"

namespace ww {

std::vector<Vec2> formationOffsets(FormationShape shape, int count, float spacing) {
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(count));
    const float c = static_cast<float>(count - 1) * 0.5f;
    for (int i = 0; i < count; ++i) {
        Vec2 p{0.0f, 0.0f};
        switch (shape) {
        case FormationShape::Single:
            break;
        case FormationShape::VLine:
            p = {0.0f, static_cast<float>(i) * spacing};
            break;
        case FormationShape::HLine:
            p = {(static_cast<float>(i) - c) * spacing, 0.0f};
            break;
        case FormationShape::VFormation: {
            // Leader at front, members alternating back-left / back-right.
            const int k = (i + 1) / 2;
            const float side = (i % 2 == 1) ? -1.0f : 1.0f;
            if (i == 0) p = {0.0f, 0.0f};
            else p = {side * static_cast<float>(k) * spacing * 0.9f,
                      static_cast<float>(k) * spacing * 0.8f};
            break;
        }
        case FormationShape::Arc: {
            const float t = count > 1
                ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.5f;
            const float a = degToRad(-55.0f) + t * degToRad(110.0f);
            const float r = spacing * 1.6f;
            p = {std::sin(a) * r, (1.0f - std::cos(a)) * r};
            break;
        }
        case FormationShape::Staggered: {
            const int k = i / 2;
            const float side = (i % 2 == 0) ? -1.0f : 1.0f;
            p = {side * spacing * 0.6f, static_cast<float>(k) * spacing};
            break;
        }
        case FormationShape::SideWaveLeft:
        case FormationShape::SideWaveRight:
        case FormationShape::Wedge: {
            // Echelon wedge trailing back from the anchor.
            p = {(count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f) *
                         spacing * 1.2f,
                 static_cast<float>(i) * spacing * 0.7f};
            break;
        }
        }
        out.push_back(p);
    }
    return out;
}

MoveState formationLeadState(const EnemyDef& def, FormationShape shape,
                             Vec2 anchor, float speedMul) {
    MoveState m;
    m.pattern = def.pattern;
    if (def.ground) {
        // Ground and sea groups ride the world scroll: the simulation
        // passes the terrain scroll rate to their anchor, so Static means
        // "welded to the terrain" and DriftDown means "carried + own way".
        if (m.pattern == MovePattern::Formation) m.pattern = MovePattern::Static;
    } else if (m.pattern == MovePattern::Static || m.pattern == MovePattern::Formation) {
        m.pattern = MovePattern::Straight;
    }
    if (shape == FormationShape::SideWaveLeft) {
        m.pattern = MovePattern::SineSide;
        m.angleDeg = 0.0f;
    } else if (shape == FormationShape::SideWaveRight) {
        m.pattern = MovePattern::SineSide;
        m.angleDeg = 180.0f;
    }
    m.speed = def.speed * speedMul;
    m.amp = def.amp;
    m.freq = def.freq;
    m.phase = 0.0f;
    m.origin = anchor;
    m.lastPos = anchor;
    m.pathRef = def.pathRef;
    return m;
}

}  // namespace ww
