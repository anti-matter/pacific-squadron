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

// The BOMB special: one press must wipe every on-screen enemy (air, sea,
// ground) and all enemy fire, while a boss only loses 20% of max hp per
// part - and the hull stays armor-gated while destructible components live.
void test_bomb_sweep_and_boss_20pct() {
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 5, "fireInterval": 0.05,
        "speed": 800, "count": 1, "lifetime": 1.2 },
      { "name": "e_gun", "pattern": "forward", "damage": 1, "fireInterval": 0.3,
        "speed": 90, "count": 1, "lifetime": 8 }
    ])";
    const char* powerupsJson = R"([
      { "name": "powerup_p", "effect": "weapon_up", "sprite": "powerup_p" }
    ])";
    const char* enemiesJson = R"([
      { "name": "popper", "sprite": "enemy_fighter_light",
        "width": 28, "height": 24, "hitbox": [0, 0, 14, 12],
        "hp": 2, "score": 100, "speed": 40, "path": "straight",
        "weapon": "e_gun", "fireInterval": 0.4 }
    ])";
    const char* bossesJson = R"([
      {
        "name": "TEST BOAT", "hullSprite": "boss_battleship_hull",
        "hullWidth": 160, "hullHeight": 96, "hullHp": 100, "hullScore": 3000,
        "entryY": 120, "hoverAmp": 0, "hoverFreq": 0.1,
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
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));
    CHECK(parseBossesJson(bossesJson, data.bosses, err));
    StageDef stage;
    stage.name = "bomb test";
    stage.duration = 600;
    stage.bossTime = 1e9f;  // boss spawned manually
    SpawnEvent ev;
    ev.trigger = 1.0f;
    ev.enemy = "popper";
    ev.formation = "single";
    ev.x = 192.0f;
    ev.y = 200.0f;
    stage.events.push_back(ev);
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 7);
    sim.startStage(0, {});
    sim.setGodMode(true);
    sim.debugSpawnBoss(0);

    // Run 4 s: the popper is on screen and has fired; boss has settled.
    for (int f = 0; f < 60 * 4; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        SimEvent e;
        while (sim.popEvent(e)) {}
    }
    int enemiesBefore = 0, shotsBefore = 0;
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind == EKind::Enemy) ++enemiesBefore;
        if (e.kind == EKind::Shot && e.team == Team::Enemy) ++shotsBefore;
    }
    CHECK(enemiesBefore == 1);
    CHECK(shotsBefore >= 1);

    // One BOMB frame.
    sim.setPlayerIntent({0, 0}, false, true);
    sim.update();
    sim.setPlayerIntent({0, 0}, false, false);
    sim.update();

    int enemiesAfter = 0, shotsAfter = 0, turretHp = -1, hullHp = -1;
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind == EKind::Enemy) ++enemiesAfter;
        if (e.kind == EKind::Shot && e.team == Team::Enemy) ++shotsAfter;
        if (e.kind == EKind::BossComponent) turretHp = e.hp;
        if (e.kind == EKind::BossHull) hullHp = e.hp;
    }
    CHECK(enemiesAfter == 0);                       // sweep killed the popper
    CHECK(shotsAfter == 0);                         // enemy fire erased
    CHECK(turretHp == 8);                           // 20% of 10 off the turret
    CHECK(hullHp == 100);                           // hull armored while turret lives

    // Strip the armor with guns, then bomb again: hull must lose exactly
    // 20% of max (4 of 20), and only from the bomb.
    int hullAtStrip = -1;
    for (int f = 0; f < 60 * 30 && hullAtStrip < 0; ++f) {
        sim.setPlayerIntent({0, 0}, true, false);
        sim.update();
        SimEvent e;
        while (sim.popEvent(e)) {}
        for (const auto& en : sim.entities())
            if (en.alive && en.kind == EKind::BossHull && hullAtStrip < 0) {
                bool comps = false;
                for (const auto& c : sim.entities())
                    if (c.alive && c.kind == EKind::BossComponent) comps = true;
                if (!comps) hullAtStrip = en.hp;
            }
    }
    CHECK(hullAtStrip >= 0);  // turret eventually destroyed
    // Let all in-flight player shots land before testing the bomb, so the
    // hull delta is attributable to the bomb alone.
    for (int f = 0; f < 60 * 2; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        SimEvent e;
        while (sim.popEvent(e)) {}
    }
    int hullPreBomb = -1;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::BossHull) hullPreBomb = e.hp;
    CHECK(hullPreBomb >= 0);
    sim.setPlayerIntent({0, 0}, false, true);
    sim.update();
    sim.setPlayerIntent({0, 0}, false, false);
    int hullAfterBomb = -1;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::BossHull) hullAfterBomb = e.hp;
    CHECK(hullAfterBomb == hullPreBomb - 20);  // 20% of the 100 hp hull
}

