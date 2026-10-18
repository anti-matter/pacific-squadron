#include "app/Application.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "audio/ProceduralSfx.h"
#include "core/MathUtil.h"
#include "core/Rng.h"
#include "graphics/ProceduralArt.h"

namespace ww {

namespace {

constexpr float kTile = 32.0f;

std::uint32_t hashXY(int x, int y) {
    std::uint32_t h = 0x9E3779B9u;
    h ^= static_cast<std::uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<std::uint32_t>(y) * 0xC2B2AE35u;
    h = (h << 15) | (h >> 17);
    return h;
}

const char* oceanFor(std::uint32_t pick) {
    static const char* names[] = {"tile_ocean_a", "tile_ocean_b", "tile_ocean_c"};
    return names[pick % 3];
}

}  // namespace

// ---------------------------------------------------------------------------
// Init / resources
// ---------------------------------------------------------------------------
Application::Application() = default;
Application::~Application() = default;

bool Application::init() {
    // Locate the asset directory: cwd/assets, then <exe>/assets.
    namespace fs = std::filesystem;
    if (fs::exists("assets/data")) {
        assetDir_ = "assets";
    } else {
        const char* base = SDL_GetBasePath();
        const fs::path p = fs::path(base ? base : ".") / "assets";
        if (fs::exists(p / "data")) assetDir_ = p.string();
        SDL_free(const_cast<char*>(base));
    }
    if (assetDir_.empty()) {
        SDL_Log("Could not find an assets/ directory next to the executable or cwd.");
        assetDir_ = "assets";  // let data loading produce a clear error
    }
    const char* pref = SDL_GetPrefPath("pacific-squadron", "pacific-squadron");
    prefDir_ = pref ? pref : ".";
    // First launch after the wwii-fighter rename: carry over saves/settings
    // from the old preference folder (copied, never moved, so nothing is lost).
    if (pref != nullptr) {
        if (const char* oldPref = SDL_GetPrefPath("wwii-fighter", "wwii-fighter")) {
            std::error_code ec;
            if (fs::is_directory(oldPref, ec))
                for (const auto& e : fs::directory_iterator(oldPref, ec))
                    if (e.is_regular_file()) {
                        const auto dst = fs::path(prefDir_) / e.path().filename();
                        if (!fs::exists(dst, ec)) fs::copy_file(e.path(), dst, ec);
                    }
            SDL_free(const_cast<char*>(oldPref));
        }
        SDL_free(const_cast<char*>(pref));
    }

    if (!renderer_.init("Pacific Squadron — a WWII arcade shooter", false))
        return false;
    input_.init();
    if (!audio_.init())
        SDL_Log("Audio unavailable; continuing silently.");

    // Data.
    std::string err;
    if (!data_.loadAll(assetDir_, err)) {
        SDL_Log("Data load error: %s", err.c_str());
        return false;
    }

    // Art. (Text needs no atlas setup: the native-resolution overlay bakes
    // stroke-font atlases on demand at the exact display scale.)
    generateAllArt(renderer_);
    loadPngOverrides(renderer_, assetDir_);

    // Audio buffers.
    std::unordered_map<std::string, SoundBuffer> sfx;
    generateAllSfx(sfx);
    for (auto& [name, buf] : sfx) audio_.registerBuffer(name, std::move(buf));

    game_ = std::make_unique<Game>(data_);
    loadSettings();
    loadHighScores();
    input_.loadBindings((prefPath("input.json")).string());

    // WWII_START_STAGE=<n> (1-based): begin a playthrough at stage n. In
    // normal mode this skips the title; with WWII_SMOKE the harness starts
    // at that stage instead of stage 1.
    int startStageIdx = -1;
    if (const char* lv = SDL_getenv("WWII_START_STAGE"))
        startStageIdx = SDL_atoi(lv) - 1;

    // Headless smoke test hooks (see cmake smoke script).
    if (const char* s = SDL_getenv("WWII_SMOKE")) {
        smokeTime_ = SDL_atof(s);
        smokeBoss_ = SDL_getenv("WWII_SMOKE_BOSS") != nullptr;
        if (const char* bi = SDL_getenv("WWII_SMOKE_BOSS"))
            smokeBossIdx_ = SDL_atoi(bi) - 1;  // ordinal: 1 -> first boss, 2 -> second
        if (smokeBossIdx_ < 0) smokeBossIdx_ = 0;
        if (const char* shot = SDL_getenv("WWII_SMOKE_SHOT")) {
            smokeShotPath_ = shot;
            smokeShotAt_ = SDL_getenv("WWII_SMOKE_SHOT_AT")
                ? SDL_atof(SDL_getenv("WWII_SMOKE_SHOT_AT")) : 10.0f;
        }
        if (!SDL_getenv("WWII_SMOKE_NOAUTO")) {
            if (startStageIdx >= 0) game_->debugStartAtStage(startStageIdx);
            else game_->debugStart();
            game_->sim().setPlayerIntent({0, 0}, true, false);
            if (SDL_getenv("WWII_SMOKE_GOD"))
                game_->sim().setGodMode(true);
        }
        if (SDL_getenv("WWII_SMOKE_HELP"))
            game_->debugSetState(GameState::Help);
        // Debug: jump straight to a named menu state (Help/Options/HighScores).
        if (const char* st = SDL_getenv("WWII_SMOKE_STATE")) {
            if (!std::strcmp(st, "Options"))
                game_->debugSetState(GameState::Options);
            else if (!std::strcmp(st, "Help"))
                game_->debugSetState(GameState::Help);
            else if (!std::strcmp(st, "HighScores"))
                game_->debugSetState(GameState::HighScores);
        }
    } else if (startStageIdx >= 0) {
        game_->debugStartAtStage(startStageIdx);
    }
    // Debug HUD on from the environment (F3 still toggles in-game).
    if (SDL_getenv("WWII_DEBUG"))
        game_->debugOverlay = true;

    audio_.setMasterVolume(game_->settings().masterVolume);
    audio_.setMusicVolume(game_->settings().musicVolume);
    audio_.setSfxVolume(game_->settings().sfxVolume);
    renderer_.setFullscreen(game_->settings().fullscreen);

    running_ = true;
    return true;
}

std::filesystem::path Application::prefPath(const char* file) const {
    return std::filesystem::path(prefDir_) / file;
}

bool Application::loadSettings() {
    std::ifstream in(prefPath("settings.json"), std::ios::binary);
    if (!in) return false;
    nlohmann::json j;
    try { in >> j; } catch (...) { return false; }
    Settings& s = game_->settings();
    if (j.contains("master")) s.masterVolume = j["master"].get<float>();
    if (j.contains("music")) s.musicVolume = j["music"].get<float>();
    if (j.contains("sfx")) s.sfxVolume = j["sfx"].get<float>();
    if (j.contains("fullscreen")) s.fullscreen = j["fullscreen"].get<bool>();
    if (j.contains("difficulty")) s.difficulty = static_cast<Difficulty>(j["difficulty"].get<int>());
    return true;
}

void Application::saveSettings() {
    const Settings& s = game_->settings();
    nlohmann::json j = {{"master", s.masterVolume},
                        {"music", s.musicVolume},
                        {"sfx", s.sfxVolume},
                        {"fullscreen", s.fullscreen},
                        {"difficulty", static_cast<int>(s.difficulty)}};
    std::ofstream out(prefPath("settings.json"), std::ios::binary);
    if (out) out << j.dump(2);
}

void Application::loadHighScores() {
    std::ifstream in(prefPath("highscores.json"), std::ios::binary);
    if (!in) return;
    nlohmann::json j;
    try { in >> j; } catch (...) { return; }
    std::vector<HighScoreEntry> table;
    if (j.is_array()) {
        for (const auto& e : j) {
            if (e.is_number_integer()) {  // legacy format: bare scores
                HighScoreEntry h;
                h.initials = "---";
                h.score = e.get<int>();
                table.push_back(std::move(h));
            } else if (e.is_object() && e.contains("score") &&
                       e["score"].is_number_integer()) {
                HighScoreEntry h;
                if (e.contains("initials") && e["initials"].is_string())
                    h.initials = e["initials"].get<std::string>();
                h.score = e["score"].get<int>();
                table.push_back(std::move(h));
            }
        }
    }
    if (!table.empty()) game_->loadHighScores(std::move(table));
}

void Application::saveHighScores() {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& e : game_->highScores())
        j.push_back({{"initials", e.initials}, {"score", e.score}});
    std::ofstream out(prefPath("highscores.json"), std::ios::binary);
    if (out) out << j.dump(2) << "\n";
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
int Application::run() {
    Uint64 prevTicks = SDL_GetTicks();
    while (running_) {
        const Uint64 loopStart = SDL_GetTicks();
        // --- events -----------------------------------------------------
        SDL_Event ev;
        input_.newFrame();
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running_ = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // Window-manager close button / Alt+F4.
                running_ = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                // Initials entry intercepts letters/backspace before the
                // action layer (B is also the Back action, so letters must
                // not reach it on this screen).
                if (game_->state() == GameState::EnterInitials &&
                    !ev.key.repeat) {
                    const SDL_Keycode kc = ev.key.key;
                    if (kc >= SDLK_A && kc <= SDLK_Z) {
                        game_->onTextInput(
                            static_cast<char>('A' + (kc - SDLK_A)));
                        break;
                    }
                    if (kc == SDLK_BACKSPACE) {
                        game_->onInitialsBackspace();
                        break;
                    }
                }
                if (SDL_getenv("WWII_KEYLOG"))
                    SDL_Log("[keylog] down keycode=%u", (unsigned)ev.key.key);
                input_.onKey(static_cast<int>(ev.key.key), true);
                break;
            case SDL_EVENT_KEY_UP:
                input_.onKey(static_cast<int>(ev.key.key), false);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                input_.onGamepadButton(static_cast<int>(ev.gbutton.button), true);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                input_.onGamepadButton(static_cast<int>(ev.gbutton.button), false);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                input_.onGamepadAxis(static_cast<int>(ev.gaxis.axis),
                                     static_cast<float>(ev.gaxis.value));
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                // No final zeroing events arrive; drop all latched state so
                // the plane never keeps flying after the pad vanishes.
                input_.forgetDevices();
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                // Open pads connected after launch (or woken from a BT
                // suspend): SDL emits no gamepad events for unopened devices.
                input_.onDeviceAdded(static_cast<int>(ev.gdevice.which));
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                input_.forgetDevices();
                break;
            default:
                break;
            }
        }

