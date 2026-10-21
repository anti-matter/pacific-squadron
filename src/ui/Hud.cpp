#include "ui/Hud.h"

#include <cstdio>

#include "game/Movement.h"
#include "game/Tuning.h"

namespace ww {
namespace {

std::string formatScore(int score) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%07d", score);
    return buf;
}

const char* weaponLabel(WeaponClass w) {
    switch (w) {
    case WeaponClass::Gun: return "GUN";
    case WeaponClass::Spread: return "SPREAD";
    case WeaponClass::Cannon: return "CANNON";
    case WeaponClass::Rocket: return "ROCKET";
    }
    return "GUN";
}

}  // namespace

void Hud::draw(Renderer& r, const Game& game) const {
    const auto& sim = game.sim();
    const auto p = sim.player();

    // Top strip background (taller to breathe with the 11px arcade font).
    r.fillRect(0, 0, kPlayW, 44, Color{0, 0, 0, 120});
    const Color dim = Color::rgb(0x9AB0C8);

    // --- left column: score, hp, weapon ---------------------------------
    r.text("SCORE", 8, 2, 1, dim);
    r.text(formatScore(game.score()), 8, 12, 2, Color::rgb(0xFFFFFF));
    for (int i = 0; i < p.maxHp; ++i) {
        const Color c = i < p.hp ? Color::rgb(0x54D254) : Color::rgb(0x3A3A3A);
        r.fillRect(8 + i * 11, 36, 9, 4, c);
    }
    std::string wl = std::string(weaponLabel(p.weapon)) + " L" +
                     std::to_string(p.weaponLevel);
    r.text(wl, 8 + p.maxHp * 11 + 6, 33, 1, Color::rgb(0xF0D060));

    // --- center column: bomb charges --------------------------------------
    r.text("BOMB", 192, 2, 1, Color::rgb(0x70A8F0), true);
    const int nb = std::min(p.bombs, 4);
    for (int i = 0; i < nb; ++i)
        r.draw(r.texture("powerup_bomb"),
               192.0f + (i - (nb - 1) * 0.5f) * 16.0f, 25, 1.0f,
               Color::rgb(0xFFFFFF), false, 0.0f);

    // --- right column: hi score, lives ------------------------------------
    std::string hi = "HI " + formatScore(game.highScore());
    r.text(hi, kPlayW - 8 - r.textWidth(hi, 1), 2, 1, dim);
    const int planeTex = r.texture("player_fighter");
    for (int i = 0; i < std::min(p.lives, 5); ++i)
        r.drawScaled(planeTex, kPlayW - 14 - i * 19, 21, 16, 16, 1.0f);
    r.text("LIVES", kPlayW - 8 - r.textWidth("LIVES", 1), 29, 1, dim);

    // Boss health bar.
    const auto boss = sim.bossView();
    if (boss.active) {
        const float bw = kPlayW - 64;
        r.fillRect(32, 48, bw, 7, Color{20, 20, 20, 220});
        r.outlineRect(32, 48, bw, 7, Color{90, 90, 100, 200});
        const Color bc = boss.hpFraction > 0.35f ? Color::rgb(0xE04040)
                                                 : Color::rgb(0xFF8020);
        r.fillRect(33, 49, (bw - 2) * clamp(boss.hpFraction, 0.0f, 1.0f), 5, bc);
        r.text(boss.name, kPlayW * 0.5f, 58, 1, Color::rgb(0xFFC0C0), true);
    }
}

