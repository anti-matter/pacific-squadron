#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace ww {

struct Color {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    static constexpr Color rgb(std::uint32_t hex) {
        return {static_cast<std::uint8_t>((hex >> 16) & 0xFF),
                static_cast<std::uint8_t>((hex >> 8) & 0xFF),
                static_cast<std::uint8_t>(hex & 0xFF), 255};
    }
};

// SDL rendering wrapper. All drawing happens on a 384x512 logical canvas
// (nearest-neighbour) that is letterboxed with integer scaling onto the
// window. Text is queued during the frame and drawn as a separate overlay
// AFTER the canvas is composited: the stroke font is baked at true display
// pixel size, so menu/HUD glyphs are crisp and smooth at any window scale
// instead of being nearest-upscaled blocks. Gameplay code never talks to
// SDL directly.
class Renderer {
public:
    bool init(const std::string& title, bool fullscreen);
    void shutdown();

    // ---- textures -----------------------------------------------------
    // Returns a texture id (stable) for `name`; creates or replaces.
    int createTextureFromPixels(const std::string& name,
                                const std::uint8_t* rgba, int w, int h);
    bool createTextureFromPng(const std::string& name, const std::string& path);
    int texture(const std::string& name) const;
    bool textureSize(int id, int& w, int& h) const;
    // RGBA bytes of the last upload for env_* textures (see
    // createTextureFromPixels); nullptr for other textures.
    const std::vector<std::uint8_t>* envPixels(int id) const {
        return id >= 0 && id < static_cast<int>(envPixels_.size()) &&
                       !envPixels_[static_cast<std::size_t>(id)].empty()
                   ? &envPixels_[static_cast<std::size_t>(id)]
                   : nullptr;
    }
    int envPixelSize(int id, int& w, int& h) const {
        if (id < 0 || id >= static_cast<int>(envW_.size())) return 0;
        w = envW_[static_cast<std::size_t>(id)];
        h = envH_[static_cast<std::size_t>(id)];
        return w > 0 ? w * h * 4 : 0;
    }

    // ---- frame --------------------------------------------------------
    void beginFrame(const Color& clearColor = Color::rgb(0x000000));
    void endFrame();
    // Saves the next finished frame as a PNG (used for headless checks).
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; }

    // ---- drawing (logical coordinates, center anchor) -----------------
    void draw(int tex, float cx, float cy, float alpha = 1.0f,
              const Color& tint = Color::rgb(0xFFFFFF), bool flipX = false,
              float angleDeg = 0.0f);
    void drawRegion(int tex, int sx, int sy, int sw, int sh, float cx, float cy,
                    float alpha = 1.0f, const Color& tint = Color::rgb(0xFFFFFF),
                    bool flipX = false, float angleDeg = 0.0f);
    // Draw scaled to an exact destination size (still nearest-neighbour).
    void drawScaled(int tex, float cx, float cy, float dw, float dh,
                    float alpha = 1.0f, const Color& tint = Color::rgb(0xFFFFFF),
                    bool flipX = false, float angleDeg = 0.0f);
    // Axis-aligned rect in screen space (x,y = top-left).
    void fillRect(float x, float y, float w, float h, const Color& c);
    void outlineRect(float x, float y, float w, float h, const Color& c);

    // ---- text (queued; rendered at native resolution in endFrame) ------
    // `scale` may be fractional (e.g. 0.8f for a subtitle one notch smaller
    // than the body text): the stroke font is baked per physical glyph size,
    // so any scale rasterizes crisply rather than resampling a bitmap.
    void text(const std::string& str, float x, float y, float scale,
              const Color& c, bool center = false);
    float textWidth(const std::string& str, float scale) const;

    // ---- window --------------------------------------------------------
    void setFullscreen(bool fs);
    bool isFullscreen() const { return fullscreen_; }
    // Splash presentation: temporarily swaps the logical canvas for a
    // `w`x`h` target sampled with linear filtering, so a high-resolution
    // artwork can fill the window smoothly (the letterbox math and the
    // native-resolution text overlay follow the new canvas). Passing false
    // restores the 384x512 nearest-neighbour game canvas.
    bool setSplashCanvas(bool on, int w, int h);
    void setWindowSize(int w, int h);
    // The window size requested at creation - the game's natural footprint.
    // restoreWindowSize() puts it back (e.g. after the splash resized it).
    void baseWindowSize(int& w, int& h) const { w = baseW_; h = baseH_; }
    void restoreWindowSize();
    // Primary display size in pixels (fallback 1920x1080 when headless).
    void displayBounds(int& w, int& h) const;
    // Vsync is disabled while the loading splash presents progress updates
    // (each present would otherwise block for a refresh), and re-enabled
    // once the first real frame is up.
    void setVSync(bool on);

    // Taskbar/dock/window-manager icon. Loads the PNG pack generated by
    // tools/make_icons.py and hands the window a multi-resolution surface, so
    // X11/Wayland taskbars, the macOS dock and the Windows titlebar all get
    // the game's artwork instead of the generic SDL icon. Missing sizes are
    // skipped, so a partial pack still works.
    bool setWindowIconFromDir(const std::string& iconsDir);

private:
    struct TextEntry {
        std::string str;
        float x, y;
        float scale;
        Color c;
        bool center;
    };
    void renderTextOverlay(float ox, float oy, float wscale);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* sdl_ = nullptr;
    SDL_Texture* canvas_ = nullptr;
    int canvasW_ = 384, canvasH_ = 512;  // current logical canvas size
    std::vector<SDL_Texture*> textures_;
    std::vector<int> texW_, texH_;
    std::vector<std::vector<std::uint8_t>> envPixels_;  // env_* RGBA store
    std::vector<int> envW_, envH_;
    std::unordered_map<std::string, int> texByName_;
    std::vector<TextEntry> textQueue_;
    std::unordered_map<int, int> fontTexByScale_;  // px*16 -> font atlas id
    bool fullscreen_ = false;
    int winW_ = 1152, winH_ = 1536;
    int baseW_ = 1152, baseH_ = 1536;  // game's natural footprint (init)
    std::string screenshotPath_;
};

}  // namespace ww
