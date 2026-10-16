#include "test_framework.h"

#include "game/GameData.h"
#include "game/LevelScript.h"
#include "game/Simulation.h"  // terrainAt

using namespace ww;

void test_levelscript() {
    const char* json = R"({
      "name": "TEST STAGE",
      "duration": 60,
      "scrollSpeed": 60,
      "boss": "MEGA BOAT",
      "bossTime": 50,
      "terrain": [
        { "fromY": 0, "toY": 1000, "kind": "ocean" },
        { "fromY": 1000, "toY": 2000, "kind": "island" }
      ],
      "events": [
        { "trigger": 10.0, "enemy": "fighter_light", "formation": "v_formation",
          "count": 6, "x": 192, "y": -30, "delayBetween": 0.2 },
        { "trigger": 2.0, "enemy": "aa_turret", "count": 1, "x": 100, "y": -30 }
      ]
    })";
    StageDef stage;
    std::string err;
    CHECK(parseStageJson(json, stage, err));
    CHECK(stage.name == "TEST STAGE");
    CHECK_NEAR(stage.scrollSpeed, 60.0f, 1e-5);
    CHECK(stage.events.size() == 2);
    CHECK(stage.events[0].trigger <= stage.events[1].trigger);  // sorted
    CHECK(stage.terrain.size() == 2);
    CHECK(stage.terrain[1].kind == TerrainKind::Island);

    // Terrain lookup.
    CHECK(terrainAt(stage, 500.0f) == TerrainKind::Ocean);
    CHECK(terrainAt(stage, 1500.0f) == TerrainKind::Island);
    CHECK(terrainAt(stage, 99999.0f) == TerrainKind::Island);  // last segment

    // Director timing.
    LevelDirector dir;
    dir.load(stage);
    std::vector<SpawnRequest> reqs;
    // Advance to just under t=2 (118 frames).
    for (int i = 0; i < 118; ++i) dir.update(1.0f / 60.0f, reqs);
    CHECK(reqs.empty());
    for (int i = 0; i < 3; ++i) dir.update(1.0f / 60.0f, reqs);  // cross t=2
    CHECK(reqs.size() == 1);
    CHECK_NEAR(reqs[0].time, 2.0f, 1e-2);

    // Crossing t=10 schedules six staggered members of one formation.
    reqs.clear();
    for (int i = 0; i < 60 * 8; ++i) dir.update(1.0f / 60.0f, reqs);
    CHECK(reqs.size() == 6);
    CHECK(reqs[0].formation >= 0);
    CHECK(reqs[0].formation == reqs[5].formation);
    for (int i = 1; i < 6; ++i)
        CHECK(reqs[static_cast<std::size_t>(i)].time >
              reqs[static_cast<std::size_t>(i - 1)].time);
    CHECK(reqs[0].shape == FormationShape::VFormation);

    // Scroll distance accumulates.
    CHECK_NEAR(dir.scrollY(), 60.0f * (121.0f / 60.0f + 8.0f), 0.6f);

    // Boss gets scheduled at bossTime.
    bool sawBoss = false;
    for (int i = 0; i < 60 * 40 && !sawBoss; ++i) {
        dir.update(1.0f / 60.0f, reqs);
        for (const auto& r : reqs)
            if (r.bossRef >= 0) sawBoss = true;
    }
    // bossRef unresolved here (no bosses list), so rely on the due flag.
    CHECK(dir.bossDue());
}
