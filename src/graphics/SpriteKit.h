#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "graphics/ProceduralArt.h"

namespace ww {

// ---------------------------------------------------------------------------
// SpriteKit — the shared raster toolkit used by every procedural art module
// (ProceduralArt.cpp, EnvironmentArt.cpp).
// ---------------------------------------------------------------------------
namespace kit {

// Scale a packed 0xRRGGBBAA color's RGB by f (alpha preserved).
inline std::uint32_t shade(std::uint32_t c, float f) {
    auto ch = [](std::uint32_t v, int sh) { return static_cast<float>((v >> sh) & 0xFF); };
    const int r = static_cast<int>(ch(c, 24) * f);
    const int g = static_cast<int>(ch(c, 16) * f);
    const int b = static_cast<int>(ch(c, 8) * f);
    const int a = static_cast<int>(c & 0xFF);
    return (static_cast<std::uint32_t>(std::min(255, r)) << 24) |
           (static_cast<std::uint32_t>(std::min(255, g)) << 16) |
           (static_cast<std::uint32_t>(std::min(255, b)) << 8) |
           static_cast<std::uint32_t>(a);
}

// Blend two 0xRRGGBB colors.
inline std::uint32_t mix24(std::uint32_t a, std::uint32_t b, float t) {
    float u = t;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    // NOTE: convert to int BEFORE subtracting — unsigned subtraction
    // underflows and poisons the blend.
    const int ar = static_cast<int>((a >> 16) & 0xFF), ag = static_cast<int>((a >> 8) & 0xFF),
              ab = static_cast<int>(a & 0xFF);
    const int br = static_cast<int>((b >> 16) & 0xFF), bg = static_cast<int>((b >> 8) & 0xFF),
              bb = static_cast<int>(b & 0xFF);
    const int r = static_cast<int>(ar + static_cast<float>(br - ar) * u);
    const int g = static_cast<int>(ag + static_cast<float>(bg - ag) * u);
    const int bl = static_cast<int>(ab + static_cast<float>(bb - ab) * u);
    return col(static_cast<std::uint32_t>((r << 16) | (g << 8) | bl));
}

// Blend two ALREADY-PACKED 0xRRGGBBAA colors (alpha of `a` preserved). The
// 24-bit mix24() above takes raw 0xRRGGBB; passing it a packed color reads
// the channels one byte too high (the historical purple-sprite trap).
inline std::uint32_t mixPacked(std::uint32_t a, std::uint32_t b, float t) {
    float u = t;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    auto ch = [](std::uint32_t v, int sh) { return static_cast<int>((v >> sh) & 0xFF); };
    const int r = static_cast<int>(ch(a, 24) + static_cast<float>(ch(b, 24) - ch(a, 24)) * u);
    const int g = static_cast<int>(ch(a, 16) + static_cast<float>(ch(b, 16) - ch(a, 16)) * u);
    const int bl = static_cast<int>(ch(a, 8) + static_cast<float>(ch(b, 8) - ch(a, 8)) * u);
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(bl) << 8) | (a & 0xFFu);
}

// ---------------------------------------------------------------------------
// Hi — 4x supersampled raster surface.
//
// Sprites are built by painting vector primitives (polygons, ellipses,
// capsules, rounded rects, strokes) into a 4x-resolution subpixel grid, then
// baking down by averaging coverage. Interior pixels carry the exact source
// color; only true silhouette-edge pixels receive partial alpha. This gives
// inherently smooth contours with crisp flat color fields — the antialiasing
// is a product of the rasterization, never a post-filter of finished art.
// ---------------------------------------------------------------------------
struct Hi {
    int w = 0, h = 0;
    static constexpr int SS = 4;  // samples per axis
    std::vector<std::uint32_t> sub;

    Hi() = default;
    Hi(int w_, int h_) : w(w_), h(h_), sub(static_cast<std::size_t>(w_ * SS) * h_ * SS, 0) {}

    void sp(int x, int y, std::uint32_t c) {
        if (x < 0 || y < 0 || x >= w * SS || y >= h * SS) return;
        sub[static_cast<std::size_t>(y) * w * SS + x] = c;
    }

    // --- primitives (coordinates in target pixels) -------------------------
    void fillEllipse(float cx, float cy, float rx, float ry, std::uint32_t c) {
        const int x0 = static_cast<int>((cx - rx) * SS) - 1, x1 = static_cast<int>((cx + rx) * SS) + 1;
        const int y0 = static_cast<int>((cy - ry) * SS) - 1, y1 = static_cast<int>((cy + ry) * SS) + 1;
        for (int sy = y0; sy <= y1; ++sy) {
            const float dy = (sy + 0.5f) / SS - cy;
            for (int sx = x0; sx <= x1; ++sx) {
                const float dx = (sx + 0.5f) / SS - cx;
                const float q = dx * dx / (rx * rx) + dy * dy / (ry * ry);
                if (q <= 1.0f) sp(sx, sy, c);
            }
        }
    }