        // --- fixed-step update -------------------------------------------
        const Uint64 now = SDL_GetTicks();
        float frameDt = static_cast<float>(now - prevTicks) / 1000.0f;
        prevTicks = now;
        if (frameDt > 0.0f) fps_ = fps_ * 0.95f + (1.0f / frameDt) * 0.05f;
        frameDt = std::min(frameDt, 0.1f);
        acc_ += frameDt;
        int steps = 0;
        while (acc_ >= kSimDt && steps < 4) {
            GameInput gi = buildGameInput();
            if (smokeTime_ > 0.0f) {
                // Autopilot: hold fire, weave, occasionally loop.
                gi.fire = true;
                gi.moveX = std::sin(smokeElapsed_ * 1.3f);
                gi.moveY = std::sin(smokeElapsed_ * 0.7f) * 0.5f;
            }
            game_->submitInput(gi);
            game_->update(kSimDt);
            acc_ -= kSimDt;
            ++steps;
            if (smokeTime_ > 0.0f) {
                smokeElapsed_ += kSimDt;
                if (smokeBoss_ && !smokeBossSpawned_ && smokeElapsed_ > 2.0f) {
                    smokeBossSpawned_ = true;
                    if (smokeBossIdx_ < static_cast<int>(data_.bosses.size()))
                        game_->sim().debugSpawnBoss(smokeBossIdx_);
                }
                if (smokeShotAt_ > 0.0f && !smokeShotTaken_ &&
                    smokeElapsed_ >= smokeShotAt_) {
                    smokeShotTaken_ = true;
                    renderer_.requestScreenshot(smokeShotPath_);
                }
                if (smokeElapsed_ >= smokeTime_) running_ = false;
            }
        }
        if (steps == 4) acc_ = 0.0f;  // spiral-of-death guard

        consumeGameEvents();
        updateMusic();
        audio_.pump();

        renderFrame(fps_);
        // Explicit frame pacing: some compositors (notably Xwayland) do not
        // block in Present even with vsync enabled, which would otherwise
        // leave the loop spinning near 100% CPU. On hosts where vsync does
        // block, this is a no-op because the frame already took >= 16 ms.
        const Uint64 spentMs = SDL_GetTicks() - loopStart;
        if (spentMs < 16)
            SDL_Delay(static_cast<Uint32>(16 - spentMs));
        if (SDL_getenv("WWII_KEYLOG")) { static int fc=0; if ((fc++ % 240)==0) SDL_Log("[perf] frame %d fps=%.0f", fc, fps_); }
    }
    input_.saveBindings(prefPath("input.json").string());
    return 0;
}