void Hud::drawDebug(Renderer& r, const Game& game, float fps) const {
    const auto& sim = game.sim();
    char buf[128];
    const float ms = fps > 0.0f ? 1000.0f / fps : 0.0f;
    std::snprintf(buf, sizeof(buf), "FPS %.0f (%.1f ms)", fps, ms);
    r.text(buf, 4, kPlayH - 62, 1, Color::rgb(0x80FF80));
    int enemies = 0;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::Enemy) ++enemies;
    std::snprintf(buf, sizeof(buf), "ENT %d  ENEMY %d  SHOT %d  FX %d",
                  sim.entityCount(), enemies, sim.projectileCount(),
                  sim.particleCount());
    r.text(buf, 4, kPlayH - 52, 1, Color::rgb(0x80FF80));
    const auto p = sim.player();
    std::snprintf(buf, sizeof(buf), "POS %.0f,%.0f  SPD %.0f", p.pos.x, p.pos.y,
                  length(p.vel));
    r.text(buf, 4, kPlayH - 42, 1, Color::rgb(0x80FF80));
    std::snprintf(buf, sizeof(buf), "TIME %.1fs  SCROLL %.0f",
                  sim.director().time(), sim.director().scrollY());
    r.text(buf, 4, kPlayH - 32, 1, Color::rgb(0x80FF80));
    static const char* kWeapons[] = {"GUN", "SPREAD", "CANNON", "ROCKET"};
    std::snprintf(buf, sizeof(buf), "STATE %d  STAGE %d  WPN %s L%d",
                  static_cast<int>(game.state()), game.stageIndex(),
                  kWeapons[static_cast<int>(p.weapon)], p.weaponLevel);
    r.text(buf, 4, kPlayH - 22, 1, Color::rgb(0x80FF80));
    std::snprintf(buf, sizeof(buf), "HP %d/%d  BOMB %d  LIVES %d%s", p.hp,
                  p.maxHp, p.bombs, p.lives, p.invulnerable ? "  [IFRAME]" : "");
    r.text(buf, 4, kPlayH - 12, 1, Color::rgb(0x80FF80));

    // --- hitboxes (mirrors Simulation::buildColliders) --------------------
    // Player: cyan, deliberately tiny relative to the sprite.
    if (p.alive) {
        constexpr float hb = tuning::kPlayerHitbox;
        r.outlineRect(p.pos.x - hb * 0.5f, p.pos.y - hb * 0.5f, hb, hb,
                      Color::rgb(0x00FFFF));
    }

    const auto& data = game.data();
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind == EKind::Shot) {
            const WeaponDef& w =
                data.weapons[static_cast<std::size_t>(e.weaponDef)];
            float sw = 5.0f, sh = 5.0f;
            shotSize(w.kind, sw, sh);
            r.outlineRect(e.pos.x - sw * 0.5f, e.pos.y - sh * 0.5f, sw, sh,
                          e.team == Team::Player ? Color::rgb(0x80FFFF)
                                                 : Color::rgb(0xFFA000));
        } else if (e.kind == EKind::Enemy && e.enemyDef >= 0) {
            const EnemyDef& def = data.enemies[static_cast<std::size_t>(e.enemyDef)];
            r.outlineRect(e.pos.x + def.hitX - def.hitW * 0.5f,
                          e.pos.y + def.hitY - def.hitH * 0.5f,
                          static_cast<float>(def.hitW),
                          static_cast<float>(def.hitH), Color::rgb(0xFF4040));
        } else if (e.kind == EKind::BossHull) {
            const BossDef& bd = data.bosses[static_cast<std::size_t>(
                sim.bossDefRef(e))];
            r.outlineRect(e.pos.x - (bd.hullWidth - 16.0f) * 0.5f,
                          e.pos.y - (bd.hullHeight - 12.0f) * 0.5f,
                          bd.hullWidth - 16.0f, bd.hullHeight - 12.0f,
                          Color::rgb(0xFF40FF));
        } else if (e.kind == EKind::BossComponent) {
            const BossDef& bd = data.bosses[static_cast<std::size_t>(
                sim.bossDefRef(e))];
            const BossComponentDef& cd =
                bd.components[static_cast<std::size_t>(e.componentIndex)];
            r.outlineRect(e.pos.x - (cd.width - 6.0f) * 0.5f,
                          e.pos.y - (cd.height - 6.0f) * 0.5f, cd.width - 6.0f,
                          cd.height - 6.0f, Color::rgb(0xFFFF40));
        }
    }
}

}  // namespace ww
