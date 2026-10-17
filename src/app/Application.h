#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "audio/Audio.h"
#include "core/Rng.h"
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
    // Routes a game sound event to the audio layer: explosion classes are
    // resolved to a random timbre variant with pan (from world x) and
    // playback-pitch jitter so repeats never sound identical.
    void playGameSound(const std::string& name, Vec2 pos);
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
    void renderHelp();
    void renderEnterInitials();
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
    int smokeBossIdx_ = 0;
    // Boss presentation timers.
    float flashT_ = 0.0f;        // white screen flash on boss detonation
    float armorFlashT_ = 0.0f;   // armor-break silhouette pulse
    int lastBossBoom_ = 0;
    Rng boomRng_{0x800D5u};  // variant/pitch/pan variation for explosions
    int prevCompAlive_ = 0;
    float lastWorldTime_ = 0.0f;
    bool smokeBossSpawned_ = false;
    float smokeShotAt_ = 0.0f;
    bool smokeShotTaken_ = false;
    std::string smokeShotPath_;
};

}  // namespace ww
