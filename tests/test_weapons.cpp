#include "test_framework.h"

#include "game/GameData.h"
#include "game/Simulation.h"

using namespace ww;

void test_weapons() {
    // --- data-driven weapon parsing ---------------------------------------
    const char* json = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 2, "fireInterval": 0.2,
        "speed": 500, "count": 2, "lifetime": 1.5, "penetration": 1 },
      { "name": "e_rad", "pattern": "radial", "kind": "enemy_bullet",
        "speed": 90, "count": 8, "fireInterval": 1.0 }
    ])";
    std::vector<WeaponDef> weapons;
    std::string err;
    CHECK(parseWeaponsJson(json, weapons, err));
    CHECK(weapons.size() == 2);
    CHECK(weapons[0].damage == 2);
    CHECK_NEAR(weapons[0].interval, 0.2f, 1e-5);
    CHECK(weapons[0].penetration == 1);
    CHECK(weapons[0].pattern == ShotPattern::Forward);
    CHECK(weapons[1].pattern == ShotPattern::Radial);
    CHECK(weapons[1].count == 8);

    // --- player fire cadence ----------------------------------------------
    // A stage-less simulation still runs the player firing system.
    GameData data;
    data.weapons = weapons;
    data.powerups = {{"powerup_p", PowerupKind::WeaponUp, "powerup_p", 55.0f}};
    Simulation sim(data, 42);
    sim.startStage(0, {});

    int shotsAtSecond = -1;
    int shotsAfterTwo = -1;
    for (int f = 0; f < 60; ++f) {
        sim.setPlayerIntent({0, 0}, true, false);
        sim.update();
    }
    // interval 0.2s -> 5 volleys/s * 2 shots = ~10 player shots alive
    // (some have expired given lifetime 1.5s).
    int alive = 0;
    for (const auto& e : sim.entities())
        if (e.alive && e.kind == EKind::Shot && e.team == Team::Player) ++alive;
    CHECK(alive >= 6);
    CHECK(alive <= 14);
    (void)shotsAtSecond;
    (void)shotsAfterTwo;

    // --- enemy weapon timing ----------------------------------------------
    // An enemy with interval 1.0 must not fire before its first delay.
    GameData data2;
    data2.weapons = weapons;
    EnemyDef ed{};
    ed.name = "dummy";
    ed.pattern = MovePattern::Static;
    ed.weapon = "e_rad";
    ed.weaponRef = 1;
    ed.fireInterval = 1.0f;
    ed.firstFireDelay = 0.5f;
    ed.aimAtPlayer = false;
    data2.enemies.push_back(ed);
    Simulation sim2(data2, 7);
    sim2.startStage(0, {});
    // Drive the director with a stage that spawns the dummy at t=0.
    StageDef stage;
    stage.name = "test";
    SpawnEvent ev;
    ev.trigger = 0.0f;
    ev.enemyRef = 0;
    ev.x = 192.0f;
    ev.y = 100.0f;
    stage.events.push_back(ev);
    // startStage uses GameData::stages; push the stage.
    data2.stages.push_back(stage);
    Simulation sim3(data2, 7);
    sim3.startStage(0, {});
    int enemyShotsBefore = 0;
    for (int f = 0; f < 30; ++f) {  // 0.5 s
        sim3.update();
    }
    for (const auto& e : sim3.entities())
        if (e.alive && e.kind == EKind::Shot && e.team == Team::Enemy)
            ++enemyShotsBefore;
    CHECK(enemyShotsBefore == 0);  // firstFireDelay not elapsed
    for (int f = 0; f < 60; ++f) sim3.update();
    int enemyShotsAfter = 0;
    for (const auto& e : sim3.entities())
        if (e.alive && e.kind == EKind::Shot && e.team == Team::Enemy)
            ++enemyShotsAfter;
    CHECK(enemyShotsAfter >= 8);   // radial volley of 8 has been fired
}
