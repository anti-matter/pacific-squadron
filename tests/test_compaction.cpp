#include "test_framework.h"

#include "game/GameData.h"
#include "game/Simulation.h"
#include "game/Tuning.h"

using namespace ww;

// Pool compaction rebuilds pool_ without dead entities, shifting every
// entity index. Formation members are raw pool indices, so they MUST be
// remapped: when they were not, compacted ship formations lost track of
// their members, the members stopped being moved AND stopped being held
// (updateEnemies skips solo motion for formation members), and the frozen
// hulls slid landward with the terrain while the world scrolled under
// them — the stage-5/6 "ships on land" report. The same run must also
// stay free of immortal hulls: a frozen member never reaches the
// off-screen despawn line.
void test_pool_compaction_remaps_formations() {
    const char* enemiesJson = R"([
      { "name": "fodder", "sprite": "enemy_fighter", "width": 32, "height": 26,
        "hitbox": [4, 4, 24, 18], "hp": 1, "score": 50, "speed": 300,
        "path": "drift_down" },
      { "name": "canvass_ship", "sprite": "ship_destroyer", "width": 34,
        "height": 64, "hitbox": [3, 6, 28, 52], "hp": 60, "score": 400,
        "speed": 8, "path": "drift_down", "ground": true, "ship": true }
    ])";
    GameData data;
    std::string err;
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));

    StageDef stage;
    stage.name = "compaction test";
    stage.scrollSpeed = 64.0f;
    stage.duration = 60.0f;
    stage.bossTime = 1e9f;  // no boss
    stage.terrain = {
        {0.0f, 1200.0f, TerrainKind::Ocean},
        {1200.0f, 1330.0f, TerrainKind::Shore},
        {1330.0f, 99999.0f, TerrainKind::Island},
    };
    // A fast fodder stream: every wave exits the bottom (and is despawned)
    // well before the forced compaction, so the free list is full of holes
    // BELOW the ship entity indices and debugCompact() really shifts them.
    for (int k = 0; k < 10; ++k) {
        SpawnEvent wave;
        wave.trigger = 1.0f + 0.2f * static_cast<float>(k);
        wave.enemy = "fodder";
        wave.formation = "h_line";
        wave.count = 6;
        wave.x = 192.0f;
        wave.y = -30.0f;
        wave.spacing = 4.0f;
        stage.events.push_back(wave);
    }
    SpawnEvent ships;
    ships.trigger = 2.0f;
    ships.enemy = "canvass_ship";
    ships.formation = "h_line";
    ships.count = 3;
    ships.x = 192.0f;
    ships.y = -40.0f;
    ships.spacing = 120.0f;
    stage.events.push_back(ships);
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data);
    sim.startStage(0, DifficultyMods{1.0f, 1.0f, 1.0f});
    sim.setGodMode(true);

    const StageDef& sd = data.stages[0];
    bool sawShips = false, compacted = false;
    int shipFrames = 0, offSeaFrames = 0;
    // Frozen-member watchdog: ship entity key (enemyDef) -> last pos +
    // frames it has sat perfectly still.
    float lastY = -1000.0f;
    int frozenFrames = 0, maxFrozen = 0;
    for (int f = 0; f <= 22 * 60; ++f) {
        if (f == 7 * 60) {
            sim.debugCompact();
            compacted = true;
        }
        sim.setPlayerIntent(Vec2{0.0f, 0.0f}, false, false);
        sim.update();
        if ((f % 30) != 0) continue;  // half-second sampling

        float base = sim.director().scrollY() + kPlayH;
        float sumY = 0.0f;
        int alive = 0;
        for (const Entity& e : sim.entities()) {
            if (!e.alive || e.kind != EKind::Enemy || !e.shipUnit) continue;
            sawShips = true;
            ++alive;
            sumY += e.pos.y;
            const float wy = base - e.pos.y;
            ++shipFrames;
            if (terrainAt(sd, wy) != TerrainKind::Ocean) ++offSeaFrames;
        }
        if (alive == 3) {
            const float avg = sumY / 3.0f;
            if (f > 7 * 60 && std::fabs(avg - lastY) < 0.001f)
                ++frozenFrames;
            else
                frozenFrames = 0;
            maxFrozen = frozenFrames > maxFrozen ? frozenFrames : maxFrozen;
            lastY = avg;
        } else {
            frozenFrames = 0;
            lastY = -1000.0f;
        }
        if (f > 16 * 60 && alive > 0) ++offSeaFrames;  // immortal hulls
    }

    CHECK(compacted);
    CHECK(sawShips);
    CHECK(shipFrames > 30);        // ships tracked for a meaningful stretch
    CHECK(offSeaFrames == 0);      // never on land, never immortal
    CHECK(maxFrozen <= 2);         // never froze in place (2 samples slack)
}
