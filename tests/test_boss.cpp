#include "test_framework.h"

#include "game/GameData.h"
#include "game/Simulation.h"

using namespace ww;

void test_boss() {
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 5, "fireInterval": 0.05,
        "speed": 800, "count": 1, "lifetime": 1.2 }
    ])";
    const char* powerupsJson = R"([
      { "name": "powerup_p", "effect": "weapon_up", "sprite": "powerup_p" }
    ])";
    const char* bossesJson = R"([
      {
        "name": "TEST BOAT", "hullSprite": "boss_battleship_hull",
        "hullWidth": 160, "hullHeight": 96, "hullHp": 20, "hullScore": 3000,
        "entryY": 120, "hoverAmp": 40, "hoverFreq": 0.5,
        "defeatDrop": "powerup_p",
        "components": [
          { "name": "TURRET", "sprite": "boss_big_turret", "offsetX": 0, "offsetY": 0,
            "width": 40, "height": 40, "hp": 10, "score": 500,
            "firstFireDelay": 999 }
        ]
      }
    ])";

    GameData data;
    std::string err;
    CHECK(parseWeaponsJson(weaponsJson, data.weapons, err));
    CHECK(parsePowerupsJson(powerupsJson, data.powerups, err));
    CHECK(parseBossesJson(bossesJson, data.bosses, err));
    StageDef stage;
    stage.name = "boss test";
    stage.duration = 600;
    stage.bossTime = 1e9f;  // no automatic boss; spawned manually
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 99);
    sim.startStage(0, {});
    sim.setGodMode(true);
    sim.debugSpawnBoss(0);

    bool defeated = false;
    bool componentDown = false;
    SimEvent ev;
    for (int f = 0; f < 60 * 120 && !defeated; ++f) {
        sim.setPlayerIntent({0, 0}, true, false);
        sim.update();
        while (sim.popEvent(ev)) {
            if (ev.type == SimEvent::Type::BossComponentDestroyed)
                componentDown = true;
            if (ev.type == SimEvent::Type::BossDefeated)
                defeated = true;
        }
    }
    CHECK(componentDown);  // turret destroyed first
    CHECK(defeated);       // then the hull, then the death chain completes
    CHECK(sim.score() >= 3000 + 500);
    CHECK(sim.bossView().active == false);

    // The victory power-up must have been dropped.
    bool sawPowerup = false;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::Powerup) sawPowerup = true;
    CHECK(sawPowerup);
}