    void fillPoly(const std::vector<std::pair<float, float>>& pts, std::uint32_t c) {
        if (pts.size() < 3) return;
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (auto& p : pts) {
            minx = std::min(minx, p.first); maxx = std::max(maxx, p.first);
            miny = std::min(miny, p.second); maxy = std::max(maxy, p.second);
        }
        const int x0 = static_cast<int>(minx * SS), x1 = static_cast<int>(maxx * SS) + 1;
        const int y0 = static_cast<int>(miny * SS), y1 = static_cast<int>(maxy * SS) + 1;
        const int n = static_cast<int>(pts.size());
        for (int sy = y0; sy <= y1; ++sy) {
            const float py = (sy + 0.5f) / SS;
            for (int sx = x0; sx <= x1; ++sx) {
                const float px = (sx + 0.5f) / SS;
                bool in = false;  // even-odd
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    const float yi = pts[i].second, yj = pts[j].second;
                    const float xi = pts[i].first, xj = pts[j].first;
                    if ((yi > py) != (yj > py)) {
                        const float xint = xi + (py - yi) / (yj - yi) * (xj - xi);
                        if (px < xint) in = !in;
                    }
                }
                if (in) sp(sx, sy, c);
            }
        }
    }

    static float segDist(float x, float y, float ax, float ay, float bx, float by) {
        const float abx = bx - ax, aby = by - ay;
        const float len2 = abx * abx + aby * aby;
        float t = len2 > 1e-9f ? ((x - ax) * abx + (y - ay) * aby) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float dx = x - (ax + abx * t), dy = y - (ay + aby * t);
        return std::sqrt(dx * dx + dy * dy);
    }

    // Round-capped stroke through a polyline.
    void stroke(const std::vector<std::pair<float, float>>& pts, float width,
                std::uint32_t c, bool closed = false) {
        const int n = static_cast<int>(pts.size());
        if (n < 2 || width <= 0.0f) return;
        const float r = width * 0.5f;
        for (int i = 0; i + 1 < n; ++i) strokeLine(pts[i].first, pts[i].second,
                                                   pts[i + 1].first, pts[i + 1].second, r, c);
        if (closed) strokeLine(pts[n - 1].first, pts[n - 1].second, pts[0].first, pts[0].second, r, c);
    }
    void strokeLine(float ax, float ay, float bx, float by, float r, std::uint32_t c) {
        const int x0 = static_cast<int>((std::min(ax, bx) - r) * SS), x1 = static_cast<int>((std::max(ax, bx) + r) * SS) + 1;
        const int y0 = static_cast<int>((std::min(ay, by) - r) * SS), y1 = static_cast<int>((std::max(ay, by) - r) * SS) + 1;
        for (int sy = y0; sy <= y1; ++sy) {
            const float py = (sy + 0.5f) / SS;
            for (int sx = x0; sx <= x1; ++sx) {
                const float px = (sx + 0.5f) / SS;
                if (segDist(px, py, ax, ay, bx, by) <= r) sp(sx, sy, c);
            }
        }
    }

    void fillRoundRect(float x, float y, float bw, float bh, float r, std::uint32_t c) {
        fillPoly({{x + r, y}, {x + bw - r, y}, {x + bw, y + r}, {x + bw, y + bh - r},
                  {x + bw - r, y + bh}, {x + r, y + bh}, {x, y + bh - r}, {x, y + r}}, c);
        fillEllipse(x + r, y + r, r, r, c);
        fillEllipse(x + bw - r, y + r, r, r, c);
        fillEllipse(x + r, y + bh - r, r, r, c);
        fillEllipse(x + bw - r, y + bh - r, r, r, c);
    }

