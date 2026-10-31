#include "test_framework.h"

#include "game/Game.h"
#include "game/GameData.h"
#include "game/Simulation.h"

using namespace ww;

void test_scoring() {
    // A minimal stage: light fighters descend straight into the player's
    // gunfire; score must increase deterministically with a fixed seed.
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 1, "fireInterval": 0.14,
        "speed": 520, "count": 2, "lifetime": 1.4 }
    ])";
    const char* enemiesJson = R"([
      { "name": "fighter_light", "hp": 1, "score": 100, "speed": 90,
        "path": "straight", "angleDeg": 90 }
    ])";
    const char* stageJson = R"({
      "name": "T", "duration": 30, "scrollSpeed": 50,
      "events": [
        { "trigger": 0.5, "enemy": "fighter_light", "count": 1, "x": 192, "y": -30 },
        { "trigger": 1.5, "enemy": "fighter_light", "count": 1, "x": 192, "y": -30 },
        { "trigger": 2.5, "enemy": "fighter_light", "count": 1, "x": 192, "y": -30 }
      ]
    })";

    GameData data;
    std::string err;
    CHECK(parseWeaponsJson(weaponsJson, data.weapons, err));
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));
    StageDef stage;
    CHECK(parseStageJson(stageJson, stage, err));
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));

    Simulation sim(data, 1234);
    sim.startStage(0, {});
    for (int f = 0; f < 60 * 8; ++f) {
        sim.setPlayerIntent({0, 0}, true, false);
        sim.update();
    }
    CHECK(sim.score() >= 300);  // all three descending fighters killed

    // Replaying with the same seed reproduces the score exactly.
    Simulation sim2(data, 1234);
    sim2.startStage(0, {});
    for (int f = 0; f < 60 * 8; ++f) {
        sim2.setPlayerIntent({0, 0}, true, false);
        sim2.update();
    }
    CHECK(sim.score() == sim2.score());

    // High-score table handling through Game.
    Game game(data);
    game.loadHighScores({{"ABC", 1000}, {"DEF", 500}, {"GHI", 750}});
    CHECK(game.highScore() == 1000);
    CHECK(game.highScores().size() == 8);
    CHECK(game.highScores()[0].initials == "ABC");
    CHECK(game.highScores()[0].score == 1000);
    CHECK(game.highScores()[1].initials == "GHI");
    CHECK(game.highScores()[2].initials == "DEF");
    CHECK(game.highScores()[7].score == 0);

    // Qualification + insertion (the "29930 run" scenario).
    CHECK(game.scoreQualifies(29930));
    CHECK(!game.scoreQualifies(0));
    game.submitHighScore("WW", 29930);  // short initials pad with A
    CHECK(game.highScore() == 29930);
    CHECK(game.highScores()[0].initials == "WWA");
    CHECK(game.highScores()[0].score == 29930);
    CHECK(game.highScores()[1].score == 1000);
    CHECK(game.highScores().size() == 8);
    bool sawSave = false;
    GameEvent ev;
    while (game.popEvent(ev))
        if (ev.type == GameEvent::Type::SaveHighScores) sawSave = true;
    CHECK(sawSave);

    // Bonus lives picked up mid-run carry across a stage transition, like
    // the score does: startStage resets the player, and Game re-applies the
    // captured lives afterwards.
    {
        const char* bossesJson = R"([
          { "name": "LIVES BOSS", "hullSprite": "boss_battleship_hull",
            "hullWidth": 120, "hullHeight": 80, "hullHp": 1, "hullScore": 100,
            "entryY": 120, "components": [] }
        ])";
        GameData data2;
        CHECK(parseWeaponsJson(weaponsJson, data2.weapons, err));
        CHECK(parseBossesJson(bossesJson, data2.bosses, err));
        StageDef s1; s1.name = "one"; s1.duration = 300; s1.bossTime = 1e9f;
        StageDef s2; s2.name = "two"; s2.duration = 300; s2.bossTime = 1e9f;
        data2.stages.push_back(s1);
        data2.stages.push_back(s2);
        CHECK(data2.linkReferences(err));

        Game game2(data2);
        game2.debugStartAtStage(0);
        for (int f = 0; f < 60 * 4 && game2.state() == GameState::StageIntro; ++f)
            game2.update(1.0f / 60.0f);
        CHECK(game2.state() == GameState::Playing);

        game2.sim().setGodMode(true);
        game2.sim().setLives(4);  // as if two 1UP pickups landed this stage
        game2.sim().debugSpawnBoss(0);
        for (int f = 0; f < 60 * 30 && game2.stageIndex() == 0; ++f) {
            GameInput in;
            in.fire = true;
            game2.submitInput(in);
            game2.update(1.0f / 60.0f);
        }
        CHECK(game2.stageIndex() == 1);
        CHECK(game2.sim().lives() == 4);    // carried, not reset to the default 2
        CHECK(game2.sim().score() >= 100);  // the score carry-over still works
    }
}

