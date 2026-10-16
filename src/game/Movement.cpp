#include "game/Movement.h"

#include <cmath>

#include "core/MathUtil.h"

namespace ww {

MoveState makeMoveState(const EnemyDef& def, float speedMul, int pathRef) {
    MoveState m;
    m.pattern = def.pattern;
    m.angleDeg = def.angleDeg;
    m.speed = def.speed * speedMul;
    m.amp = def.amp;
    m.freq = def.freq;
    m.delay = def.diveDelay;
    m.pathRef = pathRef >= 0 ? pathRef : def.pathRef;
    m.t = 0.0f;
    m.phase = 0.0f;
    return m;
}

Vec2 cubicBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3.0f * u * u * t) +
           p2 * (3.0f * u * t * t) + p3 * (t * t * t);
}

float polylineLength(const std::vector<Vec2>& pts) {
    float total = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
    return total;
}

Vec2 polylineAt(const std::vector<Vec2>& pts, float dist) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pts[0];
    float acc = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float seg = distance(pts[i - 1], pts[i]);
        if (acc + seg >= dist || i + 1 == pts.size()) {
            const float t = seg > 1e-6f ? clamp((dist - acc) / seg, 0.0f, 1.0f) : 0.0f;
            const Vec2& a = pts[i - 1];
            const Vec2& b = pts[i];
            return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
        }
        acc += seg;
    }
    return pts.back();
}

Vec2 advanceMovement(MoveState& m, Vec2 spawnPos, Vec2 targetPos,
                     const PathDef* path, float scrollSpeed, float dt) {
    // First frame: seed origin/lastPos with the actual spawn position.
    if (m.t == 0.0f) {
        m.origin = spawnPos;
        m.lastPos = spawnPos;
    }
    m.t += dt;

    switch (m.pattern) {
    case MovePattern::Static:
        // Ground units still scroll with the world.
        m.lastPos.y += scrollSpeed * dt;
        break;

    case MovePattern::DriftDown:
        m.lastPos.y += (scrollSpeed + m.speed) * dt;
        break;

    case MovePattern::Straight: {
        const Vec2 v = fromAngle(degToRad(m.angleDeg), m.speed);
        m.lastPos = m.lastPos + v * dt;
        break;
    }

    case MovePattern::SineDown: {
        const float baseX = m.origin.x +
            m.amp * std::sin(2.0f * kPi * m.freq * m.t + m.phase);
        m.lastPos.y += m.speed * dt;
        m.lastPos.x = baseX;
        break;
    }

    case MovePattern::SineSide: {
        const float dir = m.origin.x < 192.0f ? 1.0f : -1.0f;
        m.lastPos.x += dir * m.speed * dt;
        m.lastPos.y = m.origin.y + m.amp * 0.25f *
            std::sin(2.0f * kPi * m.freq * m.t + m.phase) +
            m.speed * 0.15f * m.t;
        break;
    }

    case MovePattern::Circular: {
        // Center drifts downward; entity orbits the center.
        m.origin.y += m.speed * 0.35f * dt;
        const float a = m.phase + 2.0f * kPi * m.freq * m.t;
        m.lastPos = m.origin + fromAngle(a, m.amp);
        break;
    }

    case MovePattern::Bezier: {
        if (path && path->points.size() >= 4) {
            const float dur = m.duration < 1e8f
                ? m.duration
                : polylineLength(path->points) / std::max(10.0f, m.speed);
            const float t = clamp(m.t / dur, 0.0f, 1.0f);
            const auto& p = path->points;
            m.lastPos = cubicBezier(p[0], p[1], p[2], p[3], t);
            // Paths are defined relative to their first point + spawn offset.
            m.lastPos = m.lastPos - p[0] + m.origin;
        } else {
            m.lastPos.y += m.speed * dt;
        }
        break;
    }

    case MovePattern::Waypoints: {
        if (path && path->points.size() >= 2) {
            m.lastPos = polylineAt(path->points, m.speed * m.t) -
                        path->points.front() + m.origin;
        } else {
            m.lastPos.y += m.speed * dt;
        }
        break;
    }

    case MovePattern::DiveAtPlayer: {
        if (m.t < m.delay) {
            // Slow approach while scouting.
            m.lastPos.y += m.speed * 0.35f * dt;
        } else if (!m.diveLocked) {
            m.diveVel = normalize(targetPos - m.lastPos) * (m.speed * 2.2f);
            m.diveLocked = true;
            m.lastPos = m.lastPos + m.diveVel * dt;
        } else {
            m.lastPos = m.lastPos + m.diveVel * dt;
        }
        break;
    }

    case MovePattern::Formation:
        // Position set by the FormationController.
        break;
    }
    return m.lastPos;
}

}  // namespace ww
