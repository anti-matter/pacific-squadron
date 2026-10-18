#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>


#include "graphics/Renderer.h"

namespace ww {

constexpr std::uint32_t col(std::uint32_t rgb, std::uint8_t a = 255) {
    return (rgb << 8) | a;
}

// Small RGBA pixel canvas used by the procedural art generators.
struct PixelBuf {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px;  // 0xRRGGBBAA

    PixelBuf() = default;
    PixelBuf(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * h_, 0) {}

    void set(int x, int y, std::uint32_t c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(x)] = c;
    }
    std::uint32_t get(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(x)];
    }
    void blend(int x, int y, std::uint32_t c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const std::uint32_t d = get(x, y);
        const int a = static_cast<int>(c & 0xFF);
        if (a >= 255) { set(x, y, c); return; }
        const int ia = 255 - a;
        auto ch = [](std::uint32_t v, int sh) { return static_cast<int>((v >> sh) & 0xFF); };
        const int r = (ch(c, 24) * a + ch(d, 24) * ia) / 255;
        const int g = (ch(c, 16) * a + ch(d, 16) * ia) / 255;
        const int b = (ch(c, 8) * a + ch(d, 8) * ia) / 255;
        const int o = std::min(255, a + ch(d, 0));
        set(x, y, (static_cast<std::uint32_t>(r) << 24) |
                      (static_cast<std::uint32_t>(g) << 16) |
                      (static_cast<std::uint32_t>(b) << 8) |
                      static_cast<std::uint32_t>(o));
    }
    void fill(std::uint32_t c) { std::fill(px.begin(), px.end(), c); }
    void fillRect(int x, int y, int w_, int h_, std::uint32_t c) {
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) set(x + i, y + j, c);
    }
    void fillEllipse(float cx, float cy, float rx, float ry, std::uint32_t c) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float dx = (x + 0.5f - cx) / rx;
                const float dy = (y + 0.5f - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) set(x, y, c);
            }
    }
    void hline(int x0, int x1, int y, std::uint32_t c) {
        for (int x = x0; x <= x1; ++x) set(x, y, c);
    }
    void vline(int x, int y0, int y1, std::uint32_t c) {
        for (int y = y0; y <= y1; ++y) set(x, y, c);
    }
    void blit(const PixelBuf& src, int dx, int dy) {
        for (int y = 0; y < src.h; ++y)
            for (int x = 0; x < src.w; ++x)
                if (src.get(x, y) & 0xFF) set(dx + x, dy + y, src.get(x, y));
    }
    void mirrorX() {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w / 2; ++x)
                std::swap(px[static_cast<std::size_t>(y) * w + x],
                          px[static_cast<std::size_t>(y) * w + (w - 1 - x)]);
    }
    const std::uint8_t* bytes() const {
        return reinterpret_cast<const std::uint8_t*>(px.data());
    }
};

// Generates and registers every game texture (original placeholder art).
bool generateAllArt(Renderer& renderer);

// Optional PNG overrides: assets/sprites/<name>.png replace generated ones.
void loadPngOverrides(Renderer& renderer, const std::string& assetDir);

}  // namespace ww