namespace {

// Indices of the live enemies currently belonging to formation id.
std::vector<int> formationMembers(const Simulation& sim, int formationId) {
    std::vector<int> out;
    const auto& pool = sim.entities();
    for (std::size_t i = 0; i < pool.size(); ++i)
        if (pool[i].alive && pool[i].kind == EKind::Enemy &&
            pool[i].formation == formationId)
            out.push_back(static_cast<int>(i));
    return out;
}

// Drains the sim event queue looking for a formation wipe bonus.
bool takeBonus(Simulation& sim, SimEvent& out) {
    SimEvent ev;
    while (sim.popEvent(ev))
        if (ev.type == SimEvent::Type::FormationBonus) {
            out = ev;
            return true;
        }
    return false;
}

GameData bonusStageData(const char* enemiesJson, const char* stageJson) {
    const char* weaponsJson = R"([
      { "name": "gun_l1", "pattern": "forward", "damage": 1, "fireInterval": 0.14,
        "speed": 520, "count": 2, "lifetime": 1.4 }
    ])";
    GameData data;
    std::string err;
    CHECK(parseWeaponsJson(weaponsJson, data.weapons, err));
    CHECK(parseEnemiesJson(enemiesJson, data.enemies, err));
    StageDef stage;
    CHECK(parseStageJson(stageJson, stage, err));
    data.stages.push_back(stage);
    CHECK(data.linkReferences(err));
    return data;
}

}  // namespace

// Whole-formation wipes award the enemy type's formationBonus, anchored at
// the last kill; partial wipes and non-bonus types award nothing.
void test_formation_bonus() {
    const char* enemiesJson = R"([
      { "name": "bfighter", "hp": 1, "score": 10, "speed": 40,
        "path": "straight", "angleDeg": 90, "formationBonus": 50 },
      { "name": "nbomber", "hp": 1, "score": 10, "speed": 40,
        "path": "straight", "angleDeg": 90 }
    ])";

    // 1) Full wipe of a 3-plane bonus formation: +50 event at the last kill,
    //    score = kills + the classic 300 clear bonus + the 50 wipe bonus.
    {
        GameData data = bonusStageData(
            enemiesJson,
            R"({ "name": "T", "duration": 60, "scrollSpeed": 50,
                 "events": [ { "trigger": 0.5, "enemy": "bfighter",
                               "formation": "h_line", "count": 3,
                               "x": 192, "y": 60 } ] })");
        Simulation sim(data, 7);
        sim.startStage(0, {});
        for (int f = 0; f < 70; ++f) sim.update();  // spawn all staggered members (no firing)
        const auto members = formationMembers(sim, 1);
        CHECK(members.size() == 3);
        sim.debugKillEnemy(members[0]);
        sim.debugKillEnemy(members[1]);
        const Vec2 lastPos =
            sim.entities()[static_cast<std::size_t>(members[2])].pos;
        sim.debugKillEnemy(members[2]);
        sim.update();  // the formation controller notices the wipe here
        SimEvent ev;
        CHECK(takeBonus(sim, ev));
        CHECK(ev.value == 50);
        CHECK_NEAR(ev.pos.x, lastPos.x, 0.5f);
        CHECK_NEAR(ev.pos.y, lastPos.y, 0.5f);
        CHECK(sim.score() == 3 * 10 + 300 + 50);
    }

    // 2) Partial wipe, one member flies away: no bonus (the player did not
    //    destroy the whole formation).
    {
        const char* fastEnemies = R"([
          { "name": "bfighter", "hp": 1, "score": 10, "speed": 260,
            "path": "straight", "angleDeg": 90, "formationBonus": 50 },
          { "name": "nbomber", "hp": 1, "score": 10, "speed": 40,
            "path": "straight", "angleDeg": 90 }
        ])";
        GameData data = bonusStageData(
            fastEnemies,
            R"({ "name": "T", "duration": 60, "scrollSpeed": 50,
                 "events": [ { "trigger": 0.5, "enemy": "bfighter",
                               "formation": "h_line", "count": 2,
                               "x": 192, "y": -30 } ] })");
        Simulation sim(data, 7);
        sim.startStage(0, {});
        for (int f = 0; f < 70; ++f) sim.update();
        const auto members = formationMembers(sim, 1);
        CHECK(members.size() == 2);
        sim.debugKillEnemy(members[0]);
        for (int f = 0; f < 60 * 4; ++f) sim.update();  // survivor escapes
        SimEvent ev;
        CHECK(!takeBonus(sim, ev));
    }

    // 3) Full wipe of a type without formationBonus: no bonus event.
    {
        GameData data = bonusStageData(
            enemiesJson,
            R"({ "name": "T", "duration": 60, "scrollSpeed": 50,
                 "events": [ { "trigger": 0.5, "enemy": "nbomber",
                               "formation": "h_line", "count": 2,
                               "x": 192, "y": 60 } ] })");
        Simulation sim(data, 7);
        sim.startStage(0, {});
        for (int f = 0; f < 70; ++f) sim.update();
        const auto members = formationMembers(sim, 1);
        CHECK(members.size() == 2);
        sim.debugKillEnemy(members[0]);
        sim.debugKillEnemy(members[1]);
        sim.update();
        SimEvent ev;
        CHECK(!takeBonus(sim, ev));
    }

    // 4) A solo bonus-plane (not a formation) never pays the wipe bonus.
    {
        GameData data = bonusStageData(
            enemiesJson,
            R"({ "name": "T", "duration": 60, "scrollSpeed": 50,
                 "events": [ { "trigger": 0.5, "enemy": "bfighter",
                               "count": 1, "x": 192, "y": 60 } ] })");
        Simulation sim(data, 7);
        sim.startStage(0, {});
        for (int f = 0; f < 70; ++f) sim.update();
        const auto members = formationMembers(sim, -1);
        CHECK(members.size() == 1);
        sim.debugKillEnemy(members[0]);
        sim.update();
        SimEvent ev;
        CHECK(!takeBonus(sim, ev));
    }
}
