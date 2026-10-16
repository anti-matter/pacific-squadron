#include "graphics/Renderer.h"

#include <SDL3/SDL.h>

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>

#include "core/MathUtil.h"
#include "graphics/Font5x7.h"

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
    Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
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
    // Screenshot of the logical canvas (headless smoke verification).
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
    SDL_SetRenderTarget(sdl_, nullptr);
    SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
    SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 255);
    SDL_RenderClear(sdl_);

    // Integer scale when it fits; nearest-neighbour fallback otherwise.
    const float fit = std::min(static_cast<float>(winW_) / kCanvasW,
                               static_cast<float>(winH_) / kCanvasH);
    float scale = std::floor(fit);
    if (scale < 1.0f) scale = fit;
    const float dw = std::floor(kCanvasW * scale);
    const float dh = std::floor(kCanvasH * scale);
    const SDL_FRect dst{std::floor((winW_ - dw) * 0.5f),
                        std::floor((winH_ - dh) * 0.5f), dw, dh};
    SDL_RenderTexture(sdl_, canvas_, nullptr, &dst);
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
// Text: glyph atlas produced from Font5x7. Each glyph occupies a 6x8 cell in
// a single row atlas.
// ---------------------------------------------------------------------------
void Renderer::initFontAtlas(int fontTexId) { fontTex_ = fontTexId; }

int Renderer::textWidth(const std::string& str, int scale) const {
    return static_cast<int>(str.size()) * kGlyphAdvance * scale - scale;
}

void Renderer::text(const std::string& str, float x, float y, int scale,
                    const Color& c, bool center) {
    if (fontTex_ < 0) return;
    const int w = textWidth(str, scale);
    float px = center ? std::round(x - w * 0.5f) : std::round(x);
    const float py = std::round(y);
    SDL_Texture* t = textures_[static_cast<std::size_t>(fontTex_)];
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);
    for (char ch : str) {
        if (ch >= 32 && ch <= 126) {
            const int gx = (ch - 32) * 6;
            const SDL_FRect src{static_cast<float>(gx), 0.0f, 5.0f, 7.0f};
            const SDL_FRect dst{px, py, static_cast<float>(5 * scale),
                                static_cast<float>(7 * scale)};
            SDL_RenderTexture(sdl_, t, &src, &dst);
        }
        px += static_cast<float>(kGlyphAdvance * scale);
    }
}

void Renderer::setFullscreen(bool fs) {
    if (fs == fullscreen_) return;
    SDL_SetWindowFullscreen(window_, fs);
    fullscreen_ = fs;
    SDL_GetWindowSizeInPixels(window_, &winW_, &winH_);
}

}  // namespace ww
