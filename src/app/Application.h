#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio/Audio.h"
#include "core/Rng.h"
#include "game/Game.h"
#include "graphics/Renderer.h"
#include "graphics/Environment.h"
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
    void renderTerrain(const StageDef* stage, float scrollY);
    void renderWorld();
    // Animated bow-wave V + stern wake for the seagoing boss ships (battleship,
    // yamato, cruiser, destroyer, carrier). Presentation-only: draws under the
    // hull sprite before it, using the wall-clock `time` like the ocean glint.
    void renderShipWake(const std::string& hullSprite, float x, float y, float t);
    void renderTitleBackground(float time);
    void renderTitle();
    void renderStageIntro();
    void renderPaused();
    void renderStageComplete();
    void renderGameOver();
    void renderVictory();
    void renderHighScores();
    void renderHelp();
    void renderHelpCatalog(int page);
    void renderEnterInitials();
    void renderOptions();

    bool loadSettings();
    void saveSettings();
    void loadHighScores();
    void saveHighScores();
    std::filesystem::path prefPath(const char* file) const;
    // Loading splash: presented between window creation and the first game
    // frame so slow probes/bakes never look like a hung launch. Draws with
    // the game's own font/palette and shows phase-labelled progress. With
    // the splash artwork loaded it fills the window edge-to-edge, keeps
    // showing after loading completes, and waits for any key to dismiss.
    void showSplash(float progress, const char* label);
    // Modal gate after loading: animates the "press any key" prompt until a
    // key/click/button arrives (or the window is closed), then restores the
    // game canvas and a gameplay-sized window.
    void waitForSplashDismiss();

    Renderer renderer_;
    EnvironmentBaker env_;  // composed land terrain chunks (see Environment.h)
    Input input_;
    Audio audio_;
    GameData data_;
    std::unique_ptr<Game> game_;
    Hud hud_;

    std::string assetDir_;
    std::string prefDir_;
    // Splash artwork (assets/splash.png): texture, logical canvas height the
    // artwork was fit to, and the loaded flag gating the "press any key"
    // prompt. splashTex_ < 0 keeps the procedural fallback splash.
    int splashTex_ = -1;
    float splashH_ = 0.0f;
    bool splashDone_ = false;
    bool running_ = false;
    // Audio stays silent until the first real frame has been presented, so
    // the player never hears the title theme from behind an unopened window.
    bool windowShown_ = false;
    float acc_ = 0.0f;
    float fps_ = 60.0f;
    std::string currentMusic_;
    float smokeTime_ = 0.0f;
    float smokeElapsed_ = 0.0f;
    bool smokeBoss_ = false;
    bool smokeNoAuto_ = false;  // idle manual player (no autopilot fire/weave)
    bool smokePopTest_ = false;  // WWII_SMOKE_POP: inject edge-case popups
    bool smokePopDone_ = false;  // injection fires exactly once
    int smokeBossIdx_ = 0;
    // Boss presentation timers.
    float lastFlashClock_ = 0.0f;  // wall clock for flash decay (stage clock
    float flashT_ = 0.0f;        // white screen flash on boss detonation / bomb
    int lastBombCount_ = -1;     // bomb charge watcher (presentation)
    static constexpr int kBombCraterMax = 6;
    int bombCraters_ = 0;        // bomb scars on the airfield (presentation)
    std::vector<float> smokeBombAt_;  // WWII_SMOKE_BOMB_AT: sim times to press bomb
    int smokeBombSent_ = 0;
    float smokeKillBossAt_ = 0.0f;  // WWII_SMOKE_KILL_BOSS_AT: sim time to force boss death
    float armorFlashT_ = 0.0f;   // armor-break silhouette pulse
    int lastBossBoom_ = 0;
    // Floating score awards (whole-formation wipes): hold kPopHold, then
    // fade out over kPopFade. Presentation-only, advanced on the wall clock.
    // pos is always clamped to the playfield (clampPopPos), so an award for
    // a kill at (or beyond) a screen edge still reads fully on screen.
    struct ScorePop {
        Vec2 pos;
        std::string text;
        float age = 0.0f;
    };
    static constexpr float kPopHold = 2.0f;
    static constexpr float kPopFade = 0.5f;
    static constexpr float kPopScale = 1.125f;
    std::vector<ScorePop> scorePops_;
    // Clamp a pop anchor so the whole string stays inside the playfield:
    // x is the string centre, y the glyph top (top margin clears the HUD).
    Vec2 clampPopPos(const ScorePop& pop) const;
    Rng boomRng_{0x800D5u};  // variant/pitch/pan variation for explosions
    int prevCompAlive_ = 0;
    bool smokeBossSpawned_ = false;
    float smokeShotAt_ = 0.0f;
    bool smokeShotTaken_ = false;
    std::string smokeShotPath_;
};

}  // namespace ww
