#include "test_framework.h"

#include "game/GameData.h"

#include <filesystem>
#include <string>

using namespace ww;

// Loads the SHIPPED asset tree and validates the stage/boss chain, so data
// regressions (renamed bosses, dangling weapon refs, missing stage files)
// fail the suite instead of silently altering gameplay.
void test_shipped_data();

void test_shipped_data() {
    namespace fs = std::filesystem;
    // Locate assets/ relative to this source file (works from any CWD).
    fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const fs::path assets = root / "assets";
    if (!fs::exists(assets / "data" / "stages")) {
        std::printf("  (skipped: shipped assets not found at %s)\n",
                    assets.string().c_str());
        return;
    }

    GameData data;
    std::string err;
    CHECK(data.loadAll(assets.string(), err));

    // All four stages ship and are wired to a resolvable boss.
    CHECK(data.stages.size() >= 4);
    for (const auto& s : data.stages) {
        CHECK(!s.name.empty());
        CHECK(s.bossRef >= 0 && s.bossRef < static_cast<int>(data.bosses.size()));
    }

    // Stage 3: the destroyer hunt.
    const StageDef& st3 = data.stages[2];
    CHECK(st3.name == "HUNT THE STEEL SEA");
    const BossDef& boss = data.bosses[static_cast<std::size_t>(st3.bossRef)];
    CHECK(boss.name == "DESTROYER HAYABUSA-CLASS");
    CHECK(boss.hullSprite == "boss_destroyer_hull");
    // Long and narrow: the hull must be markedly longer than it is wide.
    CHECK(boss.hullHeight >= boss.hullWidth * 3);
    // Three guns + bridge + two AA stations.
    CHECK(boss.components.size() == 6);
    int guns = 0;
    for (const auto& c : boss.components) {
        if (c.sprite == "boss_destroyer_gun") ++guns;
        CHECK(c.weapon.empty() || data.findWeapon(c.weapon) >= 0);
    }
    CHECK(guns == 3);
    // Total effective hp (hull + stations) — the "kill difficulty" budget.
    int totalHp = boss.hullHp;
    for (const auto& c : boss.components) totalHp += c.hp;
    CHECK(totalHp == 194);
    // Stage 3's boss trigger sits inside the stage duration.
    CHECK(st3.bossTime > 0.0f && st3.bossTime < st3.duration);

    // The destroyer's bespoke guns exist.
    CHECK(data.findWeapon("b_dp_gun") >= 0);
    CHECK(data.findWeapon("b_spray5") >= 0);

    // Stage 4: the carrier hunt.
    const StageDef& st4 = data.stages[3];
    CHECK(st4.name == "BREAK THE IRON WING");
    CHECK(st4.bossTime > 0.0f && st4.bossTime < st4.duration);
    const BossDef& carrier = data.bosses[static_cast<std::size_t>(st4.bossRef)];
    CHECK(carrier.name == "CARRIER KAGAMI-CLASS");
    CHECK(carrier.hullSprite == "boss_carrier_hull");
    // Wide beam, unlike the destroyer's narrow hull.
    CHECK(carrier.hullWidth >= 120);
    // Island tower + four AA stations + bow gun.
    CHECK(carrier.components.size() == 6);
    bool hasIsland = false;
    for (const auto& c : carrier.components)
        if (c.sprite == "boss_carrier_island") hasIsland = true;
    CHECK(hasIsland);
    // Kill-difficulty budget: ~30% above the destroyer's 194 hp.
    int carrierHp = carrier.hullHp;
    for (const auto& c : carrier.components) carrierHp += c.hp;
    CHECK(carrierHp == 252);
    // ...within 1% of exactly 1.3x the destroyer budget.
    CHECK(carrierHp * 100 >= 194 * 129 && carrierHp * 100 <= 194 * 131);
    // The air group: the carrier launches the new elite fighter.
    CHECK(carrier.sortieEnemy == "fighter_carrier");
    CHECK(carrier.sortieInterval > 0.0f);
    CHECK(carrier.sortieCount >= 2);
    CHECK(data.findEnemy("fighter_carrier") >= 0);

    // The elite fighter is harder to shoot down than every earlier aircraft:
    // more hp than any pre-stage-4 plane, and a smaller hitbox relative to
    // its sprite than the previous toughest (heavy_fighter).
    const EnemyDef& elite = data.enemies[static_cast<std::size_t>(
        data.findEnemy("fighter_carrier"))];
    CHECK(elite.hp == 6);
    CHECK(elite.hp >= 5);
    const EnemyDef& heavy = data.enemies[static_cast<std::size_t>(
        data.findEnemy("heavy_fighter"))];
    CHECK(static_cast<float>(elite.hitW) / static_cast<float>(elite.width) <
          static_cast<float>(heavy.hitW) / static_cast<float>(heavy.width));
    CHECK(elite.speed > heavy.speed);

    // Stage 5: the island fortress.
    CHECK(data.stages.size() >= 5);
    const StageDef& st5 = data.stages[4];
    CHECK(st5.name == "ISLAND FORTRESS");
    CHECK(st5.bossTime > 0.0f && st5.bossTime < st5.duration);
    const BossDef& fort = data.bosses[static_cast<std::size_t>(st5.bossRef)];
    CHECK(fort.name == "COASTAL FORTRESS");
    CHECK(fort.hullSprite == "boss_fortress_hull");
    // The fortress carries its own island: its arena is open sea.
    CHECK(fort.arenaOcean);
    // Three large main guns, secondaries, AA nests and the command keep.
    CHECK(fort.components.size() == 8);
    int mains = 0;
    for (const auto& c : fort.components)
        if (c.sprite == "boss_big_turret") ++mains;
    CHECK(mains == 3);
    // Kill-difficulty budget: ~34% above the carrier's 252 hp.
    int fortHp = fort.hullHp;
    for (const auto& c : fort.components) fortHp += c.hp;
    CHECK(fortHp == 338);
    CHECK(fortHp * 100 >= 252 * 131 && fortHp * 100 <= 252 * 137);
    // The fortress scrambles Zero fighters; bomber waves arrive via stage
    // events during the boss window.
    CHECK(fort.sortieEnemy == "fighter_light");
    CHECK(fort.sortieInterval > 0.0f);
    CHECK(fort.sortieCount >= 3);
    // Fire rate keeps climbing as it burns: three escalation phases.
    CHECK(fort.phases.size() == 3);
}