    // Bake: average subpixel coverage. Solid interiors snap to the exact
    // source color; only edge pixels keep fractional alpha.
    PixelBuf bake() const {
        PixelBuf out(w, h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int aSum = 0, rSum = 0, gSum = 0, bSum = 0;
                for (int sy = 0; sy < SS; ++sy) {
                    for (int sx = 0; sx < SS; ++sx) {
                        const std::uint32_t c = sub[static_cast<std::size_t>(y * SS + sy) *
                                                        w * SS + x * SS + sx];
                        const int a = static_cast<int>(c & 0xFF);
                        if (!a) continue;
                        ++aSum;
                        rSum += static_cast<int>((c >> 24) & 0xFF);
                        gSum += static_cast<int>((c >> 16) & 0xFF);
                        bSum += static_cast<int>((c >> 8) & 0xFF);
                    }
                }
                if (!aSum) continue;
                const int total = SS * SS;
                int alpha = aSum * 255 / total;
                if (alpha > 235) alpha = 255;   // snap near-solid
                else if (alpha < 28) continue;  // drop faint dust
                // Divide BEFORE shifting: rSum can reach 255*SS*SS, and
                // (rSum << 24) overflows 32 bits for any channel > 15
                // (the wraparound used to green-shift every solid sprite).
                const std::uint32_t r =
                    static_cast<std::uint32_t>((rSum + aSum / 2) / aSum) << 24;
                const std::uint32_t g =
                    static_cast<std::uint32_t>((gSum + aSum / 2) / aSum) << 16;
                const std::uint32_t b =
                    static_cast<std::uint32_t>((bSum + aSum / 2) / aSum) << 8;
                out.set(x, y, r + g + b + static_cast<std::uint32_t>(alpha));
            }
        }
        return out;
    }
};

// One-pixel outset outline around the silhouette (drawn onto transparent
// neighbours), giving every sprite in the game the same crisp contour.
inline void outlineSilhouette(PixelBuf& p, std::uint32_t ink) {
    PixelBuf src = p;
    const std::uint8_t inkA = static_cast<std::uint8_t>(ink & 0xFF);
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            if (src.get(x, y) & 0xFF) continue;  // only empty pixels
            int n = 0;
            if (src.get(x - 1, y) & 0xFF) ++n;
            if (src.get(x + 1, y) & 0xFF) ++n;
            if (src.get(x, y - 1) & 0xFF) ++n;
            if (src.get(x, y + 1) & 0xFF) ++n;
            if (!n) continue;
            p.set(x, y, (ink & 0xFFFFFF00u) | inkA);
        }
    }
}

// Soft translucent ellipse blended straight onto a baked sprite (grounded
// shadows, exhaust flame): 4x4 coverage sampling per pixel. Hi.bake()
// flattens source alpha, so anything that must stay see-through is applied
// after baking through this helper instead of as a hard-edged pixel blob.
inline void softBlendEllipse(PixelBuf& p, float cx, float cy, float rx, float ry,
                             std::uint32_t rgb, int maxA) {
    const int x0 = static_cast<int>(std::floor(cx - rx)), x1 = static_cast<int>(std::ceil(cx + rx));
    const int y0 = static_cast<int>(std::floor(cy - ry)), y1 = static_cast<int>(std::ceil(cy + ry));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int n = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = (x + (sx + 0.5f) / 4.0f - cx) / rx;
                    const float dy = (y + (sy + 0.5f) / 4.0f - cy) / ry;
                    if (dx * dx + dy * dy <= 1.0f) ++n;
                }
            }
            if (!n) continue;
            p.blend(x, y, col(rgb, static_cast<std::uint8_t>(maxA * n / 16)));
        }
    }
}

// Soft translucent ellipse blended into a float-alpha working canvas used by
// the environment baker (same coverage sampling as softBlendEllipse, but on
// parallel R/G/B/A planes so it composites over procedural ground).
inline void softEllipseRGBA(std::uint8_t* r, std::uint8_t* g, std::uint8_t* b,
                            std::uint8_t* a, int w, int h, float cx, float cy,
                            float rx, float ry, std::uint32_t rgb, int maxA) {
    const int x0 = static_cast<int>(std::floor(cx - rx)), x1 = static_cast<int>(std::ceil(cx + rx));
    const int y0 = static_cast<int>(std::floor(cy - ry)), y1 = static_cast<int>(std::ceil(cy + ry));
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= h) continue;
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || x >= w) continue;
            int n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = (x + (sx + 0.5f) / 4.0f - cx) / rx;
                    const float dy = (y + (sy + 0.5f) / 4.0f - cy) / ry;
                    if (dx * dx + dy * dy <= 1.0f) ++n;
                }
            if (!n) continue;
            const int idx = y * w + x;
            const int na = (a[idx] * 255 + maxA * n / 16 * (255 - a[idx])) / 255;
            const int srcA = maxA * n / 16;
            const int dr = static_cast<int>((rgb >> 16) & 0xFF);
            const int dg = static_cast<int>((rgb >> 8) & 0xFF);
            const int db = static_cast<int>(rgb & 0xFF);
            r[idx] = static_cast<std::uint8_t>((dr * srcA + r[idx] * (255 - srcA)) / 255);
            g[idx] = static_cast<std::uint8_t>((dg * srcA + g[idx] * (255 - srcA)) / 255);
            b[idx] = static_cast<std::uint8_t>((db * srcA + b[idx] * (255 - srcA)) / 255);
            a[idx] = static_cast<std::uint8_t>(std::min(255, na));
        }
    }
}

}  // namespace kit
}  // namespace ww