GameInput Application::buildGameInput() const {
    GameInput gi;
    gi.moveX = input_.axisMoveX();
    gi.moveY = input_.axisMoveY();
    const float ax = input_.padStickX, ay = input_.padStickY;
    gi.moveX = clamp(gi.moveX + ax, -1.0f, 1.0f);
    gi.moveY = clamp(gi.moveY + ay, -1.0f, 1.0f);
    gi.fire = input_.held(Action::Fire);
    gi.special = input_.pressedThisFrame(Action::Special);
    gi.up = input_.pressedThisFrame(Action::MoveUp);
    gi.down = input_.pressedThisFrame(Action::MoveDown);
    gi.left = input_.pressedThisFrame(Action::MoveLeft);
    gi.right = input_.pressedThisFrame(Action::MoveRight);
    gi.confirm = input_.pressedThisFrame(Action::Confirm);
    gi.back = input_.pressedThisFrame(Action::Back);
    gi.pause = input_.pressedThisFrame(Action::Pause);
    gi.debugToggle = input_.pressedThisFrame(Action::Debug);
    return gi;
}

// Explosion classes resolve to a random timbre variant (independent synth
// seeds), a pitch jitter of +-10% and a stereo pan derived from the world
// x position, so repeated detonations never sound copy-pasted. Everything
// else plays by name as before.
void Application::playGameSound(const std::string& name, Vec2 pos) {
    struct BoomClass { const char* base; int variants; float volume; };
    static constexpr BoomClass kBooms[] = {
        {"explosion_small", 3, 0.85f},
        {"explosion_medium", 3, 0.95f},
        {"explosion_large", 3, 1.00f},
        {"explosion_boss", 2, 1.00f},
        {"boss_breakup", 2, 0.60f},  // continuous rumble bed under the finale
    };
    for (const BoomClass& k : kBooms) {
        if (name == k.base) {
            const int v = 1 + boomRng_.rangeInt(0, k.variants - 1);
            // Unanchored events carry pos {0,0}: keep those centred.
            const float pan = (pos.x == 0.0f && pos.y == 0.0f)
                                  ? 0.0f
                                  : clamp((pos.x - 192.0f) / 192.0f, -1.0f, 1.0f) * 0.55f;
            const float pitch = 1.0f + boomRng_.range(-0.10f, 0.10f);
            audio_.play(std::string(k.base) + "_" + std::to_string(v),
                        k.volume, true, pan, pitch);
            return;
        }
    }
    audio_.play(name, 1.0f, name != "music");
}

void Application::consumeGameEvents() {
    GameEvent ev;
    while (game_->popEvent(ev)) {
        switch (ev.type) {
        case GameEvent::Type::Sound:
            playGameSound(ev.name, ev.pos);
            break;
        case GameEvent::Type::SaveHighScores:
            saveHighScores();
            break;
        case GameEvent::Type::SaveSettings:
            saveSettings();
            audio_.setMasterVolume(game_->settings().masterVolume);
            audio_.setMusicVolume(game_->settings().musicVolume);
            audio_.setSfxVolume(game_->settings().sfxVolume);
            renderer_.setFullscreen(game_->settings().fullscreen);
            break;
        case GameEvent::Type::Quit:
            running_ = false;
            break;
        }
    }
}

