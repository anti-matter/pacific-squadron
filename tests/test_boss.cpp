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

// Regression: the boss spawn request must be consumed from the spawn queue
// exactly once. Previously the queued boss entry was never erased, so the
// spawn loop re-spawned the boss every iteration and the game hung at
// stage bossTime with the entity pool growing without bound.
void test_boss_natural_spawn() {
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
    stage.name = "natural boss";
    stage.duration = 600;
    stage.bossTime = 2.0f;  // director spawns the boss naturally at t=2s
    stage.boss = "TEST BOAT";
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));
    CHECK(data.stages[0].bossRef >= 0);

    Simulation sim(data, 7);
    sim.startStage(0, {});
    sim.setGodMode(true);

    int hulls = 0;
    for (int f = 0; f < 60 * 12; ++f) {  // 12 s: well past bossTime
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        if (f == 60 * 4) {  // t=4s: exactly one boss exists
            hulls = 0;
            for (const auto& e : sim.entities())
                if (e.alive && e.kind == EKind::BossHull) ++hulls;
            CHECK(hulls == 1);
        }
    }
    hulls = 0;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::BossHull) ++hulls;
    CHECK(hulls == 1);                              // no respawn flood
    CHECK(sim.entityCount() < 64);                  // pool stayed sane
    CHECK(sim.bossView().active == true);
}

// Regression: compaction must not reshuffle the pool while a boss is alive.
// Previously compactIfNeeded() could fire mid-encounter (pool > 256 with
// half the slots free from bullet churn), moving every entity while
// bosses_ kept its raw hull/component indices. The armor gate then read
// arbitrary entities at the stale component indices: if one was alive,
// the hull turned permanently invulnerable and the HP bar stuck at the
// hull-only fraction (~50%) - the boss could never die.
void test_boss_slot_pinning() {
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 1, "fireInterval": 0.02,
        "speed": 120, "count": 3, "lifetime": 8.0 }
    ])";
    const char* powerupsJson = R"([
      { "name": "powerup_p", "effect": "weapon_up", "sprite": "powerup_p" }
    ])";
    const char* bossesJson = R"([
      {
        "name": "TEST BOAT", "hullSprite": "boss_battleship_hull",
        "hullWidth": 160, "hullHeight": 96, "hullHp": 1400, "hullScore": 3000,
        "entryY": 120, "hoverAmp": 24, "hoverFreq": 0.5,
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
    stage.name = "pinning test";
    stage.duration = 600;
    stage.bossTime = 1e9f;
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 99);
    sim.startStage(0, {});
    sim.setGodMode(true);
    sim.debugSpawnBoss(0);

    bool compDown = false, defeated = false, compactionDuringFight = false;
    std::size_t lastPoolSize = 0;
    SimEvent ev;
    for (int f = 0; f < 60 * 90 && !defeated; ++f) {
        // 6 s bursts of fire alternating with 6 s quiet: the bursts drive
        // the pool past the compaction watermark, the quiet phases kill off
        // enough entities for the "half the pool is free" trigger.
        const bool firing = ((f / 60) % 12) < 6;
        sim.setPlayerIntent({0, 0}, firing, false);
        sim.update();
        while (sim.popEvent(ev)) {
            if (ev.type == SimEvent::Type::BossComponentDestroyed) compDown = true;
            if (ev.type == SimEvent::Type::BossDefeated) defeated = true;
        }
        // A shrinking pool while the boss is still fighting means the pool
        // was rebuilt: every stored boss index just went stale.
        if (compDown && !defeated) {
            const std::size_t ps = sim.entities().size();
            if (ps < lastPoolSize) compactionDuringFight = true;
            lastPoolSize = ps;
        }
    }
    CHECK(compDown);
    CHECK(!compactionDuringFight);  // pool stayed stable through the fight
    CHECK(defeated);                // armor gate opened: the hull died
    CHECK(sim.bossView().active == false);
}
