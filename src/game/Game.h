#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/GameData.h"
#include "game/Simulation.h"

namespace ww {

// All game-state transitions live here (title/menu/playing/...). Classes
// outside Game never trigger state changes themselves; they raise SimEvents
// and Game reacts.
enum class GameState : std::uint8_t {
    Title,
    Options,
    HighScores,
    StageIntro,
    Playing,
    Paused,
    StageComplete,
    GameOver,
    Victory,
};

struct GameInput {
    // Movement axis from the input layer, -1..1.
    float moveX = 0.0f;
    float moveY = 0.0f;
    bool fire = false;
    bool special = false;
    // Menu / system actions (edge-detected inside Game).
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool confirm = false;
    bool back = false;
    bool pause = false;
    bool debugToggle = false;
};

struct GameEvent {
    enum class Type : std::uint8_t {
        Sound,           // presentation sound by name
        SaveHighScores,  // persist the table now
        SaveSettings,
        Quit,            // leave the application (main menu QUIT)
    };
    Type type;
    std::string name;
};

enum class Difficulty : std::uint8_t { Easy = 0, Normal = 1, Hard = 2 };

struct Settings {
    float masterVolume = 0.8f;
    float musicVolume = 0.6f;
    float sfxVolume = 0.9f;
    bool fullscreen = false;
    Difficulty difficulty = Difficulty::Normal;
};

class Game {
public:
    explicit Game(const GameData& data);

    void update(float dt);
    void submitInput(const GameInput& in);

    GameState state() const { return state_; }
    Simulation& sim() { return sim_; }
    const Simulation& sim() const { return sim_; }
    const GameData& data() const { return data_; }

    int score() const { return score_; }
    int highScore() const;
    const std::vector<int>& highScores() const { return highScores_; }
    void loadHighScores(std::vector<int> scores);

    Settings& settings() { return settings_; }
    void loadSettings(Settings s) { settings_ = s; }

    bool debugOverlay = false;

    int menuSelection() const { return menuSel_; }
    int stageIndex() const { return stageIndex_; }
    std::string stageName() const;
    float stateTimer() const { return stateTimer_; }

    bool popEvent(GameEvent& out);

    // Headless smoke-testing hook (used with WWII_SMOKE env).
    void debugStart() { startGame(); }

private:
    void enterState(GameState s);
    void startGame();
    void startStage();
    void finishStage();
    void gameOver();
    void processSimEvents();
    static DifficultyMods modsFor(Difficulty d);
    bool pressed(bool now, bool& prev);

    const GameData& data_;
    Simulation sim_;
    GameState state_ = GameState::Title;
    float stateTimer_ = 0.0f;
    int menuSel_ = 0;
    int stageIndex_ = 0;
    int score_ = 0;
    std::vector<int> highScores_;
    Settings settings_;

    GameInput input_{};
    GameInput prevInput_{};
    std::vector<GameEvent> events_;

    // Playing-state bookkeeping.
    float gameOverDelay_ = 0.0f;
    float stageCompleteDelay_ = 0.0f;
    bool bossDefeated_ = false;
};

}  // namespace ww