void Application::updateMusic() {
    if (!audio_.available()) return;
    std::string want;
    switch (game_->state()) {
    case GameState::Title:
    case GameState::Options:
    case GameState::HighScores:
    case GameState::Help:
    case GameState::EnterInitials:
        want = "music_title";
        break;
    case GameState::StageIntro:
    case GameState::Playing:
    case GameState::Paused:
    case GameState::StageComplete:
        want = game_->stageIndex() == 0 ? "music_stage1" : "music_stage2";
        break;
    case GameState::GameOver:
    case GameState::Victory:
        want = "music_title";
        break;
    }
    if (want != currentMusic_) {
        currentMusic_ = want;
        audio_.playMusic(want);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void Application::renderTerrain(const StageDef* stage, float scrollY,
                                float time) {
    const int cols = 12;
    const int wavePhase = static_cast<int>(time * 1.4f);
    // Tile choice for a plain (non-shore) terrain kind.
    auto tileForKind = [&](TerrainKind kind, std::uint32_t h, int col) -> const char* {
        switch (kind) {
        case TerrainKind::Ocean:    return oceanFor(h + static_cast<std::uint32_t>(wavePhase));
        case TerrainKind::Island:   return (h % 5 == 0) ? "tile_sand"
                                         : (h % 2 == 0) ? "tile_jungle_a" : "tile_jungle_b";
        case TerrainKind::Jungle:   return (h % 2 == 0) ? "tile_jungle_a" : "tile_jungle_b";
        case TerrainKind::Road:     return (h % 6 < 3) ? "tile_road_v"
                                         : (h % 3 == 0) ? "tile_sand" : "tile_jungle_a";
        // Airfield: two centre lanes are real runway (continuous centreline
        // dashes); the rest is poured apron concrete with sand weathering.
        case TerrainKind::Airfield: return (col == 5 || col == 6)
                                         ? "tile_runway"
                                         : (h % 5 == 0) ? "tile_sand" : "tile_airfield";
        case TerrainKind::Fortress: return (h % 7 == 0) ? "tile_sand" : "tile_fortress";
        case TerrainKind::Shore:    return "tile_sand";  // handled below
        }
        return "tile_ocean_a";
    };
    // Smooth (sub-tile) scrolling: iterate WORLD tile rows and compute each
    // row's screen y from the exact scroll offset, instead of snapping rows
    // to fixed 32px screen slots (which popped one tile at a time).
    const float base = scrollY + kPlayH;   // screenY = base - worldY
    const int rowFirst = static_cast<int>(std::floor(scrollY / kTile)) - 1;
    const int rowLast = static_cast<int>(std::floor(base / kTile)) + 1;
    for (int row = rowFirst; row <= rowLast; ++row) {
        const float tileCenterY = base - (static_cast<float>(row) + 0.5f) * kTile;
        const float worldY = (static_cast<float>(row) + 0.5f) * kTile;
        const TerrainKind kind = stage ? terrainAt(*stage, worldY)
                                       : TerrainKind::Ocean;
        for (int i = 0; i < cols; ++i) {
            const std::uint32_t h = hashXY(i, row);
            const char* texName = "tile_ocean_a";
            if (kind == TerrainKind::Shore) {
                // A shore band must contain exactly ONE coastline:
                //   sea -> land: foam transition row, one beach row, then
                //                the land tiles.
                //   land -> sea: one beach row, foam transition row, then
                //                ocean.
                // (Tiling the transition art across the whole band stacked
                //  multiple coastlines per shore.)
                const auto& segs = stage->terrain;
                std::size_t idx = segs.size() - 1;
                for (std::size_t s = 0; s < segs.size(); ++s)
                    if (worldY >= segs[s].fromY) idx = s;
                const TerrainKind prevK =
                    idx > 0 ? segs[idx - 1].kind : TerrainKind::Ocean;
                const TerrainKind nextK = idx + 1 < segs.size()
                    ? segs[idx + 1].kind : TerrainKind::Ocean;
                // Band edges are not tile-aligned, so count rows from the
                // boundaries themselves: at most one screen row can lie
                // within kTile of a boundary (row references step by kTile),
                // guaranteeing a single waterline row per coastline.
                const float upperY =
                    idx + 1 < segs.size() ? segs[idx + 1].fromY
                                          : segs[idx].toY;
                const float dUpper = upperY - worldY;   // >0 inside band
                const float dLower = worldY - segs[idx].fromY;  // >=0
                const bool prevSea = prevK == TerrainKind::Ocean;
                const bool nextSea = nextK == TerrainKind::Ocean;
                // NOTE: worldY grows toward the TOP of the screen (the
                // forward direction). "prev" terrain is behind/below,
                // "next" terrain is ahead/above.
                if (prevSea && !nextSea) {
                    // Approaching land: water at the bottom of the screen.
                    if (dLower < kTile) texName = "tile_shore_in";
                    else if (dLower < 2.0f * kTile) texName = "tile_sand";
                    else texName = tileForKind(nextK, h, i);
                } else if (!prevSea && nextSea) {
                    // Leaving land: water appears at the top of the screen.
                    if (dUpper < kTile) texName = "tile_shore";
                    else if (dUpper < 2.0f * kTile) texName = "tile_sand";
                    else texName = tileForKind(prevK, h, i);
                } else {
                    // Land-to-land (or sea-to-sea) shore band: a dry sand
                    // strip, no waterline.
                    texName = "tile_sand";
                }
            } else {
                texName = tileForKind(kind, h, i);
            }
            const int tid = renderer_.texture(texName);
            renderer_.drawRegion(tid, 0, 0, 32, 32,
                                 static_cast<float>(i) * kTile + 16.0f,
                                 tileCenterY);
        }
    }
}

void Application::renderWorld() {
    const auto& sim = game_->sim();
    const StageDef* stage = sim.director().stageDef();
    const float scrollY = sim.director().scrollY();
    const float time = sim.director().time();

    // Decay presentation timers (clamped dt tolerates per-stage time resets).
    const float dt = clamp(time - lastWorldTime_, 0.0f, 0.1f);
    lastWorldTime_ = time;
    flashT_ = std::max(0.0f, flashT_ - dt);
    armorFlashT_ = std::max(0.0f, armorFlashT_ - dt);

    renderTerrain(stage, scrollY, time);

    auto findAnim = [&](const char* name) -> const AnimDef* {
        const int i = data_.findAnim(name);
        return i >= 0 ? &data_.anims[static_cast<std::size_t>(i)] : nullptr;
    };
    const AnimDef* exSmall = findAnim("explosion_small");
    const AnimDef* exMed = findAnim("explosion_medium");
    const AnimDef* exBig = findAnim("explosion_large");
    const AnimDef* exXl = findAnim("explosion_xlarge");
    const AnimDef* exBoss = findAnim("boss_explosion");
    const AnimDef* muzzleAd = findAnim("muzzle_flash");
    const int muzzleTex = renderer_.texture("muzzle_flash");

    const int shadowTex = renderer_.texture("shadow_ell");

    // Pass 1: ground/sea units with shadows.
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind == EKind::Enemy && e.enemyDef >= 0) {
            const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
            if (!def.ground && !def.ship) continue;
            if (def.ship) renderer_.draw(shadowTex, e.pos.x + 5.0f, e.pos.y + 7.0f);
            const Color tint = e.flashTimer > 0.0f ? Color::rgb(0xFF9090)
                                                   : Color::rgb(0xFFFFFF);
            renderer_.draw(renderer_.texture(def.sprite), e.pos.x, e.pos.y, 1.0f,
                           tint);
        }
    }

    // Pass 2: power-ups (bobbing).
    for (const auto& e : sim.entities()) {
        if (!e.alive || e.kind != EKind::Powerup) continue;
        const PowerupDef& def = data_.powerups[static_cast<std::size_t>(e.powerupDef)];
        const float bob = 2.0f * std::sin(e.age * 6.0f);
        const float alpha = e.lifeLeft < 2.0f
            ? (std::fmod(e.lifeLeft * 6.0f, 1.0f) < 0.5f ? 0.35f : 1.0f)
            : 1.0f;
        renderer_.draw(renderer_.texture(def.sprite), e.pos.x, e.pos.y + bob, alpha);
    }

    // Pass 3: boss hull & components (sprites carried on the entities).
    int compAlive = 0;
    bool hullAlive = false;
    for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind != EKind::BossHull && e.kind != EKind::BossComponent) continue;
        const Color tint = e.flashTimer > 0.0f ? Color::rgb(0xFF9090)
                                               : Color::rgb(0xFFFFFF);
        const bool hull = e.kind == EKind::BossHull;
        if (hull) hullAlive = true; else ++compAlive;
        const float frac = e.maxHp > 0
            ? static_cast<float>(e.hp) / static_cast<float>(e.maxHp) : 1.0f;
        std::string texName = e.sprite;
        if (hull) {
            // Damage-state sprites (present only for bosses that define them).
            // Thresholds are generous because a stripped hull dies quickly:
            // each state must be on screen for a readable beat.
            if (frac < 0.35f && renderer_.texture(e.sprite + "_wreck") >= 0)
                texName = e.sprite + "_wreck";
            else if (frac < 0.70f && renderer_.texture(e.sprite + "_dmg") >= 0)
                texName = e.sprite + "_dmg";
            else if (frac < 0.85f && renderer_.texture(e.sprite + "_light") >= 0)
                texName = e.sprite + "_light";
            // Wake foam trails behind the battleship's stern (screen-up).
            if (texName.find("battleship") != std::string::npos)
                renderer_.draw(renderer_.texture("wake_foam"),
                               e.pos.x + std::sin(time * 2.0f) * 2.0f,
                               e.pos.y - 140.0f);
        }
        renderer_.draw(renderer_.texture(texName), e.pos.x, e.pos.y, 1.0f, tint);
        // Armor-break pulse: white silhouette flashes over the hull for a
        // beat after the LAST destructible mount dies (hull now vulnerable).
        if (hull && armorFlashT_ > 0.0f) {
            const int flash = renderer_.texture(e.sprite + "_flash");
            if (flash >= 0) {
                const float p = armorFlashT_ / 0.85f;
                const float a = 0.75f * p * (0.45f + 0.55f * std::sin(time * 24.0f));
                if (a > 0.01f)
                    renderer_.draw(flash, e.pos.x, e.pos.y, a);
            }
        }
        // Burning / smoking hull once the armor is broken.
        if (hull && frac < 0.70f) {
            const int fire = renderer_.texture("fire_puff");
            const int smoke = renderer_.texture("smoke_puff");
            const int n = frac < 0.35f ? 3 : 2;
            for (int k = 0; k < n; ++k) {
                const float fx = e.pos.x + static_cast<float>((k * 53) % 61) - 30.0f;
                const float fy = e.pos.y + static_cast<float>((k * 37) % 41) - 20.0f;
                const float fl = 0.75f + 0.35f * std::sin(time * 13.0f + k * 2.1f);
                renderer_.drawScaled(fire, fx, fy, 14.0f * fl, 16.0f * fl);
                const float rise = std::fmod(time * 22.0f + k * 31.0f, 62.0f);
                renderer_.drawScaled(smoke, fx - rise * 0.15f, fy - 10.0f - rise,
                                     12.0f + rise * 0.35f, 12.0f + rise * 0.35f,
                                     1.0f - rise / 70.0f);
            }
        }
        // Sky Fortress: trailing engine smoke so damage reads per nacelle
        // line rather than as generic hull fire.
        if (hull && texName.find("bomber") != std::string::npos && frac < 0.60f) {
            const int smoke = renderer_.texture("smoke_puff");
            const int n = frac < 0.30f ? 4 : 2;
            const float nx[4] = {-46.0f, 46.0f, -68.0f, 68.0f};
            for (int k = 0; k < n; ++k) {
                const float rise = std::fmod(time * 26.0f + k * 17.0f, 56.0f);
                renderer_.drawScaled(smoke, e.pos.x + nx[k],
                                     e.pos.y + 12.0f - rise * 0.8f,
                                     9.0f + rise * 0.3f, 9.0f + rise * 0.3f,
                                     0.8f - rise / 72.0f);
            }
        }
        // Wounded components sputter: small flame + smoke so the player can
        // read which mounts are about to die.
        if (!hull && frac < 0.5f) {
            const int fire = renderer_.texture("fire_puff");
            const int smoke = renderer_.texture("smoke_puff");
            const float fl = 0.7f + 0.3f * std::sin(time * 15.0f + e.pos.x);
            renderer_.drawScaled(fire, e.pos.x + 3.0f, e.pos.y - 3.0f,
                                 9.0f * fl, 10.0f * fl);
            const float rise = std::fmod(time * 26.0f + e.pos.y, 34.0f);
            renderer_.drawScaled(smoke, e.pos.x - rise * 0.1f, e.pos.y - 8.0f - rise,
                                 8.0f + rise * 0.3f, 8.0f + rise * 0.3f,
                                 0.9f - rise / 40.0f);
        }
    }
    // Armored again? No: once the last mount dies, flash the hull once.
    if (hullAlive && prevCompAlive_ > 0 && compAlive == 0)
        armorFlashT_ = 0.85f;
    prevCompAlive_ = hullAlive ? compAlive : 0;

    // Pass 4: enemy aircraft with shadows.
    for (const auto& e : sim.entities()) {
        if (!e.alive || e.kind != EKind::Enemy || e.enemyDef < 0) continue;
        const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
        if (def.ground || def.ship) continue;
        renderer_.draw(shadowTex, e.pos.x + 4.0f, e.pos.y + 8.0f);
        const Color tint = e.flashTimer > 0.0f ? Color::rgb(0xFF9090)
                                               : Color::rgb(0xFFFFFF);
        renderer_.draw(renderer_.texture(def.sprite), e.pos.x, e.pos.y, 1.0f, tint);
    }

    // Pass 5: player.
    {
        const auto p = sim.player();
        if (p.alive) {
            const bool blink = p.invulnerable &&
                               std::fmod(time * 12.0f, 1.0f) < 0.5f;
            float alpha = blink ? 0.4f : 1.0f;
            const char* tex = "player_fighter";
            if (p.bank < -0.25f) tex = "player_fighter_l";
            else if (p.bank > 0.25f) tex = "player_fighter_r";
            const int ptex = renderer_.texture(tex);
            renderer_.draw(shadowTex, p.pos.x + 6.0f, p.pos.y + 14.0f);
            if (p.specialTimer > 0.0f) {
                // Aerobatic loop: vertical squash as the plane barrels.
                const float spin = (0.9f - p.specialTimer) / 0.9f;
                const float squash =
                    std::abs(std::sin(spin * 3.14159265f * 4.0f));
                renderer_.drawScaled(ptex, p.pos.x, p.pos.y, 40.0f,
                                     8.0f + 32.0f * squash, alpha);
            } else {
                renderer_.draw(ptex, p.pos.x, p.pos.y, alpha);
            }
        }
    }

    // Pass 6: projectiles.
    for (const auto& e : sim.entities()) {
        if (!e.alive || e.kind != EKind::Shot) continue;
        const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
        renderer_.draw(renderer_.texture(w.sprite), e.pos.x, e.pos.y, 1.0f,
                       Color::rgb(0xFFFFFF), false,
                       e.rotates ? angleOf(e.vel) * 57.29578f + 90.0f : 0.0f);
        // Brief muzzle flash at the gun while the shell is still fresh.
        if (muzzleAd && muzzleTex >= 0 && e.team == Team::Enemy && e.age < 0.10f) {
            const int frame = clamp(static_cast<int>(e.age * muzzleAd->fps), 0,
                                    muzzleAd->frames - 1);
            renderer_.drawRegion(muzzleTex, frame * muzzleAd->frameW, 0,
                                 muzzleAd->frameW, muzzleAd->frameH,
                                 e.pos.x, e.pos.y - 4.0f);
        }
    }

    // Pass 7: particles.
    for (const auto& pt : sim.particles()) {
        const std::uint32_t c = pt.color;
        const int a = static_cast<int>((c & 0xFF) *
                                       clamp(pt.life / pt.maxLife, 0.0f, 1.0f));
        const Color pcol{static_cast<std::uint8_t>((c >> 24) & 0xFF),
                         static_cast<std::uint8_t>((c >> 16) & 0xFF),
                         static_cast<std::uint8_t>((c >> 8) & 0xFF),
                         static_cast<std::uint8_t>(a)};
        renderer_.fillRect(pt.pos.x - pt.size * 0.5f, pt.pos.y - pt.size * 0.5f,
                           pt.size, pt.size, pcol);
    }

    // Pass 8: explosions. Texture and AnimDef are chosen together so the
    // frame geometry always matches the sheet being sliced.
    const AnimDef* exSplashS = findAnim("splash_small");
    const AnimDef* exSplashL = findAnim("splash_large");
    int bossBooms = 0;
    for (const auto& ex : sim.explosions()) {
        if (ex.t < 0.0f) continue;   // fuse delay for staggered finales
        const char* texName = "";
        const AnimDef* ad = nullptr;
        if (ex.splash) {
            if (ex.sizeClass <= 1) { texName = "splash_small"; ad = exSplashS; }
            else                   { texName = "splash_large"; ad = exSplashL; }
        } else if (ex.sizeClass == 0) { texName = "explosion_small";  ad = exSmall; }
        else if (ex.sizeClass == 1)   { texName = "explosion_medium"; ad = exMed; }
        else if (ex.sizeClass == 3)   { texName = "explosion_xlarge"; ad = exXl; }
        else if (ex.sizeClass == 4)   { texName = "boss_explosion";   ad = exBoss; }
        else                          { texName = "explosion_large";  ad = exBig; }
        if (ex.sizeClass == 4) {
            ++bossBooms;
            // Smoke-hook: negative WWII_SMOKE_SHOT_AT captures N seconds
            // after the detonation core appears (afterglow shot).
            if (smokeShotAt_ < 0.0f && !smokeShotTaken_ && !smokeShotPath_.empty() &&
                ex.t >= -smokeShotAt_) {
                smokeShotTaken_ = true;
                renderer_.requestScreenshot(smokeShotPath_);
            }
        }
        if (!ad) continue;
        const int tid = renderer_.texture(texName);
        const int frame = clamp(static_cast<int>(ex.t * ad->fps), 0, ad->frames - 1);
        renderer_.drawRegion(tid, frame * ad->frameW, 0, ad->frameW, ad->frameH,
                             ex.pos.x, ex.pos.y);
    }
    // Boss detonation: new class-4 blast => full-playfield white flash.
    if (bossBooms > lastBossBoom_) flashT_ = 0.45f;
    lastBossBoom_ = bossBooms;
    if (flashT_ > 0.0f) {
        const float a = clamp(flashT_ / 0.45f, 0.0f, 1.0f);
        renderer_.fillRect(0.0f, 0.0f, kPlayW, kPlayH,
                           Color{255, 255, 255,
                                 static_cast<std::uint8_t>(235.0f * a *
                                                           std::sqrt(a))});
    }
}

