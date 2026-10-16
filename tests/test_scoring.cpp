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
}