// The FLOATING DRYDOCK mechanic: anchored while any mount lives, then it
// undocks (sailing flag) and maneuvers sideways at sailSpeed, bouncing
// inside margins that keep the full hull on screen.
void test_drydock_undocks_and_sails() {
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 5, "fireInterval": 0.05,
        "speed": 800, "count": 1, "lifetime": 1.2 }
    ])";
    const char* powerupsJson = R"([
      { "name": "powerup_p", "effect": "weapon_up", "sprite": "powerup_p" }
    ])";
    const char* bossesJson = R"([
      {
        "name": "TEST DRYDOCK", "hullSprite": "boss_drydock_hull",
        "hullWidth": 160, "hullHeight": 96, "hullHp": 500, "hullScore": 3000,
        "entryY": 120, "hoverAmp": 0, "hoverFreq": 0.05, "sailSpeed": 60,
        "defeatDrop": "powerup_p",
        "components": [
          { "name": "TURRET", "sprite": "boss_big_turret", "offsetX": 0, "offsetY": 0,
            "width": 40, "height": 40, "hp": 5, "score": 500,
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
    stage.name = "sail test";
    stage.duration = 600;
    stage.bossTime = 1e9f;  // boss spawned manually
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 9);
    sim.startStage(0, {});
    sim.setGodMode(true);
    sim.debugSpawnBoss(0);
    const auto hullX = [&sim]() {
        for (const auto& e : sim.entities())
            if (e.alive && e.kind == EKind::BossHull) return e.pos.x;
        return -1.0f;
    };
    SimEvent ev;
    // Wait for the entry descent to finish (hull settles at entryY 120).
    for (int f = 0; f < 60 * 10; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        while (sim.popEvent(ev)) {}
        float y = -999.0f;
        for (const auto& e : sim.entities())
            if (e.alive && e.kind == EKind::BossHull) y = e.pos.y;
        if (y >= 119.5f) break;
    }
    CHECK(sim.bossView().active);
    CHECK(hullX() == 192.0f);        // anchored while the mount lives
    CHECK(!sim.bossView().sailing);

    // Strip the armor with guns.
    bool stripped = false;
    for (int f = 0; f < 60 * 30 && !stripped; ++f) {
        sim.setPlayerIntent({0, 0}, true, false);
        sim.update();
        while (sim.popEvent(ev)) {}
        stripped = true;
        for (const auto& e : sim.entities())
            if (e.alive && e.kind == EKind::BossComponent) stripped = false;
    }
    CHECK(stripped);

    // Undocked: one second of sailing drifts it screen-left noticeably.
    for (int f = 0; f < 60; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        while (sim.popEvent(ev)) {}
    }
    CHECK(sim.bossView().sailing);
    CHECK(hullX() < 192.0f - 20.0f);

    // Bounces keep the whole hull (half 80 + 6 margin) on screen forever,
    // and the swept range proves it actually bounced off both margins.
    float minX = 1e9f, maxX = -1e9f;
    for (int f = 0; f < 60 * 15; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        while (sim.popEvent(ev)) {}
        const float x = hullX();
        CHECK(x >= 86.0f - 0.5f && x <= 298.0f + 0.5f);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
    }
    CHECK(maxX - minX >= 200.0f);  // full margin-to-margin sweep
}

// Regression: formation members must never get stuck off-screen. The// enteredView clamp used to fight the formation controller (pinning
// members at the edge as a group drifted past it), and the off-screen
// despawn gate checked move.t, which never advances for formation
// members (the controller owns their motion) - so pinned members were
// immortal and stayed half-visible at the edge forever.
void test_formation_edge_despawn() {
    const char* enemiesJson = R"([
      { "name": "side_runner", "sprite": "enemy_fighter_light",
        "width": 28, "height": 24, "hitbox": [0, 0, 14, 12],
        "hp": 1, "score": 100, "speed": 210, "path": "sine_side" },
      { "name": "down_runner", "sprite": "enemy_fighter_light",
        "width": 28, "height": 24, "hitbox": [0, 0, 14, 12],
        "hp": 1, "score": 100, "speed": 100, "path": "straight" }
    ])";
    const char* weaponsJson = R"([])";
    const char* powerupsJson = R"([])";
    GameData data;
    std::string err;
    CHECK(parseWeaponsJson(weaponsJson, data.weapons, err));
    CHECK(parsePowerupsJson(powerupsJson, data.powerups, err));
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));
    const char* stageJson = R"({
      "name": "edge test", "duration": 60, "scrollSpeed": 40,
      "events": [
        { "trigger": 1.0, "enemy": "side_runner", "formation": "wave_left",
          "count": 4, "x": 414, "y": 100, "delayBetween": 0.15 },
        { "trigger": 3.0, "enemy": "side_runner", "formation": "v_formation",
          "count": 5, "x": 20, "y": -40, "delayBetween": 0.15, "spacing": 36 }
      ]
    })";
    StageDef stage;
    CHECK(parseStageJson(stageJson, stage, err));
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 5);
    sim.startStage(0, {});
    sim.setGodMode(true);

    bool stuck = false, outOfBounds = false;
    int spawned = 0;
    for (int f = 0; f < 60 * 45; ++f) {
        sim.setPlayerIntent({0, 0}, false, false);
        sim.update();
        for (const auto& e : sim.entities()) {
            if (!e.alive || e.kind != EKind::Enemy) continue;
            ++spawned;
            // Pinned at the view clamp or beyond it after entering.
            if (e.enteredView && (e.pos.x < -30.0f || e.pos.x > 384.0f + 30.0f))
                stuck = true;
            // Any enemy far outside the playfield after a long life is stuck.
            if (e.age > 6.0f && (e.pos.x < -140.0f || e.pos.x > 384.0f + 140.0f ||
                                 e.pos.y > 512.0f + 140.0f))
                outOfBounds = true;
        }
    }
    CHECK(spawned > 0);
    CHECK(!stuck);        // nothing pinned at the edge
    CHECK(!outOfBounds);  // everything off-screen got despawned
    // And at rest, no live enemies remain: both waves left the playfield.
    int live = 0;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::Enemy) ++live;
    CHECK(live == 0);
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
