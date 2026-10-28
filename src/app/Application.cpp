#include "app/Application.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <signal.h>  // sigaction (POSIX only; SDL maps console events on Windows)
#endif

#include <nlohmann/json.hpp>

#include "audio/ProceduralSfx.h"
#include "core/MathUtil.h"
#include "core/Rng.h"
#include "graphics/ProceduralArt.h"

// Release version shown on the splash, generated from the project VERSION in
// CMakeLists.txt (see cmake/version.h.in). The fallback keeps a standalone
// compile (no CMake configure) building; it should never be edited by hand.
#if __has_include("version.h")
#include "version.h"
#else
#define WW_VERSION_STRING "0.0.0"
#endif

namespace ww {

namespace {
// "Slightly smaller" than the scale-1 subtitle: 0.8x, which the native-res
// stroke font rasterizes crisply. The string is 13 glyphs -> ~98 logical px,
// so it can never reach the 384px edges.
constexpr float kVersionScale = 0.8f;
const std::string kVersionLine =
    std::string("VERSION ") + WW_VERSION_STRING;

// Set by the SIGINT/SIGTERM handler so every wait loop can bail out. SDL's own
// handler (which turns those signals into SDL_EVENT_QUIT) is only installed
// when the disposition is still SIG_DFL, and a backgrounded shell starts the
// game with SIGINT already ignored -- so a polite `kill`/`timeout` would be
// swallowed and the process would spin forever. std::atomic<int> is
// lock-free and async-signal-safe, which is all the handler may touch.
std::atomic<int> gTerminateSignal{0};

void handleTerminateSignal(int sig) {
    gTerminateSignal.store(sig, std::memory_order_relaxed);
}

// Installs handleTerminateSignal for SIGINT/SIGTERM (and SIGHUP, so closing the
// controlling terminal also stops a headless run). Replaces whatever
// disposition was inherited -- including SIG_IGN, which is exactly the case
// that made SDL skip its own handler.
void installTerminateHandlers() {
#ifndef _WIN32
    struct sigaction sa {};
    sa.sa_handler = handleTerminateSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: let blocking calls return EINTR
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
#endif
}

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
    // Locate the asset directory. cwd/assets comes first (dev runs from the
    // repo root), then everything derived from the executable so a Finder /
    // `open` / Dock launch -- which starts with cwd "/" -- still finds its
    // data. On a macOS bundle SDL_GetBasePath() is Contents/Resources, NOT
    // Contents/MacOS, so both bundle layouts are probed.
    namespace fs = std::filesystem;
    if (fs::exists("assets/data")) {
        assetDir_ = "assets";
    } else {
        const char* base = SDL_GetBasePath();
        const fs::path exeDir = fs::path(base ? base : ".");
        SDL_free(const_cast<char*>(base));
        const fs::path candidates[] = {
            exeDir / "assets",
            exeDir / ".." / "Resources" / "assets",  // macOS bundle (preferred)
            exeDir / ".." / "MacOS" / "assets",      // macOS bundle (legacy)
            exeDir / ".." / "share" / "pacific-squadron" / "assets",
        };
        for (const fs::path& p : candidates) {
            std::error_code ec;
            if (fs::exists(p / "data", ec)) {
                assetDir_ = p.lexically_normal().string();
                break;
            }
        }
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

#ifdef NDEBUG
    auto phase = [](const char*) {};
#else
    const Uint64 t0 = SDL_GetTicks();
    Uint64 tPrev = t0;
    // Startup phase report: if a machine loads slowly, the log names the
    // phase (device probe, art, synth, ...) without needing a profiler.
    auto phase = [&tPrev](const char* what) {
        const Uint64 now = SDL_GetTicks();
        SDL_Log("[startup] %-22s %6.1f s", what, (now - tPrev) / 1000.0);
        tPrev = now;
    };
#endif

    if (!renderer_.init("Pacific Squadron — a WWII arcade shooter", false))
        return false;
    phase("renderer init");
    // After SDL_Init on purpose: SDL installs its own SIGINT/SIGTERM handler
    // during init only if the disposition is still SIG_DFL, and would otherwise
    // be the sole path to a clean shutdown. Ours is unconditional.
    installTerminateHandlers();

    // Window/taskbar icon from the generated PNG pack (tools/make_icons.py).
    // Best-effort: on macOS the dock uses the app bundle's .icns instead, and
    // a missing pack simply leaves the default icon in place.
    renderer_.setWindowIconFromDir(
        (std::filesystem::path(assetDir_) / "icons").string());

    // Splash first: as soon as a window exists, make it visible. Everything
    // below can take seconds on slower machines (gamepad and audio device
    // probes, art baking, music synthesis) and the player must never stare
    // at an unopened window while that happens. Vsync stays off until the
    // loop starts so splash progress presents never block on a refresh.
    renderer_.setVSync(false);

    // Splash artwork: fill the window edge-to-edge at the poster's aspect
    // ratio (logical canvas 384 wide, height derived from the image). The
    // splash window is capped to the game's original window footprint
    // (and to the display on smaller screens), so it never grows the app.
    if (renderer_.createTextureFromPng(
            "splash_screen",
            (std::filesystem::path(assetDir_) / "splash.png").string())) {
        splashTex_ = renderer_.texture("splash_screen");
        int iw = 0, ih = 0;
        renderer_.textureSize(splashTex_, iw, ih);
        if (splashTex_ >= 0 && iw > 0 && ih > 0) {
            splashH_ = 384.0f * static_cast<float>(ih) /
                       static_cast<float>(iw);
            int dw = 0, dh = 0, bw = 0, bh = 0;
            renderer_.displayBounds(dw, dh);
            renderer_.baseWindowSize(bw, bh);
            const float fit = std::min(std::min(dw * 0.94f / 384.0f,
                                                dh * 0.94f / splashH_),
                                       std::min(bw * 1.0f / 384.0f,
                                                bh * 1.0f / splashH_));
            renderer_.setSplashCanvas(true, 384,
                                      static_cast<int>(std::lround(splashH_)));
            renderer_.setWindowSize(static_cast<int>(std::lround(384.0f * fit)),
                                    static_cast<int>(std::lround(splashH_ * fit)));
        } else {
            splashTex_ = -1;
        }
    }
    showSplash(0.02f, "STARTING UP");

    // Data.
    std::string err;
    if (!data_.loadAll(assetDir_, err)) {
        SDL_Log("Data load error: %s", err.c_str());
        return false;
    }
    // A missing/renamed asset tree makes every category load zero entries,
    // which loadAll reports as success -- the game then boots into an empty
    // ocean with no enemies, weapons or stages and no clue why. Fail loudly
    // with the directory it looked in instead.
    if (data_.stages.empty() || data_.enemies.empty() || data_.weapons.empty()) {
        const char* what = data_.stages.empty() ? "stages"
                         : (data_.enemies.empty() ? "enemies" : "weapons");
        SDL_Log("Loaded no %s from '%s' -- the asset tree is missing or corrupt.",
                what, assetDir_.c_str());
        // A double-clicked bundle has no terminal to read that log in, so say
        // it in a dialog too. Skipped for headless/smoke runs, where a modal
        // box would hang an automated run.
        const bool headless = SDL_getenv("WWII_SMOKE") != nullptr ||
                              (SDL_GetCurrentVideoDriver() &&
                               SDL_strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0);
        if (!headless) {
            const std::string msg =
                "No " + std::string(what) + " were loaded from:\n" + assetDir_ +
                "\n\nThe game's data files are missing or the installation is "
                "incomplete.";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Pacific Squadron",
                                     msg.c_str(), nullptr);
        }
        return false;
    }
    game_ = std::make_unique<Game>(data_);
    loadSettings();
    loadHighScores();
    if (game_->settings().fullscreen) renderer_.setFullscreen(true);
    phase("data load");
    showSplash(0.10f, "CHARTING THE PACIFIC");