void Application::renderTitleBackground(float time) {
    renderTerrain(nullptr, time * 40.0f, time);
    // Beauty shot: the battleship flagship idles at the bottom of the
    // screen, half cropped by the frame edge, slowly swaying.
    const int hull = renderer_.texture("boss_battleship_hull");
    if (hull >= 0) {
        const float bx = 192.0f + std::sin(time * 0.12f) * 30.0f;
        const float by = 486.0f + std::sin(time * 0.5f) * 3.0f;
        const int wake = renderer_.texture("wake_foam");
        if (wake >= 0)
            renderer_.draw(wake, bx + std::sin(time * 2.0f) * 2.0f, by - 140.0f,
                           0.45f, Color::rgb(0x50687C));
        renderer_.draw(hull, bx, by, 1.0f, Color::rgb(0xD0D8E0));
    }
    // Decorative flyovers.
    const int en = renderer_.texture("enemy_fighter_light");
    for (int i = 0; i < 3; ++i) {
        const float t = std::fmod(time * 0.25f + i * 0.33f, 1.4f) - 0.2f;
        const float x = 192.0f + std::sin(t * 6.2832f * 1.5f + i) * 130.0f;
        const float y = -40.0f + t * 620.0f;
        renderer_.draw(en, x, y, 0.9f, Color::rgb(0xC8C8FF));
    }
}

