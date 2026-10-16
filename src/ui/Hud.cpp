#include "ui/Hud.h"

#include <cstdio>

#include "game/Movement.h"

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

    // Top strip background.
    r.fillRect(0, 0, kPlayW, 34, Color{0, 0, 0, 120});

    r.text("SCORE", 8, 4, 1, Color::rgb(0x9AB0C8));
    r.text(formatScore(game.score()), 8, 13, 2, Color::rgb(0xFFFFFF));
    std::string hi = "HI " + formatScore(game.highScore());
    r.text(hi, kPlayW - 8, 4, 1, Color::rgb(0x9AB0C8), true);

    // Lives as little plane icons.
    const int planeTex = r.texture("player_fighter");
    for (int i = 0; i < std::min(p.lives, 5); ++i)
        r.drawScaled(planeTex, kPlayW - 20 - i * 22, 24, 18, 18, 1.0f);
    r.text("LIVES", kPlayW - 8, 13, 1, Color::rgb(0x9AB0C8), true);

    // Health bar (cockpit hp).
    for (int i = 0; i < p.maxHp; ++i) {
        const Color c = i < p.hp ? Color::rgb(0x54D254) : Color::rgb(0x3A3A3A);
        r.fillRect(8 + i * 12, 28, 9, 4, c);
    }

    // Weapon + level.
    std::string wl = std::string(weaponLabel(p.weapon)) + " L" +
                     std::to_string(p.weaponLevel);
    r.text(wl, 60, 28, 1, Color::rgb(0xF0D060));

    // Bombs (special count).
    r.text("BOMB", 130, 28, 1, Color::rgb(0x70A8F0));
    for (int i = 0; i < p.bombs && i < 4; ++i)
        r.draw(r.texture("powerup_bomb"), 168 + i * 14, 31, 1.0f,
               Color::rgb(0xFFFFFF), false, 0.0f);

    // Boss health bar.
    const auto boss = sim.bossView();
    if (boss.active) {
        const float bw = kPlayW - 64;
        r.fillRect(32, 40, bw, 7, Color{20, 20, 20, 220});
        const Color bc = boss.hpFraction > 0.35f ? Color::rgb(0xE04040)
                                                 : Color::rgb(0xFF8020);
        r.fillRect(33, 41, (bw - 2) * clamp(boss.hpFraction, 0.0f, 1.0f), 5, bc);
        r.text(boss.name, kPlayW * 0.5f, 50, 1, Color::rgb(0xFFC0C0), true);
    }
}

void Hud::drawDebug(Renderer& r, const Game& game, float fps) const {
    const auto& sim = game.sim();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "FPS %.0f  ENT %d  SHOT %d", fps,
                  sim.entityCount(), sim.projectileCount());
    r.text(buf, 4, kPlayH - 32, 1, Color::rgb(0x80FF80));
    std::snprintf(buf, sizeof(buf), "TIME %.1fs  Y %.0f",
                  sim.director().time(), sim.director().scrollY());
    r.text(buf, 4, kPlayH - 22, 1, Color::rgb(0x80FF80));
    std::snprintf(buf, sizeof(buf), "STATE %d  STAGE %d",
                  static_cast<int>(game.state()), game.stageIndex());
    r.text(buf, 4, kPlayH - 12, 1, Color::rgb(0x80FF80));

    // Player hitbox.
    const auto p = sim.player();
    if (p.alive)
        r.outlineRect(p.pos.x - 5.0f, p.pos.y - 5.0f, 10.0f, 10.0f,
                      Color::rgb(0x00FFFF));

    // Enemy hitboxes + spawn anchors.
    const auto& data = game.data();
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind == EKind::Enemy && e.enemyDef >= 0) {
            const EnemyDef& def = data.enemies[static_cast<std::size_t>(e.enemyDef)];
            r.outlineRect(e.pos.x + def.hitX - def.hitW * 0.5f,
                          e.pos.y + def.hitY - def.hitH * 0.5f,
                          static_cast<float>(def.hitW),
                          static_cast<float>(def.hitH), Color::rgb(0xFF4040));
        } else if (e.kind == EKind::BossHull || e.kind == EKind::BossComponent) {
            r.outlineRect(e.pos.x - 12.0f, e.pos.y - 12.0f, 24.0f, 24.0f,
                          Color::rgb(0xFF40FF));
        }
    }
}

}  // namespace ww