    input_.init();
    input_.loadBindings((prefPath("input.json")).string());
    phase("input init");

    // Art. (Text needs no atlas setup: the native-resolution overlay bakes
    // stroke-font atlases on demand at the exact display scale.) The disk
    // cache uploads the pre-baked texture set on later launches, skipping
    // the rasterizer entirely; WWII_NOCACHE=1 forces a rebuild.
    std::string artCache, sfxCache;
    if (!SDL_getenv("WWII_NOCACHE")) {
        artCache = (std::filesystem::path(prefDir_) / "art_cache.bin").string();
        sfxCache = (std::filesystem::path(prefDir_) / "sfx_cache.bin").string();
    }
    generateAllArt(renderer_, artCache, [this](float p) {
        showSplash(0.12f + 0.60f * p, "ASSEMBLING THE SQUADRON");
    });
    loadPngOverrides(renderer_, assetDir_);
    env_.init(renderer_);  // pooled chunk textures for composed land
    phase("art");

    // Audio buffers: synthesized once, then cached like the art. The audio
    // device itself opens last of the heavy probes, and stays silent until
    // the first real frame is presented (see windowShown_).
    std::unordered_map<std::string, SoundBuffer> sfx;
    generateAllSfx(sfx, sfxCache, [this](float p) {
        showSplash(0.74f + 0.20f * p, "TUNING THE ENGINES");
    });
    phase("sfx synth");
    if (!audio_.init())
        SDL_Log("Audio unavailable; continuing silently.");
    for (auto& [name, buf] : sfx) audio_.registerBuffer(name, std::move(buf));
    phase("audio init");
#ifndef NDEBUG
    SDL_Log("[startup] TOTAL since renderer   %6.1f s",
            (SDL_GetTicks() - t0) / 1000.0);
#endif

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
        // WWII_SMOKE_BOMB_AT=<t1,t2,...>: press the bomb at those sim times
        // (useful for verifying bomb effects headlessly).
        if (const char* bombAt = SDL_getenv("WWII_SMOKE_BOMB_AT")) {
            const char* c = bombAt;
            while (*c != '\0') {
                char* end = nullptr;
                const float v = std::strtof(c, &end);
                if (end != c) smokeBombAt_.push_back(v);
                if (end == c) ++c;  // skip garbage instead of spinning
                else c = (*end == ',') ? end + 1 : end;
            }
        }
        if (startStageIdx >= 0) game_->debugStartAtStage(startStageIdx);
        else game_->debugStart();
        if (SDL_getenv("WWII_SMOKE_GOD"))
            game_->sim().setGodMode(true);
        if (!SDL_getenv("WWII_SMOKE_NOAUTO")) {
            game_->sim().setPlayerIntent({0, 0}, true, false);
            smokeNoAuto_ = false;
        } else {
            smokeNoAuto_ = true;  // idle manual player: no auto-fire, no weave
        }
        if (SDL_getenv("WWII_SMOKE_HELP"))
            game_->debugSetState(GameState::Help);
        // Debug: jump straight to a named menu state (Help/Options/HighScores).
        if (const char* st = SDL_getenv("WWII_SMOKE_STATE")) {
            if (!std::strcmp(st, "Options"))
                game_->debugSetState(GameState::Options);
            else if (!std::strcmp(st, "Help"))
                game_->debugSetState(GameState::Help);
            else if (!std::strncmp(st, "Help", 4) && st[4] >= '1' && st[4] <= '9') {
                game_->debugSetState(GameState::Help);
                game_->debugSetHelpPage(st[4] - '0' - 1);  // "Help2" -> page 1
            } else if (!std::strcmp(st, "HighScores"))
                game_->debugSetState(GameState::HighScores);
        }
    } else if (startStageIdx >= 0) {
        game_->debugStartAtStage(startStageIdx);
    }
    // WWII_BOSS_NOW[=<delay s>] (debug, works in normal mode): skip the
    // build-up and trigger the CURRENT stage's boss a moment after the intro
    // card, e.g. WWII_START_STAGE=5 WWII_BOSS_NOW jumps straight to the
    // Coastal Fortress. Unlike the smoke-only WWII_SMOKE_BOSS (which spawns
    // a boss definition by index), this uses the stage's own boss, so the
    // fight, bar and escort behaviour are exactly as scripted. The override
    // applies to this stage only; later stages use their scripted bossTime.
    if (const char* bn = SDL_getenv("WWII_BOSS_NOW")) {
        float delay = bn[0] ? SDL_atof(bn) : 1.0f;
        if (delay < 0.0f) delay = 0.0f;
        game_->sim().debugSetBossTime(delay);
    }
    // Debug HUD on from the environment (F3 still toggles in-game).
    if (SDL_getenv("WWII_DEBUG"))
        game_->debugOverlay = true;

    audio_.setMasterVolume(game_->settings().masterVolume);
    audio_.setMusicVolume(game_->settings().musicVolume);
    audio_.setSfxVolume(game_->settings().sfxVolume);