void Application::renderTitle() {
    const float t = game_->stateTimer();
    renderTitleBackground(t);
    dimOverlay();
    const Color gold = Color::rgb(0xFFD860);
    const Color white = Color::rgb(0xF0F4F8);
    // The text renderer carries its own drop shadow now: one draw each.
    renderer_.text("PACIFIC", 192.0f, 88.0f, 4, gold, true);
    renderer_.text("SQUADRON", 192.0f, 128.0f, 4, white, true);
    renderer_.text("A WORLD WAR II ARCADE SHOOTER", 192.0f, 172.0f, 1,
                   Color::rgb(0x9AB0C8), true);

    const char* items[] = {"START MISSION", "HIGH SCORES", "HELP", "OPTIONS",
                           "QUIT"};
    for (int i = 0; i < 5; ++i) {
        const bool sel = game_->menuSelection() == i;
        // Pad both sides so centering is exact; one-sided padding used to
        // shift unselected items right.
        const std::string s = (sel ? "> " : "  ") + std::string(items[i]) +
                              (sel ? " <" : "  ");
        renderer_.text(s, 192.0f, 296.0f + i * 28.0f, 2,
                       sel ? gold : white, true);
    }
    if (std::fmod(t * 2.0f, 1.0f) < 0.6f)
        renderer_.text("WASD/ARROWS MOVE   SPACE FIRE   SHIFT LOOP",
                       192.0f, 466.0f, 1, Color::rgb(0x7A90A8), true);
    renderer_.text("ESC PAUSE   GAMEPAD SUPPORTED", 192.0f, 482.0f, 1,
                   Color::rgb(0x50687E), true);
}

