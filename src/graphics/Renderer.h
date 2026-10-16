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
// window. Gameplay code never talks to SDL directly.
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

    // ---- text (uses the baked BitmapFont atlas) ------------------------
    void initFontAtlas(int fontTexId);
    void text(const std::string& str, float x, float y, int scale,
              const Color& c, bool center = false);
    int textWidth(const std::string& str, int scale) const;

    // ---- window --------------------------------------------------------
    void setFullscreen(bool fs);
    bool isFullscreen() const { return fullscreen_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* sdl_ = nullptr;
    SDL_Texture* canvas_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    std::vector<int> texW_, texH_;
    std::unordered_map<std::string, int> texByName_;
    int fontTex_ = -1;
    bool fullscreen_ = false;
    int winW_ = 1152, winH_ = 1536;
    std::string screenshotPath_;
};

}  // namespace ww