    splashDone_ = true;
    if (SDL_getenv("WWII_SMOKE")) {
        // WWII_SMOKE_SPLASH_SHOT=<path>: grab the finished splash (prompt
        // visible) before handing over to gameplay.
        if (const char* sp = SDL_getenv("WWII_SMOKE_SPLASH_SHOT"))
            renderer_.requestScreenshot(sp);
        showSplash(1.0f, "READY");
    } else {
        waitForSplashDismiss();
    }
    // Back to the game presentation: nearest-neighbour 384x512 canvas in
    // the game's original window size. (The smoke path skips the dismiss
    // gate entirely, so the restore lives here rather than inside
    // waitForSplashDismiss. If the user closed the splash window, running_
    // is already false and run() simply no-ops.)
    renderer_.setSplashCanvas(false, 0, 0);
    renderer_.restoreWindowSize();
    renderer_.setVSync(true);

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
// Loading splash
//
// With assets/splash.png present the artwork fills the splash window
// edge-to-edge and the title, progress bar, phase label and a pulsing
// "PRESS ANY KEY" prompt are superimposed over it. Without the artwork the
// fallback draws with the game's own palette, stroke font and pixel
// language: night sea with tile-tone glints, the title stack, the player
// fighter once its texture exists, and a progress bar whose label names the
// current phase (also a live diagnostic: the last label shown pinpoints a
// slow probe).
// ---------------------------------------------------------------------------
void Application::showSplash(float progress, const char* label) {
    if (splashTex_ >= 0) {
        // ---- artwork splash --------------------------------------------
        renderer_.beginFrame(Color::rgb(0x05080C));
        renderer_.drawScaled(splashTex_, 192.0f, splashH_ * 0.5f, 384.0f,
                             splashH_);
        // Title over the bright sky band at the top; the overlay's built-in
        // drop shadow keeps it legible over cloud. Scale 3 would be 381 of
        // 384 logical px wide - the glyph ink and shadow then clip at the
        // window edges - so auto-fit with a safety margin.
        static constexpr const char* kTitle = "PACIFIC SQUADRON";
        int ts = 3;
        while (ts > 1 && renderer_.textWidth(kTitle, ts) > 348) --ts;
        renderer_.text(kTitle, 192.0f, 32.0f, ts, Color::rgb(0xFFD860), true);
        const float subY = 32.0f + 11.0f * ts + 7.0f;
        renderer_.text("A WORLD WAR II ARCADE SHOOTER", 192.0f, subY, 1,
                       Color::rgb(0xF0F6FC), true);
        renderer_.text(kVersionLine, 192.0f, subY + 15.0f, kVersionScale,
                       Color::rgb(0xB0C4D8), true);
        // Progress over the dark sea band at the bottom.
        constexpr float kBarW = 232.0f;
        const float bx = 192.0f - kBarW * 0.5f;
        const float by = splashH_ - 52.0f;
        renderer_.fillRect(bx - 1.0f, by - 1.0f, kBarW + 2.0f, 14.0f,
                           Color{5, 10, 16, 170});
        renderer_.outlineRect(bx, by, kBarW, 12.0f, Color::rgb(0x3A5068));
        const float w = (kBarW - 4.0f) * clamp(progress, 0.0f, 1.0f);
        if (w >= 1.0f)
            renderer_.fillRect(bx + 2.0f, by + 2.0f, w, 8.0f,
                               Color::rgb(0xFFD860));
        renderer_.text(label, 192.0f, by + 17.0f, 1, Color::rgb(0xC8D8E8),
                       true);
        if (splashDone_) {
            // Loading finished: the dismissal prompt breathes between a dim
            // slate and full white (~1.1 Hz) to draw the eye.
            const float t = static_cast<float>(SDL_GetTicks()) / 1000.0f;
            const float b = 0.5f + 0.5f * std::sin(t * 6.2832f * 1.1f);
            const auto v = static_cast<std::uint8_t>(110.0f + 145.0f * b);
            renderer_.text("PRESS ANY KEY", 192.0f, by - 22.0f, 2,
                           Color{v, v, v, 255}, true);
        }
        renderer_.endFrame();
        return;
    }

    // ---- procedural fallback -------------------------------------------
    renderer_.beginFrame(Color::rgb(0x06101C));  // night sea, as in-game

    // Static glints: sparse dashes in the ocean tiles' dark tone.
    for (int row = 0; row < 25; ++row) {
        const float y = 14.0f + static_cast<float>(row) * 20.0f;
        for (int col = 0; col < 7; ++col) {
            const float x = std::fmod(static_cast<float>(col) * 53.0f +
                                          static_cast<float>(row) * 31.0f,
                                      372.0f);
            renderer_.fillRect(x, y, 6.0f, 1.0f, Color::rgb(0x0E2438));
        }
    }

    renderer_.text("PACIFIC", 192.0f, 150.0f, 4, Color::rgb(0xFFD860), true);
    renderer_.text("SQUADRON", 192.0f, 190.0f, 4, Color::rgb(0xF0F4F8), true);
    renderer_.text("A WORLD WAR II ARCADE SHOOTER", 192.0f, 232.0f, 1,
                   Color::rgb(0x9AB0C8), true);
    renderer_.text(kVersionLine, 192.0f, 247.0f, kVersionScale,
                   Color::rgb(0x7A90A8), true);

    // The player's fighter appears as soon as the art pass has created it.
    const int plane = renderer_.texture("player_fighter");
    if (plane >= 0) renderer_.draw(plane, 192.0f, 286.0f);

    constexpr float kBarW = 232.0f;
    const float bx = 192.0f - kBarW * 0.5f, by = 340.0f;
    renderer_.outlineRect(bx, by, kBarW, 12.0f, Color::rgb(0x3A5068));
    const float w = (kBarW - 4.0f) * clamp(progress, 0.0f, 1.0f);
    if (w >= 1.0f)
        renderer_.fillRect(bx + 2.0f, by + 2.0f, w, 8.0f, Color::rgb(0xFFD860));
    renderer_.text(label, 192.0f, 366.0f, 1, Color::rgb(0x7A90A8), true);
    if (splashDone_) {
        const float t = static_cast<float>(SDL_GetTicks()) / 1000.0f;
        const float b = 0.5f + 0.5f * std::sin(t * 6.2832f * 1.1f);
        const auto v = static_cast<std::uint8_t>(110.0f + 145.0f * b);
        renderer_.text("PRESS ANY KEY", 192.0f, 400.0f, 2, Color{v, v, v, 255},
                       true);
    }
    renderer_.endFrame();
}

// ---------------------------------------------------------------------------
// Splash dismissal gate. Presents the finished splash (with the pulsing
// prompt) until any key, mouse click or gamepad button arrives. The headless
// smoke harness skips it — there is nobody at the keyboard.
// ---------------------------------------------------------------------------
void Application::waitForSplashDismiss() {
    renderer_.setVSync(true);
    bool dismiss = false;
    while (!dismiss) {
        // A pending SIGINT/SIGTERM must end the wait: nobody is at the keyboard
        // in a headless run, and this gate would otherwise hold the process.
        if (gTerminateSignal.load(std::memory_order_relaxed) != 0) {
            running_ = false;
            return;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running_ = false;
                return;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN ||
                ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                dismiss = true;
        }
        if (dismiss) break;
        showSplash(1.0f, "READY");
        SDL_Delay(1);
    }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
int Application::run() {
    Uint64 prevTicks = SDL_GetTicks();
    while (running_) {
        // Same reason as the splash gate: the signal handler is the one
        // shutdown path that is guaranteed to fire regardless of how the
        // process was started.
        if (gTerminateSignal.load(std::memory_order_relaxed) != 0) running_ = false;
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
            if (smokeTime_ > 0.0f && !smokeNoAuto_) {
                // Autopilot: hold fire, weave, occasionally loop.
                gi.fire = true;
                gi.moveX = std::sin(smokeElapsed_ * 1.3f);
                gi.moveY = std::sin(smokeElapsed_ * 0.7f) * 0.5f;
            }
            if (smokeTime_ > 0.0f && smokeBombSent_ < static_cast<int>(smokeBombAt_.size()) &&
                smokeElapsed_ >= smokeBombAt_[static_cast<std::size_t>(smokeBombSent_)]) {
                ++smokeBombSent_;
                gi.special = true;  // one-step press: the edge triggers the bomb
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
        // First real frame is on screen: audio is allowed to make sound now
        // (updateMusic/playGameSound stay gated until this flips).
        windowShown_ = true;
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
    if (!windowShown_) return;  // no sound from behind an unopened window
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
    if (!windowShown_) return;  // no sound from behind an unopened window
    if (!audio_.available()) return;
    // Music belongs to the mission only: every non-game screen (title,
    // options, high scores, help, initials entry, results) stays silent, so
    // the empty request below stops the loop at the stage intro/game-over
    // transitions.
    std::string want;
    switch (game_->state()) {
    case GameState::StageIntro:
    case GameState::Playing:
    case GameState::Paused:
    case GameState::StageComplete:
        want = game_->stageIndex() == 0 ? "music_stage1" : "music_stage2";
        break;
    case GameState::Title:
    case GameState::Options:
    case GameState::HighScores:
    case GameState::Help:
    case GameState::EnterInitials:
    case GameState::GameOver:
    case GameState::Victory:
        want = "";
        break;
    }
    if (want != currentMusic_) {
        currentMusic_ = want;
        if (want.empty())
            audio_.stopMusic();
        else
            audio_.playMusic(want);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void Application::renderTerrain(const StageDef* stage, float scrollY) {
    env_.beginFrame();
    const int cols = 12;
    // Tile choice for a plain (non-shore) terrain kind. Ocean variants are
    // picked from the STATIC tile hash only — never from time. (A time term
    // here used to re-roll every ocean tile ~1.4 times/s, which teleported
    // the whole sea pattern: the "jumping water". Surface motion now comes
    // from the smoothly drifting glint layer drawn after the tile pass.)
    auto tileForKind = [&](TerrainKind kind, std::uint32_t h, int col) -> const char* {
        switch (kind) {
        case TerrainKind::Ocean:    return oceanFor(h);
        case TerrainKind::Island:   return (h % 5 == 0) ? "tile_sand"
                                         : (h % 2 == 0) ? "tile_jungle_a" : "tile_jungle_b";
        case TerrainKind::Jungle:   return (h % 2 == 0) ? "tile_jungle_a" : "tile_jungle_b";
        case TerrainKind::Road:     return (h % 6 < 3) ? "tile_road_v"
                                         : (h % 3 == 0) ? "tile_sand" : "tile_jungle_a";
        // Airfield: two centre lanes are real runway (continuous centreline
        // dashes); the rest is poured apron concrete with sparse sand
        // weathering (too-frequent sand patches read as a yellow checkerboard
        // when the boss freezes the scroll over one wide apron view).
        case TerrainKind::Airfield: return (col == 5 || col == 6)
                                         ? "tile_runway"
                                         : (h % 11 == 0) ? "tile_sand" : "tile_airfield";
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
    // Composed land (Island/Jungle) skips the 32px filler pass: entire rows
    // are painted by the EnvironmentBaker chunks drawn after this loop, so
    // no per-tile lattice ever shows through. Fallback tiles stay for the
    // (unexpected) case where chunk baking is unavailable.
    auto composedLand = [&](TerrainKind kind) {
        return env_.ready() && stage &&
               (kind == TerrainKind::Island || kind == TerrainKind::Jungle);
    };
    // A filler tile may only be skipped when a baked chunk covers the WHOLE
    // tile. Composed spans start/end at arbitrary world Y (never tile- or
    // chunk-aligned) and the bake leaves every row outside its [lo, hi) span
    // transparent, so a tile whose centre is composed land but whose edge
    // crosses the span boundary would otherwise leave an uncovered clear-
    // colour stripe (and a hard cut) at every coastline. Straddling tiles
    // keep their filler art: the chunk paints opaquely over its part.
    auto composedTileCovered = [&](float worldY) {
        if (!stage || !env_.ready()) return false;
        if (!composedLand(terrainAt(*stage, worldY))) return false;
        const auto& segs = stage->terrain;
        std::size_t s = 0;
        for (std::size_t i2 = 0; i2 < segs.size(); ++i2)
            if (worldY >= segs[i2].fromY) s = i2;
        const float lo = segs[s].fromY;
        std::size_t e = s;
        while (e + 1 < segs.size() &&
               (segs[e + 1].kind == TerrainKind::Island ||
                segs[e + 1].kind == TerrainKind::Jungle) &&
               segs[e + 1].fromY <= segs[e].toY + 1.0f)
            ++e;
        const float hi = e + 1 < segs.size() ? segs[e + 1].fromY : segs[e].toY;
        return worldY - 16.0f >= lo && worldY + 16.0f <= hi;
    };
    // Shore bands that border composed land: the baked chunks own that
    // coastline end-to-end (curved waterline + foam are baked in), so the
    // filler under them is plain sea - the old straight sand/foam tile
    // rows must not show through the chunk's soft alpha edge.
    auto coastBand = [&](float worldY) {
        if (!stage || !env_.ready()) return false;
        if (terrainAt(*stage, worldY) != TerrainKind::Shore) return false;
        const auto& segs = stage->terrain;
        std::size_t idx = 0;
        for (std::size_t s = 0; s < segs.size(); ++s)
            if (worldY >= segs[s].fromY) idx = s;
        const auto isLand = [](TerrainKind k) {
            return k == TerrainKind::Island || k == TerrainKind::Jungle;
        };
        const bool prevLand = idx > 0 && isLand(segs[idx - 1].kind);
        const bool nextLand = idx + 1 < segs.size() && isLand(segs[idx + 1].kind);
        return prevLand || nextLand;
    };
    for (int row = rowFirst; row <= rowLast; ++row) {
        const float tileCenterY = base - (static_cast<float>(row) + 0.5f) * kTile;
        const float worldY = (static_cast<float>(row) + 0.5f) * kTile;
        const TerrainKind kind = stage ? terrainAt(*stage, worldY)
                                       : TerrainKind::Ocean;
        if (composedTileCovered(worldY)) continue;
        if (coastBand(worldY)) {
            // Plain sea base: the baked coastline chunk paints beach, land
            // and foam over this; its alpha edge blends into these tiles.
            for (int i = 0; i < cols; ++i) {
                const int tid = renderer_.texture(oceanFor(hashXY(i, row)));
                renderer_.drawRegion(tid, 0, 0, 32, 32,
                                     static_cast<float>(i) * kTile + 16.0f,
                                     tileCenterY);
            }
            continue;
        }
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

    // Composed land chunks: 64px baked landscape (fields, roads, farms,
    // woodland) keyed to world position; deterministic and cached. Rows
    // whose chunk bake is unavailable fall back to the filler tiles above.
    if (stage && env_.ready()) {
        const int chunkFirst =
            static_cast<int>(std::floor(scrollY / EnvironmentBaker::kChunk)) - 1;
        const int chunkLast =
            static_cast<int>(std::floor(base / EnvironmentBaker::kChunk)) + 1;
        for (int cy = chunkFirst; cy <= chunkLast; ++cy) {
            const int tex = env_.get(renderer_, *stage, 0, cy);
            if (tex < 0) {
                // No chunk (e.g. bake failure): keep the old filler so the
                // ground never holes out.
                const float worldY = (static_cast<float>(cy) + 0.5f) *
                                     EnvironmentBaker::kChunk;
                if (composedLand(terrainAt(*stage, worldY))) {
                    const float tileCenterY0 =
                        base - (static_cast<float>(cy) + 0.5f) * EnvironmentBaker::kChunk;
                    for (int i = 0; i < 384 / 32; ++i)
                        for (int j = 0; j < 2; ++j) {
                            const std::uint32_t h = hashXY(i, cy * 2 + j);
                            const int t = renderer_.texture(tileForKind(
                                TerrainKind::Jungle, h, i));
                            renderer_.drawRegion(
                                t, 0, 0, 32, 32,
                                static_cast<float>(i) * kTile + 16.0f,
                                tileCenterY0 + static_cast<float>(j) * kTile -
                                    kTile * 0.5f);
                        }
                }
                continue;
            }
            const float ccy =
                base - (static_cast<float>(cy) + 0.5f) * EnvironmentBaker::kChunk;
            renderer_.draw(tex, 192.0f, ccy);
        }
    }

    // Drifting surface-glint layers: two seamless sheets of soft, low-alpha
    // wave lines (128px near + 192px far) sliding slowly and monotonically
    // downward over open water at different speeds, so their overlap never
    // reads as a lattice. Driven by the monotonic wall clock — not the
    // stage clock, which resets per level (that would snap the layers) and
    // not the scroll, so the sea keeps a gentle living surface even in
    // scroll-frozen boss arenas. A cell is drawn only while the WHOLE cell
    // sits on ocean: coastlines, beaches and shallows keep the calm base
    // plus their own foam, and the glints never wash over land. Per-cell
    // alpha wobble is a STATIC hash (never time-driven), which breaks up
    // the grid impression without any flicker.
    struct GlintLayer { const char* tex; int cell; float speed; };
    static constexpr GlintLayer kGlintLayers[] = {
        {"tile_ocean_glint_far", 192, 7.0f},
        {"tile_ocean_glint", 128, 12.0f},
    };
    const float wall = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    for (const auto& layer : kGlintLayers) {
        const int tid = renderer_.texture(layer.tex);
        if (tid < 0) continue;
        const float cell = static_cast<float>(layer.cell);
        const float half = cell * 0.5f;
        const float drift = std::fmod(wall * layer.speed, cell);
        const int gcols = static_cast<int>(kPlayW / cell);
        const int grows = static_cast<int>(kPlayH / cell) + 2;
        for (int gy = -1; gy <= grows; ++gy) {
            const float cy = static_cast<float>(gy) * cell - half + drift;
            if (cy + half < 0.0f || cy - half > kPlayH) continue;
            if (stage) {
                const bool topSea =
                    terrainAt(*stage, base - (cy - half)) == TerrainKind::Ocean;
                const bool botSea =
                    terrainAt(*stage, base - (cy + half)) == TerrainKind::Ocean;
                if (!topSea || !botSea) continue;
            }
            for (int gx = 0; gx < gcols; ++gx) {
                const float a =
                    0.50f + static_cast<float>(hashXY(gx, gy) % 5U) * 0.05f;
                renderer_.draw(tid, static_cast<float>(gx) * cell + half,
                               cy, a);
            }
        }
    }
}

// Animated wake for the seagoing boss ships. The hull sprites only keep the
// foam hugging the plating now; the broad Kelvin-V bow wave and the churning
// stern trail flow here. Presentation-only: uses the wall-clock render time
// like the ocean glint, and draws BEFORE the hull so foam passes under it.
void Application::renderShipWake(const std::string& hull, float x, float y,
                                 float t) {
    // Canvas geometry per ship family: bow/stern offsets from the sprite
    // centre (all bosses draw bow-down) and the midships half-beam.
    struct Geo { float bow, stern, beam; };
    Geo g{};
    bool ship = true;
    if (hull.find("battleship") != std::string::npos)
        g = {78.0f, -78.0f, 38.0f};
    else if (hull.find("yamato") != std::string::npos)
        g = {124.0f, -124.0f, 48.0f};
    else if (hull.find("destroyer") != std::string::npos)
        g = {90.0f, -90.0f, 22.0f};
    else if (hull.find("cruiser") != std::string::npos)
        g = {98.0f, -98.0f, 29.0f};
    else if (hull.find("carrier") != std::string::npos)
        g = {92.0f, -92.0f, 65.0f};
    else
        ship = false;
    const int dot = renderer_.texture("wake_dot");
    if (!ship || dot < 0) return;

    // --- bow: Kelvin-V foam arms streaming outward from the bow tip --------
    const float bx = x, by = y + g.bow;
    const float armLen = 26.0f + g.beam * 2.0f;
    for (int s = -1; s <= 1; s += 2) {
        for (int k = 0; k < 12; ++k) {
            // Dense enough that the puffs read as one continuous foam line.
            const float u = std::fmod(t * 0.6f + k / 12.0f, 1.0f);
            const float wob = std::sin(t * 3.4f + k * 2.3f) * 0.9f;
            const float px = bx + s * (1.5f + u * armLen * 0.75f);
            const float py = by - 2.0f - u * armLen * 0.66f + wob;
            const float birth = u < 0.06f ? u / 0.06f : 1.0f;
            const float fade = (1.0f - u) * birth;
            // Soft outer foam + a brighter crest core on the same spot: the
            // V stays a crisp white line close to the bow and turns into
            // dissipating spray as it flows aft.
            const float sc = 4.5f + u * 6.5f;
            renderer_.drawScaled(dot, px, py, sc, sc * 0.8f, fade * 0.55f);
            renderer_.drawScaled(dot, px, py, sc * 0.45f, sc * 0.4f,
                                 fade * fade * 0.95f);
        }
    }
    // Pulsing crest right at the bow tip where the water parts.
    const float crest = 0.55f + 0.30f * std::sin(t * 5.0f);
    renderer_.drawScaled(dot, bx, by - 1.0f, 8.0f, 5.0f, crest);
    renderer_.drawScaled(dot, bx - 4.0f, by - 4.0f, 5.0f, 4.0f, crest * 0.7f);
    renderer_.drawScaled(dot, bx + 4.0f, by - 4.0f, 5.0f, 4.0f, crest * 0.7f);

    // --- stern: base foam trail + churning bubbles drifting aft ------------
    const float sy = y + g.stern;
    const int wake = renderer_.texture("wake_foam");
    if (wake >= 0)
        renderer_.draw(wake, x + std::sin(t * 2.0f) * 2.0f, sy - 58.0f);
    for (int k = 0; k < 10; ++k) {
        // Bubbles surface at the transom and are left behind: they drift
        // screen-up, spread across the beam, grow and dissipate.
        const float u = std::fmod(t * 0.5f + k / 10.0f, 1.0f);
        const float side = (k % 2 ? 1.0f : -1.0f) * (2.0f + u * g.beam * 0.9f);
        const float a = (1.0f - u) * (1.0f - u);
        const float sc = 5.0f + u * 10.0f;
        renderer_.drawScaled(dot, x + side, sy - 4.0f - u * 100.0f, sc,
                             sc * 0.7f, a * 0.5f);
        renderer_.drawScaled(dot, x + side, sy - 4.0f - u * 100.0f, sc * 0.4f,
                             sc * 0.3f, a * 0.8f);
    }
    // Foam feathers diverging off the stern corners.
    for (int s = -1; s <= 1; s += 2)
        for (int k = 0; k < 4; ++k) {
            const float u = std::fmod(t * 0.6f + k / 4.0f, 1.0f);
            renderer_.drawScaled(dot, x + s * (g.beam * 0.55f + u * 16.0f),
                                 sy - 2.0f - u * 22.0f, 4.0f + u * 3.0f,
                                 3.0f + u * 3.0f, (1.0f - u) * 0.4f);
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

    // Bosses with arenaOcean (the island fortress) carry their own ground:
    // while one is alive the world renders as open sea, so the island never
    // appears to slide over scrolling land.
    renderTerrain(sim.bossView().arenaOcean ? nullptr : stage, scrollY);

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
    // Hulls draw BEFORE components so the mounts always sit on the deck:
    // the entity pool's free-list can hand a late-spawned boss hull an
    // index higher than its components, and a single pool-order pass would
    // then bury the whole fortress under its own opaque island sprite.
    int compAlive = 0;
    bool hullAlive = false;
    bool airHullOn = false;
    float airHullX = 0.0f, airHullY = 0.0f;
    for (int pass = 0; pass < 2; ++pass) {
      for (const auto& e : sim.entities()) {
        if (!e.alive) continue;
        if (e.kind != EKind::BossHull && e.kind != EKind::BossComponent) continue;
        const bool hull = e.kind == EKind::BossHull;
        if ((pass == 0) != hull) continue;
        const Color tint = e.flashTimer > 0.0f ? Color::rgb(0xFF9090)
                                               : Color::rgb(0xFFFFFF);
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
            // Seagoing bosses: animated bow-wave V + churning stern wake.
            renderShipWake(texName, e.pos.x, e.pos.y, time);
            // Sailing drydock: a horizontal wake streak off the stern.
            if (texName.find("drydock") != std::string::npos) {
                const auto dockView = sim.bossView();
                if (dockView.sailing) {
                    const int wake = renderer_.texture("wake_foam");
                    if (wake >= 0)
                        renderer_.drawScaled(
                            wake, e.pos.x - dockView.sailDir * 150.0f,
                            e.pos.y + std::sin(time * 3.0f) * 3.0f, 120.0f,
                            64.0f, 0.85f, Color::rgb(0xFFFFFF), false,
                            -90.0f * dockView.sailDir);
                }
            }
        }
        if (!hull && e.angle != 0.0f) {
            // Predictive turret: rotate about the barbette centre.
            int tw = 1, th = 1;
            renderer_.textureSize(renderer_.texture(texName), tw, th);
            renderer_.drawScaled(renderer_.texture(texName), e.pos.x, e.pos.y,
                                 static_cast<float>(tw), static_cast<float>(th),
                                 1.0f, tint, false, e.angle * 57.29578f);
        } else {
            renderer_.draw(renderer_.texture(texName), e.pos.x, e.pos.y, 1.0f,
                           tint);
        }
        if (hull && texName.find("airfield") != std::string::npos) {
            airHullOn = true;
            airHullX = e.pos.x;
            airHullY = e.pos.y;
        }
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
    }
    // Bomb scars: each BOMB spent during the AIRFIELD COMMAND fight leaves
    // a fresh crater on the base (fixed apron spots clear of the mounts, so
    // readability holds). Cleared when the flagship leaves the screen.
    if (!hullAlive) bombCraters_ = 0;
    if (airHullOn && bombCraters_ > 0) {
        static constexpr float kCraterOff[kBombCraterMax][2] = {
            {-30.0f, -20.0f}, {34.0f, 36.0f},  {-36.0f, 40.0f},
            {28.0f, -46.0f},  {-24.0f, 86.0f}, {30.0f, 104.0f},
        };
        const int craterTex = renderer_.texture("bomb_crater");
        if (craterTex >= 0)
            for (int i = 0; i < bombCraters_; ++i)
                renderer_.draw(craterTex, airHullX + kCraterOff[i][0],
                               airHullY + kCraterOff[i][1]);
    }
    // Armored again? No: once the last mount dies, flash the hull once.
    if (hullAlive && prevCompAlive_ > 0 && compAlive == 0)
        armorFlashT_ = 0.85f;
    prevCompAlive_ = hullAlive ? compAlive : 0;

    // Pass 4: enemy aircraft with shadows.
    const int propE = renderer_.texture("prop_blur_e");
    for (const auto& e : sim.entities()) {
        if (!e.alive || e.kind != EKind::Enemy || e.enemyDef < 0) continue;
        const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
        if (def.ground || def.ship) continue;
        renderer_.draw(shadowTex, e.pos.x + 4.0f, e.pos.y + 8.0f);
        const Color tint = e.flashTimer > 0.0f ? Color::rgb(0xFF9090)
                                               : Color::rgb(0xFFFFFF);
        renderer_.draw(renderer_.texture(def.sprite), e.pos.x, e.pos.y, 1.0f, tint);
        // Spinning propellers — the same edge-on language as the player's
        // P-38: the sprite bakes a flat prop disc, this pulses the blur
        // streak over it (twins run 180 degrees out of phase) so the eye
        // reads rotation without any upright blade. Anchors come from the
        // art builder, so they match each silhouette's crop exactly; the
        // per-entity phase offset keeps a formation from beating as one.
        if (propE >= 0) {
            if (const auto* anchors = enemyPropAnchors(def.sprite)) {
                const bool beat =
                    std::fmod(time * 11.0f + e.pos.x * 0.11f +
                                  e.pos.y * 0.07f,
                              1.0f) < 0.5f;
                for (std::size_t i = 0; i < anchors->size(); ++i) {
                    const EnemyPropAnchor& a = (*anchors)[i];
                    const float pa = ((i % 2 == 0) == beat) ? 1.0f : 0.3f;
                    // Stretched wider than tall: the drawn disc must
                    // overhang the nose cowl on both sides, so the pulse
                    // reads against the fuselage/wing and the shadow.
                    renderer_.drawScaled(propE, e.pos.x + a.dx,
                                         e.pos.y + a.dy, 15.0f * a.scale,
                                         9.0f * a.scale, pa, tint);
                }
            }
        }
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
                renderer_.drawScaled(ptex, p.pos.x, p.pos.y, 46.0f,
                                     8.0f + 32.0f * squash, alpha);
            } else {
                renderer_.draw(ptex, p.pos.x, p.pos.y, alpha);
                // Spinning propellers: the plane sprite bakes a flat
                // edge-on prop disc at each engine; this pulses the blade
                // streak (prop_blur_p) across it. Two-frame cycle, right
                // engine half a beat out of phase, so the eye reads a
                // continuous spin without any upright blade ever showing.
                // Anchors are constant across the three bank frames.
                const int prop = renderer_.texture("prop_blur_p");
                if (prop >= 0) {
                    const float cyc = std::fmod(time * 11.0f, 1.0f);
                    const float aL = cyc < 0.5f ? 1.0f : 0.4f;
                    const float aR = cyc < 0.5f ? 0.4f : 1.0f;
                    renderer_.draw(prop, p.pos.x - kPlayerEngineX,
                                   p.pos.y + kPlayerPropY, alpha * aL);
                    renderer_.draw(prop, p.pos.x + kPlayerEngineX,
                                   p.pos.y + kPlayerPropY, alpha * aR);
                }
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
    // Bomb detonation: player spent a charge => white flash (the existing
    // boss-detonation overlay, triggered on the charge count dropping).
    // On the airfield, each detonation also scars the base (see pass 3).
    if (const auto pv = sim.player(); pv.bombs < lastBombCount_) {
        flashT_ = 0.3f;
        const auto bv = sim.bossView();
        if (bv.active && std::strstr(bv.hullSprite, "airfield") != nullptr)
            bombCraters_ = std::min(bombCraters_ + 1, kBombCraterMax);
    }
    lastBombCount_ = sim.player().bombs;
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
    renderTerrain(nullptr, time * 40.0f);
    // Beauty shot: the battleship flagship idles at the bottom of the
    // screen, half cropped by the frame edge, slowly swaying.
    const int hull = renderer_.texture("boss_battleship_hull");
    if (hull >= 0) {
        const float bx = 192.0f + std::sin(time * 0.12f) * 30.0f;
        const float by = 486.0f + std::sin(time * 0.5f) * 3.0f;
        // Animated wake (same helper as the boss fight), then the tinted hull.
        renderShipWake("boss_battleship_hull", bx, by, time);
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
        renderer_.text("WASD/ARROWS MOVE   SPACE FIRE   SHIFT BOMB",
                       192.0f, 466.0f, 1, Color::rgb(0x7A90A8), true);
    renderer_.text("ESC PAUSE   GAMEPAD SUPPORTED", 192.0f, 482.0f, 1,
                   Color::rgb(0x50687E), true);
}

void Application::renderStageIntro() {
    const std::string num = "STAGE " + std::to_string(game_->stageIndex() + 1);
    renderer_.text(num, 192.0f, 200.0f, 2, Color::rgb(0x9AB0C8), true);
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
    const std::string clear =
        "STAGE " + std::to_string(game_->stageIndex() + 1) + " CLEAR";
    int cs = 3;
    while (cs > 1 && renderer_.textWidth(clear, cs) > 368) --cs;
    renderer_.text(clear, 192.0f, 200.0f, cs, Color::rgb(0x7AE87A), true);
    // Stage title under the header, auto-fit like the intro screen.
    const std::string name = game_->stageName();
    int ns = 2;
    while (ns > 1 && renderer_.textWidth(name, ns) > 368) --ns;
    renderer_.text(name, 192.0f, 240.0f, ns, Color::rgb(0xFFD860), true);
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

namespace {
// One sprite-catalog entry for the paged HELP screens.
struct HelpSpriteEntry {
    const char* sprite;
    const char* desc;
};

struct HelpPageDef {
    const char* title;
    const HelpSpriteEntry* entries;
    int count;
    int boxW;   // icon fit-box: longest displayed edge, px
    int boxH;
    float rowH;
    float textX;  // description column x
};

// Page 2: enemy aircraft.
const HelpSpriteEntry kHelpAircraft[] = {
    {"enemy_fighter_light", "LIGHT FIGHTER - ATTACKS IN FORMATIONS"},
    {"enemy_interceptor", "INTERCEPTOR - HEAD-ON GUN RUNNER"},
    {"enemy_heavy_fighter", "HEAVY FIGHTER - ARMORED AND DEADLY"},
    {"enemy_fighter_carrier", "CARRIER FIGHTER - LAUNCHED AT YOU"},
    {"enemy_bomber", "HEAVY BOMBER - SLOW, HEAVILY ARMED"},
};
// Page 3: surface & ground targets.
const HelpSpriteEntry kHelpGround[] = {
    {"turret_aa", "AA EMPLACEMENT - SHOOTS AT YOU"},
    {"tank", "ARMORED TANK - ROLLS AND FIRES SHELLS"},
    {"bunker", "BUNKER - TOUGH CONCRETE FLAK POST"},
    {"boat_patrol", "PATROL BOAT - FAST, LIGHTLY ARMED"},
    {"ship_destroyer", "DESTROYER - THREE-GUN SALVOS"},
};
// Page 4 & 5: enemy flagships (boss hulls), five rows per page maximum.
const HelpSpriteEntry kHelpBosses[] = {
    {"boss_battleship_hull", "BATTLESHIP - HEAVY SHELL BARRAGES"},
    {"boss_bomber_hull", "SKY FORTRESS - TURRETED BOMBER"},
    {"boss_destroyer_hull", "DESTROYER - RAPID-FIRE GUN FLAGSHIP"},
    {"boss_carrier_hull", "CARRIER - LAUNCHES FIGHTERS"},
    {"boss_fortress_hull", "COASTAL FORTRESS - ISLAND GUNS"},
};
const HelpSpriteEntry kHelpBosses2[] = {
    {"boss_cruiser_hull", "HEAVY CRUISER - DEADLIEST FLEET GUN"},
    {"boss_airfield_hull", "AIRFIELD COMMAND - ENDLESS ZEROES"},
    {"boss_drydock_hull", "DRYDOCK - SAILS WHEN STRIPPED"},
    {"boss_yamato_hull", "SUPER BATTLESHIP - TRIPLE TURRETS"},
    {"boss_fortress2_hull", "SKY FORTRESS OMEGA - BOMB BAYS"},
};

const HelpPageDef kHelpPages[] = {
    {"ENEMY AIRCRAFT", kHelpAircraft, 5, 34, 34, 46.0f, 76.0f},
    {"SEA AND GROUND TARGETS", kHelpGround, 5, 34, 40, 46.0f, 76.0f},
    {"ENEMY FLAGSHIPS", kHelpBosses, 5, 56, 56, 56.0f, 88.0f},
    {"ENEMY FLAGSHIPS II", kHelpBosses2, 5, 56, 56, 56.0f, 88.0f},
};
}  // namespace

// One catalog page: title, then sprite icon + one-line description rows.
void Application::renderHelpCatalog(int page) {
    const Color gold = Color::rgb(0xFFD860);
    const Color white = Color::rgb(0xD8E0E8);
    const Color dim = Color::rgb(0x9AB0C8);
    const HelpPageDef& pg = kHelpPages[page - 1];
    renderer_.text("HELP", 192.0f, 36.0f, 3, gold, true);
    renderer_.text(pg.title, 192.0f, 78.0f, 2, gold, true);
    // Rows centered between the title and the footer band (which starts
    // at y=444).
    const float startY =
        122.0f + (312.0f - static_cast<float>(pg.count - 1) * pg.rowH) * 0.5f;
    for (int i = 0; i < pg.count; ++i) {
        const float y = startY + static_cast<float>(i) * pg.rowH;
        const int tex = renderer_.texture(pg.entries[i].sprite);
        if (tex >= 0) {
            int tw = 1, th = 1;
            renderer_.textureSize(tex, tw, th);
            const float s = std::min(static_cast<float>(pg.boxW) / static_cast<float>(tw),
                                     static_cast<float>(pg.boxH) / static_cast<float>(th));
            renderer_.drawScaled(tex, 48.0f, y,
                                 static_cast<float>(tw) * s,
                                 static_cast<float>(th) * s);
        }
        renderer_.text(pg.entries[i].desc, pg.textX, y, 1, white, false);
    }
    renderer_.text("LEFT / RIGHT: PAGE", 192.0f, 444.0f, 1, dim, true);
    const std::string pn = std::to_string(page + 1) + " / " +
                           std::to_string(Game::kHelpPages);
    renderer_.text(pn, 192.0f, 460.0f, 1, dim, true);
    renderer_.text("ESC / B: BACK", 192.0f, 476.0f, 1,
                   Color::rgb(0x7A90A8), true);
}

void Application::renderHelp() {
    if (const int page = game_->helpPage(); page > 0) {
        renderHelpCatalog(page);
        return;
    }
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
        {"SHIFT / CTRL", "BOMB", "PAD B / LB"},
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
    renderer_.text("AND FIRE WITH SPACE. BOMB WIPES OUT", 192.0f, 266.0f,
                   1, dim, true);
    renderer_.text("ALL ENEMIES - FLAGSHIPS TAKE 20%", 192.0f, 280.0f, 1,
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
        {"powerup_bomb", "+1 BOMB"},
        {"powerup_life", "EXTRA LIFE"},
    };
    constexpr float kIconX1 = 74.0f, kDescX1 = 94.0f;
    constexpr float kIconX2 = 218.0f, kDescX2 = 238.0f;
    // Text is top-anchored in a 7x11 cell with the cap box at y+1..y+9, so
    // the description's ink center sits 5 px below the row y. The icons are
    // center-anchored: drop them by the same 5 px to share that center.
    constexpr float kIconDY = 5.0f;
    for (int i = 0; i < 4; ++i) {
        const float y = 342.0f + static_cast<float>(i) * 20.0f;
        const int tex = renderer_.texture(leftCol[i].sprite);
        if (tex >= 0) renderer_.draw(tex, kIconX1, y + kIconDY);
        renderer_.text(leftCol[i].desc, kDescX1, y, 1, white, false);
    }
    for (int i = 0; i < 3; ++i) {
        const float y = 342.0f + static_cast<float>(i) * 20.0f;
        const int tex = renderer_.texture(rightCol[i].sprite);
        if (tex >= 0) renderer_.draw(tex, kIconX2, y + kIconDY);
        renderer_.text(rightCol[i].desc, kDescX2, y, 1, white, false);
    }

    // Same paging footer as the catalog pages so the first page shows
    // "1 / 4" too, centered between the supply-drops block (ends ~408)
    // and the screen bottom.
    renderer_.text("LEFT / RIGHT: PAGE", 192.0f, 444.0f, 1,
                   Color::rgb(0x9AB0C8), true);
    renderer_.text("1 / 4", 192.0f, 460.0f, 1, Color::rgb(0x9AB0C8), true);
    renderer_.text("ESC / B: BACK", 192.0f, 476.0f, 1,
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