void Application::renderStageIntro() {
    renderer_.text("STAGE", 192.0f, 200.0f, 2, Color::rgb(0x9AB0C8), true);
    // Auto-fit long stage names ("SOLO OVER THE CORAL SEA" at scale 3 is
    // wider than the 384px playfield) so they never run off either edge.
    const std::string name = game_->stageName();
    int scale = 3;
    while (scale > 1 && renderer_.textWidth(name, scale) > 368) --scale;
    renderer_.text(name, 192.0f, scale == 3 ? 230.0f : 234.0f, scale,
                   Color::rgb(0xFFD860), true);
    renderer_.text("SCRAMBLE!", 192.0f, 300.0f, 2, Color::rgb(0xF0F4F8), true);
}

void Application::renderPaused() {
    renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 150});
    renderer_.text("PAUSED", 192.0f, 230.0f, 3, Color::rgb(0xFFD860), true);
    renderer_.text("ESC/ENTER RESUME   BACK QUITS TO TITLE", 192.0f, 280.0f, 1,
                   Color::rgb(0x9AB0C8), true);
}

void Application::renderStageComplete() {
    renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 120});
    renderer_.text("STAGE CLEAR", 192.0f, 210.0f, 3, Color::rgb(0x7AE87A), true);
    const std::string s = "SCORE " + std::to_string(game_->score());
    renderer_.text(s, 192.0f, 270.0f, 2, Color::rgb(0xF0F4F8), true);
}

void Application::renderGameOver() {
    renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 150});
    renderer_.text("GAME OVER", 192.0f, 220.0f, 3, Color::rgb(0xE05050), true);
    const std::string s = "SCORE " + std::to_string(game_->score());
    renderer_.text(s, 192.0f, 280.0f, 2, Color::rgb(0xF0F4F8), true);
    if (std::fmod(game_->stateTimer() * 2.0f, 1.0f) < 0.6f)
        renderer_.text("PRESS ENTER", 192.0f, 330.0f, 1,
                       Color::rgb(0x9AB0C8), true);
}

void Application::renderVictory() {
    renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 140});
    renderer_.text("MISSION", 192.0f, 180.0f, 4, Color::rgb(0xFFD860), true);
    renderer_.text("COMPLETE", 192.0f, 220.0f, 4, Color::rgb(0x7AE87A), true);
    const std::string s = "FINAL SCORE " + std::to_string(game_->score());
    renderer_.text(s, 192.0f, 300.0f, 2, Color::rgb(0xF0F4F8), true);
}

void Application::renderHighScores() {
    renderer_.text("HIGH SCORES", 192.0f, 100.0f, 3, Color::rgb(0xFFD860), true);
    const auto& hs = game_->highScores();
    for (std::size_t i = 0; i < hs.size() && i < 8; ++i) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%d.  %s  %07d", static_cast<int>(i) + 1,
                      hs[i].initials.c_str(), hs[i].score);
        renderer_.text(buf, 192.0f, 160.0f + static_cast<float>(i) * 28.0f, 2,
                       i == 0 ? Color::rgb(0xFFD860) : Color::rgb(0xD8E0E8),
                       true);
    }
    renderer_.text("BACK: ESC / B", 192.0f, 470.0f, 1, Color::rgb(0x9AB0C8),
                   true);
}

void Application::renderOptions() {
    renderer_.text("OPTIONS", 192.0f, 80.0f, 3, Color::rgb(0xFFD860), true);
    const Settings& s = game_->settings();
    const char* diffNames[] = {"EASY", "NORMAL", "HARD"};
    char vals[6][24];
    std::snprintf(vals[0], 24, "%s",
                  diffNames[std::min(2, static_cast<int>(s.difficulty))]);
    std::snprintf(vals[1], 24, "%d%%",
                  static_cast<int>(s.masterVolume * 100.0f + 0.5f));
    std::snprintf(vals[2], 24, "%d%%",
                  static_cast<int>(s.musicVolume * 100.0f + 0.5f));
    std::snprintf(vals[3], 24, "%d%%",
                  static_cast<int>(s.sfxVolume * 100.0f + 0.5f));
    std::snprintf(vals[4], 24, "%s", s.fullscreen ? "ON" : "OFF");
    vals[5][0] = '\0';
    const char* labels[] = {"DIFFICULTY", "MASTER VOL", "MUSIC VOL", "SFX VOL",
                            "FULLSCREEN", "BACK"};
    // Fixed columns (label left, value left) so every row lines up; the
    // old single-string-per-row layout drifted as value lengths varied.
    constexpr float kLabelX = 64.0f, kValueX = 240.0f, kSelL = 40.0f,
                    kSelR = 344.0f;
    for (int i = 0; i < 6; ++i) {
        const bool sel = game_->menuSelection() == i;
        const Color c = sel ? Color::rgb(0xFFD860) : Color::rgb(0xD8E0E8);
        const float y = 150.0f + static_cast<float>(i) * 32.0f;
        renderer_.text(labels[i], kLabelX, y, 2, c, false);
        if (vals[i][0]) renderer_.text(vals[i], kValueX, y, 2, c, false);
        if (sel) {
            renderer_.text(">", kSelL, y, 2, c, false);
            renderer_.text("<", kSelR, y, 2, c, false);
        }
    }
    renderer_.text("UP/DOWN SELECT   LEFT/RIGHT ADJUST", 192.0f, 420.0f, 1,
                   Color::rgb(0x9AB0C8), true);
}

