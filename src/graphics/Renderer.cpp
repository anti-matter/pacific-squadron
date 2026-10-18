#include "graphics/Renderer.h"

#include <SDL3/SDL.h>

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>

#include "core/MathUtil.h"
#include "graphics/BitmapFont.h"


namespace ww {

namespace {
constexpr int kCanvasW = 384;
constexpr int kCanvasH = 512;
}  // namespace

bool Renderer::init(const std::string& title, bool fullscreen) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
#ifndef NDEBUG
    // Startup portability report: confirms which backend SDL picked
    // (wayland / x11 / dummy / ...) without needing env archaeology.
    SDL_Log("SDL video driver: %s",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    // Build stamp: identifies exactly which commit the binary came from
    // (a git hash, unlike __TIME__ which only reflects one TU's compile).
#if __has_include("buildinfo.h")
#include "buildinfo.h"
    SDL_Log("PACIFIC SQUADRON build %s (built %s)", WW_BUILD_HASH, WW_BUILD_TS);
#else
    SDL_Log("PACIFIC SQUADRON build %s %s (no git info)", __DATE__, __TIME__);
#endif
#endif
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    window_ = SDL_CreateWindow(title.c_str(), 1152, 1536, flags);
    if (!window_) {
        SDL_Log("CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    sdl_ = SDL_CreateRenderer(window_, nullptr);
    if (!sdl_) {
        SDL_Log("CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(sdl_, 1);
    // Explicit blending: not all SDL render backends default to it.
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);

    canvas_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, kCanvasW, kCanvasH);
    if (!canvas_) {
        SDL_Log("CreateTexture(canvas) failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(canvas_, SDL_SCALEMODE_NEAREST);
    fullscreen_ = fullscreen;

    if (fullscreen) {
        SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
    } else {
        // Fit a 3x scaled canvas with a modest window.
        SDL_SetWindowSize(window_, kCanvasW * 2, kCanvasH * 2);
        SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
    }
    return true;
}

void Renderer::shutdown() {
    for (auto* t : textures_) SDL_DestroyTexture(t);
    textures_.clear();
    if (canvas_) SDL_DestroyTexture(canvas_);
    canvas_ = nullptr;
    if (sdl_) SDL_DestroyRenderer(sdl_);
    sdl_ = nullptr;
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int Renderer::createTextureFromPixels(const std::string& name,
                                      const std::uint8_t* rgba, int w, int h) {
    SDL_Texture* old = nullptr;
    auto it = texByName_.find(name);
    const bool replace = it != texByName_.end();
    int id = replace ? it->second : static_cast<int>(textures_.size());
    if (replace) {
        old = textures_[static_cast<std::size_t>(id)];
    } else {
        textures_.push_back(nullptr);
        texW_.push_back(w);
        texH_.push_back(h);
    }
    SDL_Texture* tex = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return -1;
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, nullptr, rgba, w * 4);
    textures_[static_cast<std::size_t>(id)] = tex;
    if (replace) {
        texW_[static_cast<std::size_t>(id)] = w;
        texH_[static_cast<std::size_t>(id)] = h;
        if (old) SDL_DestroyTexture(old);
    }
    texByName_[name] = id;
    return id;
}

bool Renderer::createTextureFromPng(const std::string& name,
                                    const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) return false;
    // stb gives RGBA byte order; our pipeline uses u32 0xRRGGBBAA values.
    std::vector<std::uint32_t> converted(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < converted.size(); ++i) {
        const unsigned char* p = px + i * 4;
        converted[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                       (static_cast<std::uint32_t>(p[1]) << 16) |
                       (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
    stbi_image_free(px);
    createTextureFromPixels(name,
                            reinterpret_cast<const std::uint8_t*>(converted.data()),
                            w, h);
    return true;
}

int Renderer::texture(const std::string& name) const {
    auto it = texByName_.find(name);
    return it == texByName_.end() ? -1 : it->second;
}

bool Renderer::textureSize(int id, int& w, int& h) const {
    if (id < 0 || id >= static_cast<int>(textures_.size())) return false;
    w = texW_[static_cast<std::size_t>(id)];
    h = texH_[static_cast<std::size_t>(id)];
    return true;
}

void Renderer::beginFrame(const Color& clear) {
    SDL_SetRenderTarget(sdl_, canvas_);
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_, clear.r, clear.g, clear.b, clear.a);
    SDL_RenderClear(sdl_);
}

void Renderer::endFrame() {
    SDL_SetRenderTarget(sdl_, nullptr);
    SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
    SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 255);
    SDL_RenderClear(sdl_);

    // Scale policy: snap to an integer multiple when the window is close to
    // one (perfectly uniform pixels at the natural 1x/2x/3x/4x sizes).
    // For awkward window sizes, fill the window with nearest-neighbour
    // fractional scaling - still crisp, just uneven pixel doubling.
    const float fit = std::min(static_cast<float>(winW_) / kCanvasW,
                               static_cast<float>(winH_) / kCanvasH);
    float scale = std::round(fit);
    if (std::abs(fit - scale) > 0.12f) scale = fit;
    if (scale < 1.0f) scale = fit;
    const float dw = std::floor(kCanvasW * scale);
    const float dh = std::floor(kCanvasH * scale);
    const SDL_FRect dst{std::floor((winW_ - dw) * 0.5f),
                        std::floor((winH_ - dh) * 0.5f), dw, dh};
    SDL_RenderTexture(sdl_, canvas_, nullptr, &dst);

    // Native-resolution text overlay: glyphs are rasterized at true display
    // pixel size, so text never inherits the canvas' blocky upscaling.
    renderTextOverlay(dst.x, dst.y, scale);

    // Screenshot of the composited frame (canvas + crisp text overlay).
    if (!screenshotPath_.empty()) {
        SDL_Surface* surf = SDL_RenderReadPixels(sdl_, nullptr);
        if (surf) {
            // Manual channel mapping: the software readback surface is a
            // 32-bit pixel with 0xAARRGGBB semantics.
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(surf->w) *
                                           surf->h * 4);
            const auto* src = static_cast<const std::uint8_t*>(surf->pixels);
            for (int y = 0; y < surf->h; ++y) {
                for (int x = 0; x < surf->w; ++x) {
                    const std::uint8_t* p = src + static_cast<std::size_t>(y) *
                                                      surf->pitch +
                                                  static_cast<std::size_t>(x) * 4;
                    const std::uint32_t v = static_cast<std::uint32_t>(p[0]) |
                                            (static_cast<std::uint32_t>(p[1]) << 8) |
                                            (static_cast<std::uint32_t>(p[2]) << 16) |
                                            (static_cast<std::uint32_t>(p[3]) << 24);
                    std::uint8_t* d = rgba.data() + (static_cast<std::size_t>(y) *
                                                         surf->w + x) * 4;
                    d[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                    d[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                    d[2] = static_cast<std::uint8_t>(v & 0xFF);
                    d[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
                }
            }
            stbi_write_png(screenshotPath_.c_str(), surf->w, surf->h, 4,
                           rgba.data(), surf->w * 4);
            SDL_DestroySurface(surf);
        }
        screenshotPath_.clear();
    }
    SDL_RenderPresent(sdl_);
}

static SDL_FRect centerRect(float cx, float cy, float w, float h) {
    return {std::round(cx - w * 0.5f), std::round(cy - h * 0.5f), w, h};
}

void Renderer::draw(int tex, float cx, float cy, float alpha, const Color& tint,
                    bool flipX, float angleDeg) {
    if (tex < 0 || tex >= static_cast<int>(textures_.size())) return;
    SDL_Texture* t = textures_[static_cast<std::size_t>(tex)];
    SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t, static_cast<Uint8>(clamp(alpha, 0.0f, 1.0f) * 255.0f));
    const SDL_FRect dst = centerRect(cx, cy, texW_[static_cast<std::size_t>(tex)],
                                     texH_[static_cast<std::size_t>(tex)]);
    if (flipX || angleDeg != 0.0f) {
        SDL_RenderTextureRotated(sdl_, t, nullptr, &dst,
                                 static_cast<double>(angleDeg), nullptr,
                                 flipX ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(sdl_, t, nullptr, &dst);
    }
}

void Renderer::drawRegion(int tex, int sx, int sy, int sw, int sh, float cx,
                          float cy, float alpha, const Color& tint, bool flipX,
                          float angleDeg) {
    if (tex < 0 || tex >= static_cast<int>(textures_.size())) return;
    SDL_Texture* t = textures_[static_cast<std::size_t>(tex)];
    SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t, static_cast<Uint8>(clamp(alpha, 0.0f, 1.0f) * 255.0f));
    const SDL_FRect src{static_cast<float>(sx), static_cast<float>(sy),
                        static_cast<float>(sw), static_cast<float>(sh)};
    const SDL_FRect dst = centerRect(cx, cy, sw, sh);
    if (flipX || angleDeg != 0.0f) {
        SDL_RenderTextureRotated(sdl_, t, &src, &dst,
                                 static_cast<double>(angleDeg), nullptr,
                                 flipX ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(sdl_, t, &src, &dst);
    }
}

void Renderer::drawScaled(int tex, float cx, float cy, float dw, float dh,
                          float alpha, const Color& tint, bool flipX,
                          float angleDeg) {
    if (tex < 0 || tex >= static_cast<int>(textures_.size())) return;
    SDL_Texture* t = textures_[static_cast<std::size_t>(tex)];
    SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t, static_cast<Uint8>(clamp(alpha, 0.0f, 1.0f) * 255.0f));
    const SDL_FRect dst = centerRect(cx, cy, dw, dh);
    if (flipX || angleDeg != 0.0f) {
        SDL_RenderTextureRotated(sdl_, t, nullptr, &dst,
                                 static_cast<double>(angleDeg), nullptr,
                                 flipX ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(sdl_, t, nullptr, &dst);
    }
}

void Renderer::fillRect(float x, float y, float w, float h, const Color& c) {
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);
    const SDL_FRect r{std::round(x), std::round(y), std::round(w), std::round(h)};
    SDL_RenderRect(sdl_, &r);   // ensure crisp edges
    SDL_RenderFillRect(sdl_, &r);
}

void Renderer::outlineRect(float x, float y, float w, float h, const Color& c) {
    SDL_SetRenderDrawColor(sdl_, c.r, c.g, c.b, c.a);
    const SDL_FRect r{std::round(x), std::round(y), std::round(w), std::round(h)};
    SDL_RenderRect(sdl_, &r);
}

// ---------------------------------------------------------------------------
// Text: queued during the frame and drawn in endFrame() as a native-resolution
// overlay. The stroke font (BitmapFont.cpp) is baked once per display scale,
// so every glyph is rasterized at its exact on-screen pixel size: smooth
// curves and hairline-consistent strokes at any window size, no canvas
// upscaling involved. Each string gets a soft offset drop shadow for legibility.
// ---------------------------------------------------------------------------
namespace {
constexpr int kTextCellW = 7;
constexpr int kTextCellH = 11;
constexpr int kTextAdvance = 8;
}  // namespace

int Renderer::textWidth(const std::string& str, int scale) const {
    return static_cast<int>(str.size()) * kTextAdvance * scale - scale;
}

void Renderer::text(const std::string& str, float x, float y, int scale,
                    const Color& c, bool center) {
    textQueue_.push_back({str, x, y, scale, c, center});
}

void Renderer::renderTextOverlay(float ox, float oy, float wscale) {
    if (textQueue_.empty()) return;
    for (const TextEntry& q : textQueue_) {
        // Bake/cache one atlas per physical glyph scale (quantized to 1/16px
        // steps so window resizes do not re-bake every frame).
        const int key = static_cast<int>(std::lround(q.scale * wscale * 16.0f));
        if (key < 4) continue;  // sub-pixel: nothing meaningful to draw
        auto it = fontTexByScale_.find(key);
        if (it == fontTexByScale_.end()) {
            const int id = buildFontAtlas(*this, key / 16.0f);
            it = fontTexByScale_.emplace(key, id).first;
        }
        if (it->second < 0) continue;
        SDL_Texture* t = textures_[static_cast<std::size_t>(it->second)];

        const float S = key / 16.0f;                // atlas pixels per base px
        const float cellW = kTextCellW * S, cellH = kTextCellH * S;
        const float adv = kTextAdvance * wscale * q.scale;
        const float wLogical = static_cast<float>(textWidth(q.str, q.scale));
        const float startX =
            ox + (q.center ? q.x - wLogical * 0.5f : q.x) * wscale;
        const float baseY = oy + q.y * wscale;
        const float off = std::max(1.0f, std::round(S * 0.75f));
        const int atlasCellW = static_cast<int>(std::lround(kTextCellW * S));

        // Pass 1: drop shadow.
        SDL_SetTextureColorMod(t, 0, 0, 0);
        SDL_SetTextureAlphaMod(t, static_cast<Uint8>(q.c.a * 6 / 10));
        {
            float px = startX;
            for (char ch : q.str) {
                if (ch >= 32 && ch <= 126) {
                    const SDL_FRect src{static_cast<float>((ch - 32) * atlasCellW),
                                        0.0f, cellW, cellH};
                    const SDL_FRect dst{px + off, baseY + off, cellW, cellH};
                    SDL_RenderTexture(sdl_, t, &src, &dst);
                }
                px += adv;
            }
        }
        // Pass 2: the glyphs themselves.
        SDL_SetTextureColorMod(t, q.c.r, q.c.g, q.c.b);
        SDL_SetTextureAlphaMod(t, q.c.a);
        {
            float px = startX;
            for (char ch : q.str) {
                if (ch >= 32 && ch <= 126) {
                    const SDL_FRect src{static_cast<float>((ch - 32) * atlasCellW),
                                        0.0f, cellW, cellH};
                    const SDL_FRect dst{px, baseY, cellW, cellH};
                    SDL_RenderTexture(sdl_, t, &src, &dst);
                }
                px += adv;
            }
        }
    }
    textQueue_.clear();
}

void Renderer::setFullscreen(bool fs) {
    if (fs == fullscreen_) return;
    SDL_SetWindowFullscreen(window_, fs);
    fullscreen_ = fs;
    SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
}

}  // namespace ww
