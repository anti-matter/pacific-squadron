#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "audio/Audio.h"
#include "game/Game.h"
#include "graphics/Renderer.h"
#include "input/Input.h"
#include "ui/Hud.h"

namespace ww {

// Owns all infrastructure (window, renderer, audio, input) and runs the
// fixed-timestep game loop. Gameplay lives in Game/Simulation; this class
// only wires systems together and draws frames.
class Application {
public:
    Application();
    ~Application();
    bool init();
    int run();
    void shutdown();

private:
    GameInput buildGameInput() const;
    void consumeGameEvents();
    void updateMusic();

    void renderFrame(float fps);
    void dimOverlay();
    void renderTerrain(const StageDef* stage, float scrollY, float time);
    void renderWorld();
    void renderTitleBackground(float time);
    void renderTitle();
    void renderStageIntro();
    void renderPaused();
    void renderStageComplete();
    void renderGameOver();
    void renderVictory();
    void renderHighScores();
    void renderOptions();

    bool loadSettings();
    void saveSettings();
    void loadHighScores();
    void saveHighScores();
    std::filesystem::path prefPath(const char* file) const;

    Renderer renderer_;
    Input input_;
    Audio audio_;
    GameData data_;
    std::unique_ptr<Game> game_;
    Hud hud_;

    std::string assetDir_;
    std::string prefDir_;
    bool running_ = false;
    float acc_ = 0.0f;
    float fps_ = 60.0f;
    std::string currentMusic_;
    float smokeTime_ = 0.0f;
    float smokeElapsed_ = 0.0f;
    bool smokeBoss_ = false;
    bool smokeBossSpawned_ = false;
    float smokeShotAt_ = 0.0f;
    bool smokeShotTaken_ = false;
    std::string smokeShotPath_;
};

}  // namespace ww