void Application::renderHelp() {
    const Color gold = Color::rgb(0xFFD860);
    const Color white = Color::rgb(0xD8E0E8);
    const Color dim = Color::rgb(0x9AB0C8);
    renderer_.text("HELP", 192.0f, 36.0f, 3, gold, true);
    renderer_.text("A 2D ARCADE SHOOTER OVER THE PACIFIC.", 192.0f, 86.0f, 1,
                   white, true);
    renderer_.text("FLY FOUR MISSIONS, DOWN THE ENEMY FLEET AND", 192.0f, 102.0f,
                   1, white, true);
    renderer_.text("SINK ITS ARMED FLAGSHIPS TO WIN.", 192.0f, 118.0f, 1,
                   white, true);

    renderer_.text("CONTROLS", 192.0f, 152.0f, 2, gold, true);
    // Controls table: three true columns at fixed x positions (key column
    // right-aligned, action + pad columns left-aligned) instead of
    // space-padded center-aligned strings, whose columns drifted with the
    // new font metrics.
    struct ControlRow {
        const char* key;
        const char* act;
        const char* pad;
    };
    const ControlRow rows[] = {
        {"WASD / ARROWS", "MOVE", "STICK"},
        {"SPACE", "FIRE", "PAD A / RB"},
        {"SHIFT / CTRL", "LOOP", "PAD B / LB"},
        {"ESC / P", "PAUSE", "PAD START"},
    };
    constexpr float kKeyEndX = 152.0f, kActX = 168.0f, kPadX = 252.0f;
    for (int i = 0; i < 4; ++i) {
        const float y = 180.0f + static_cast<float>(i) * 16.0f;
        renderer_.text(rows[i].key, kKeyEndX - renderer_.textWidth(rows[i].key, 1),
                       y, 1, white, false);
        renderer_.text(rows[i].act, kActX, y, 1, white, false);
        renderer_.text(rows[i].pad, kPadX, y, 1, white, false);
    }
    renderer_.text("PICKED-UP GUNS REPLACE YOUR GUN", 192.0f, 252.0f, 1,
                   dim, true);
    renderer_.text("AND FIRE WITH SPACE. LOOP CLEARS ALL", 192.0f, 266.0f,
                   1, dim, true);
    renderer_.text("ENEMY FIRE AND MAKES YOU INVINCIBLE", 192.0f, 280.0f, 1,
                   dim, true);

    renderer_.text("SUPPLY DROPS", 192.0f, 304.0f, 2, gold, true);
    // Legend shows the actual pickup sprites next to each description
    // (icons are center-anchored, descriptions start at a constant x so
    // the columns line up vertically).
    struct LegendRow {
        const char* sprite;
        const char* desc;
    };
    const LegendRow leftCol[] = {
        {"powerup_p", "GUN UPGRADE"},
        {"powerup_spread", "SPREAD GUN"},
        {"powerup_rocket", "ROCKETS"},
        {"powerup_repair", "REPAIR 1 HULL"},
    };
    const LegendRow rightCol[] = {
        {"powerup_cannon", "HEAVY CANNON"},
        {"powerup_bomb", "+1 LOOP CHARGE"},
        {"powerup_life", "EXTRA LIFE"},
    };
    constexpr float kIconX1 = 74.0f, kDescX1 = 94.0f;
    constexpr float kIconX2 = 218.0f, kDescX2 = 238.0f;
    for (int i = 0; i < 4; ++i) {
        const float y = 342.0f + static_cast<float>(i) * 20.0f;
        const int tex = renderer_.texture(leftCol[i].sprite);
        if (tex >= 0) renderer_.draw(tex, kIconX1, y);
        renderer_.text(leftCol[i].desc, kDescX1, y, 1, white, false);
    }
    for (int i = 0; i < 3; ++i) {
        const float y = 342.0f + static_cast<float>(i) * 20.0f;
        const int tex = renderer_.texture(rightCol[i].sprite);
        if (tex >= 0) renderer_.draw(tex, kIconX2, y);
        renderer_.text(rightCol[i].desc, kDescX2, y, 1, white, false);
    }

    renderer_.text("ESC / B: BACK", 192.0f, 470.0f, 1,
                   Color::rgb(0x7A90A8), true);
}

void Application::renderEnterInitials() {
    renderer_.text("NEW HIGH SCORE!", 192.0f, 150.0f, 3, Color::rgb(0xFFD860),
                   true);
    const std::string sc =
        "SCORE " + std::to_string(game_->pendingScore());
    renderer_.text(sc, 192.0f, 205.0f, 2, Color::rgb(0xF0F4F8), true);
    renderer_.text("ENTER INITIALS", 192.0f, 250.0f, 2,
                   Color::rgb(0x9AB0C8), true);
    // Letters with a blinking cursor on the active slot.
    const int len = game_->initialsLength();
    const std::string entry = game_->initialsEntry();
    for (int i = 0; i < 3; ++i) {
        const bool filled = i < len;
        const float x = 144.0f + static_cast<float>(i) * 48.0f;
        const std::string g(1, filled ? entry[static_cast<std::size_t>(i)]
                                      : '-');
        const Color col = filled ? Color::rgb(0xFFD860) : Color::rgb(0x3A5068);
        renderer_.text(g, x, 290.0f, 3, col, true);
        if (i == len && std::fmod(game_->stateTimer() * 2.5f, 1.0f) < 0.6f)
            renderer_.text("_", x, 316.0f, 3, Color::rgb(0xF0F4F8), true);
    }
    renderer_.text("TYPE A-Z   BACKSPACE TO FIX   ESC TO SKIP", 192.0f, 360.0f,
                   1, Color::rgb(0x7A90A8), true);
}

void Application::dimOverlay() {
    renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 110});
}

void Application::renderFrame(float fps) {
    renderer_.beginFrame(Color::rgb(0x06101C));
    const auto state = game_->state();
    if (state == GameState::Title) {
        renderTitle();
    } else if (state == GameState::Options) {
        renderTitleBackground(game_->stateTimer());
        dimOverlay();
        renderOptions();
    } else if (state == GameState::HighScores) {
        renderTitleBackground(game_->stateTimer());
        dimOverlay();
        renderHighScores();
    } else if (state == GameState::Help) {
        renderTitleBackground(game_->stateTimer());
        dimOverlay();
        renderHelp();
    } else if (state == GameState::EnterInitials) {
        renderTitleBackground(game_->stateTimer());
        dimOverlay();
        renderEnterInitials();
    } else {
        renderWorld();
        if (state == GameState::Playing || state == GameState::Paused ||
            state == GameState::StageComplete)
            hud_.draw(renderer_, *game_);
        if (state == GameState::StageIntro) {
            renderer_.fillRect(0, 0, kPlayW, kPlayH, Color{0, 0, 0, 120});
            renderStageIntro();
        }
        if (state == GameState::Paused) renderPaused();
        if (state == GameState::StageComplete) renderStageComplete();
        if (state == GameState::GameOver) {
            hud_.draw(renderer_, *game_);
            renderGameOver();
        }
        if (state == GameState::Victory) renderVictory();
        if (game_->debugOverlay) hud_.drawDebug(renderer_, *game_, fps);
    }
    renderer_.endFrame();
}

void Application::shutdown() {
    audio_.shutdown();
    input_.shutdown();
    renderer_.shutdown();
}

}  // namespace ww
