#include "test_framework.h"

#include "game/GameData.h"
#include "game/Simulation.h"

using namespace ww;

// Ground installations must be welded to the terrain: their per-frame
// descent has to equal the terrain scroll (director scrollY delta) at any
// difficulty, for solo AND grouped (formation-anchored) spawns. Historically
// two bugs broke this: formation anchors advanced with scroll = 0 (turrets
// froze off-screen / ships slid backwards), and the difficulty speedMul was
// applied to unit drift but not to the terrain clock.
void test_ground_scroll_anchor() {
    const char* enemiesJson = R"([
      { "name": "gun_emplacement", "sprite": "turret_aa", "width": 32, "height": 32,
        "hitbox": [0, 0, 16, 14], "hp": 3, "score": 100, "speed": 0,
        "path": "static", "ground": true },
      { "name": "tracked_vehicle", "sprite": "tank", "width": 40, "height": 28,
        "hitbox": [0, 0, 22, 16], "hp": 4, "score": 100, "speed": 15,
        "path": "drift_down", "ground": true }
    ])";
    GameData data;
    std::string err;
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));

    StageDef stage;
    stage.name = "anchor test";
    stage.scrollSpeed = 60.0f;
    stage.duration = 600.0f;
    stage.bossTime = 1e9f;  // no boss
    SpawnEvent solo;
    solo.trigger = 0.5f;
    solo.enemy = "gun_emplacement";
    solo.count = 1;
    solo.x = 96.0f;
    solo.y = -30.0f;
    SpawnEvent group;
    group.trigger = 0.5f;
    group.enemy = "gun_emplacement";
    group.formation = "h_line";
    group.count = 2;
    group.x = 192.0f;
    group.y = -30.0f;
    group.spacing = 60.0f;
    stage.events.push_back(solo);
    stage.events.push_back(group);
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    // Runs the stage for `frames`, returns {max |slip|, tracked frames} for
    // static ground units. Slip = per-frame descent minus terrain scroll.
    auto run = [&](DifficultyMods mods, float& maxSlipOut, int& samplesOut) {
        Simulation sim(data);
        sim.startStage(0, mods);
        float prevScroll = sim.director().scrollY();
        float prevY[64];
        bool seen[64] = {};
        maxSlipOut = 0.0f;
        samplesOut = 0;
        for (int f = 0; f < 300; ++f) {  // 5 s
            sim.update();
            const float scrollDy = sim.director().scrollY() - prevScroll;
            const auto& pool = sim.entities();
            for (std::size_t i = 0; i < pool.size() && i < 64; ++i) {
                const Entity& e = pool[i];
                if (!e.alive || e.kind != EKind::Enemy) { seen[i] = false; continue; }
                const EnemyDef& def = data.enemies[static_cast<std::size_t>(e.enemyDef)];
                if (!def.ground || def.speed > 0.0f ||
                    e.pos.y < 0.0f || e.pos.y > 400.0f) { seen[i] = false; continue; }
                if (seen[i]) {
                    const float slip = std::fabs(e.pos.y - prevY[i] - scrollDy);
                    if (slip > maxSlipOut) maxSlipOut = slip;
                    ++samplesOut;
                }
                seen[i] = true;
                prevY[i] = e.pos.y;
            }
            prevScroll = sim.director().scrollY();
        }
    };

    // Normal difficulty: scroll rate 60 px/s; solo + 2 grouped turrets.
    float slip = 0.0f;
    int samples = 0;
    run(DifficultyMods{1.0f, 1.0f, 1.0f}, slip, samples);
    CHECK(samples >= 3 * 60);   // all three turrets tracked on-screen
    CHECK(slip < 0.02f);        // welded to the terrain

    // Hard difficulty (speedMul 1.1): terrain and units must match too.
    run(DifficultyMods{1.25f, 0.72f, 1.1f}, slip, samples);
    CHECK(samples >= 3 * 60);
    CHECK(slip < 0.02f);

    // Easy difficulty (speedMul 0.9) for symmetry.
    run(DifficultyMods{0.8f, 1.35f, 0.9f}, slip, samples);
    CHECK(samples >= 3 * 60);
    CHECK(slip < 0.02f);

    // The director clock is the single source of truth for the world speed.
    Simulation sim(data);
    sim.startStage(0, DifficultyMods{1.0f, 1.0f, 0.5f});
    CHECK_NEAR(sim.director().scrollRate(), 30.0f, 1e-3);
}
