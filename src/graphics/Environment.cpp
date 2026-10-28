#include "graphics/Environment.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "game/Simulation.h"  // terrainAt + StageDef
#include "graphics/EnvironmentArt.h"
#include "graphics/SpriteKit.h"

#ifndef NDEBUG
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#endif

namespace ww {
namespace {

using kit::mixPacked;
using kit::shade;

constexpr std::uint64_t SEED = 0xE4E51D0ull;  // fixed environment salt

// ---------------------------------------------------------------------------
// Deterministic hashing / coherent noise (world-space, no time input).
// ---------------------------------------------------------------------------
std::uint32_t hsh(std::int64_t x, std::int64_t y, std::uint64_t seed) {
    std::uint64_t v = seed +
                      static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull +
                      (static_cast<std::uint64_t>(y) + 0x1234567ull) * 0xC2B2AE3D27D4EB4Full;
    v ^= v >> 30; v *= 0xBF58476D1CE4E5B9ull;
    v ^= v >> 27; v *= 0x94D049BB133111EBull;
    v ^= v >> 31;
    return static_cast<std::uint32_t>(v >> 32);
}
float hf(std::int64_t x, std::int64_t y, std::uint64_t seed) {
    return static_cast<float>(hsh(x, y, seed) & 0xFFFFFF) / 16777216.0f;
}
float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }

// Bilinear coherent noise; lattice cell = `cell` world px.
float vnoise(float x, float y, float cell, std::uint64_t seed) {
    const float gx = x / cell, gy = y / cell;
    const std::int64_t x0 = static_cast<std::int64_t>(std::floor(gx));
    const std::int64_t y0 = static_cast<std::int64_t>(std::floor(gy));
    const float fx = smooth01(gx - static_cast<float>(x0));
    const float fy = smooth01(gy - static_cast<std::int64_t>(y0));
    const float a = hf(x0, y0, seed), b = hf(x0 + 1, y0, seed);
    const float c = hf(x0, y0 + 1, seed), d = hf(x0 + 1, y0 + 1, seed);
    return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
}

// ---------------------------------------------------------------------------
// Chunk canvas. The bake canvas spans the FULL play width plus x padding
// (kCanvasW x kCanvasH px) and the chunk's world rows [chunkLo - kPad,
// chunkLo + kChunk + kPad). Canvas row 0 is the TOP (screen space); world Y
// grows upward, so py = kCanvasH - 1 - (wy - chunkLo), px = wx + kPadX.
// ---------------------------------------------------------------------------
constexpr int kChunk = EnvironmentBaker::kChunk;
constexpr int kPadY = 32;
constexpr int kPadX = 32;
constexpr int kSpanW = 384;  // play-area width (Tuning.h exposes its own kPlayW)
constexpr int kCanvasW = kSpanW + 2 * kPadX;
constexpr int kCanvasH = kChunk + 2 * kPadY;
constexpr std::int64_t kCanvasWN = kCanvasW;
constexpr std::int64_t kCanvasHN = kCanvasH;

struct LandMask {
    // Per canvas pixel: 0 = transparent (ocean/shore rows stay with the
    // base tile pass), 1..254 = land blended with sand at the coastline,
    // 255 = full land (features may be stamped).
    std::vector<std::uint8_t> w;
    LandMask() : w(static_cast<std::size_t>(kCanvasH) * kCanvasW, 0) {}
    int at(std::int64_t px, std::int64_t py) const {
        if (px < 0 || py < 0 || px >= kCanvasWN || py >= kCanvasHN) return 0;
        return w[static_cast<std::size_t>(py) * kCanvasW + px];
    }
};

// World-space raster with a coastline clip (nothing ever paints past the
// land mask) and Y-flip handled once.
struct Raster {
    std::int64_t wyBase = 0;  // world y of canvas bottom row
    const LandMask* land = nullptr;

    void setF(std::int64_t px, std::int64_t py, std::uint32_t c) {
        if (px < 0 || py < 0 || px >= kCanvasWN || py >= kCanvasHN) return;
        if (land->at(px, py) < 255) return;
        px_[py * kCanvasW + px] = c;
    }
    void blendF(std::int64_t px, std::int64_t py, std::uint32_t c) {
        if (px < 0 || py < 0 || px >= kCanvasWN || py >= kCanvasHN) return;
        if (land->at(px, py) < 255) return;
        std::uint32_t& d = px_[py * kCanvasW + px];
        const int a = static_cast<int>(c & 0xFF);
        if (a <= 0) return;
        if (a >= 255) { d = c; return; }
        auto ch = [](std::uint32_t v, int sh) { return static_cast<int>((v >> sh) & 0xFF); };
        const int ia = 255 - a;
        const int r = (ch(c, 24) * a + ch(d, 24) * ia) / 255;
        const int g = (ch(c, 16) * a + ch(d, 16) * ia) / 255;
        const int b = (ch(c, 8) * a + ch(d, 8) * ia) / 255;
        const int o = std::min(255, a + ch(d, 0));
        d = (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
            (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(o);
    }
    void setW(std::int64_t wx, std::int64_t wy, std::uint32_t c) {
        setF(wx + kPadX, kCanvasHN - 1 - (wy - wyBase), c);
    }
    void blendW(std::int64_t wx, std::int64_t wy, std::uint32_t c) {
        blendF(wx + kPadX, kCanvasHN - 1 - (wy - wyBase), c);
    }
    void stampDiscW(float x, float y, float r, std::uint32_t c) {
        const std::int64_t bx = static_cast<std::int64_t>(std::floor(x));
        const std::int64_t by = static_cast<std::int64_t>(std::floor(y));
        const int ri = static_cast<int>(r) + 1;
        for (std::int64_t dy = -ri; dy <= ri; ++dy)
            for (std::int64_t dx = -ri; dx <= ri; ++dx) {
                const float ddx = static_cast<float>(dx) - (x - static_cast<float>(bx));
                const float ddy = static_cast<float>(dy) - (y - static_cast<float>(by));
                if (ddx * ddx + ddy * ddy <= r * r) setW(bx + dx, by + dy, c);
            }
    }
    void lineW(float x0, float y0, float x1, float y1, float halfW, std::uint32_t c) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int steps = std::max(1, static_cast<int>(len * (halfW > 1.0f ? 1.0f : 2.0f)));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            stampDiscW(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, halfW, c);
        }
    }
    void polyW(const std::vector<std::pair<float, float>>& pts, std::uint32_t c) {
        if (pts.size() < 3) return;
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (const auto& p : pts) {
            minx = std::min(minx, p.first); maxx = std::max(maxx, p.first);
            miny = std::min(miny, p.second); maxy = std::max(maxy, p.second);
        }
        const int n = static_cast<int>(pts.size());
        for (std::int64_t wy = std::floor(miny); wy <= std::ceil(maxy); ++wy) {
            const float py = static_cast<float>(wy) + 0.5f;
            std::vector<float> xs;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const float yi = pts[i].second, yj = pts[j].second;
                if ((yi > py) != (yj > py))
                    xs.push_back(pts[i].first +
                                 (py - yi) / (yj - yi) * (pts[j].first - pts[i].first));
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2)
                for (std::int64_t wx = std::ceil(xs[k] - 0.5f);
                     wx <= std::floor(xs[k + 1] - 0.5f); ++wx)
                    setW(wx, wy, c);
        }
    }
    bool insidePoly(const std::vector<std::pair<float, float>>& pts, float x, float y) {
        bool in = false;
        const int n = static_cast<int>(pts.size());
        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float yi = pts[i].second, yj = pts[j].second;
            if ((yi > y) != (yj > y)) {
                const float xi = pts[i].first + (y - yi) / (yj - yi) * (pts[j].first - pts[i].first);
                if (x < xi) in = !in;
            }
        }
        return in;
    }
    void lineClipPoly(const std::vector<std::pair<float, float>>& pts, float x0, float y0,
                      float x1, float y1, float halfW, std::uint32_t c) {
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int steps = std::max(1, static_cast<int>(len * 1.5f));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / steps;
            const float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
            if (!insidePoly(pts, x, y)) continue;
            stampDiscW(x, y, halfW, c);
        }
    }

    std::uint32_t* px_ = nullptr;
};

// Soft translucent world disc (used for shade patches and pads).
void softDiscW(Raster& r, float x, float y, float r0, std::uint32_t rgb, int maxA) {
    const std::int64_t bx = static_cast<std::int64_t>(std::floor(x));
    const std::int64_t by = static_cast<std::int64_t>(std::floor(y));
    const int ri = static_cast<int>(r0) + 1;
    for (std::int64_t dy = -ri; dy <= ri; ++dy)
        for (std::int64_t dx = -ri; dx <= ri; ++dx) {
            const float ddx = static_cast<float>(dx) - (x - static_cast<float>(bx));
            const float ddy = static_cast<float>(dy) - (y - static_cast<float>(by));
            const float q = (ddx * ddx + ddy * ddy) / (r0 * r0);
            if (q > 1.0f) continue;
            const int a = static_cast<int>(maxA * (1.0f - q));
            if (a > 0) r.blendW(bx + dx, by + dy, col(rgb, static_cast<std::uint8_t>(a)));
        }
}

// Stamp an environment sprite (RGBA pixels from the renderer's env store),
// centered at world (cx, cy), with a soft dark silhouette copy offset to the
// bottom-right first (cast shadow; sun from the top-left).
void stampSprite(Raster& r, const std::uint32_t* spr, int sw, int sh, float cx,
                 float cy, bool withShadow) {
    const std::int64_t bx = std::lround(cx) - sw / 2;
    const std::int64_t by = std::lround(cy) - sh / 2;
    if (withShadow) {
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                if (!(spr[static_cast<std::size_t>(y) * sw + x] & 0xFF)) continue;
                r.blendW(bx + x + 3, by + y + 4, col(0x0A140C, 70));
            }
    }
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const std::uint32_t c = spr[static_cast<std::size_t>(y) * sw + x];
            if (!(c & 0xFF)) continue;
            r.blendW(bx + x, by + y, c);
        }
}

// ---------------------------------------------------------------------------
// Road lattice: jittered graph of curved edges. Nodes sit on a 192px grid
// with large per-node jitter; the main trunk (grid column 1) is continuous
// inside a land segment, side lanes appear per hash. Bends are hash-chosen,
// so routes curve gently and junctions land at irregular spots.
// ---------------------------------------------------------------------------
constexpr float kRoadCell = 192.0f;
struct RoadNode { float x, y; };
RoadNode roadNode(int i, int j, std::uint64_t salt) {
    return {static_cast<float>(i) * kRoadCell + (hf(i, j, salt + 11) - 0.5f) * 116.0f,
            static_cast<float>(j) * kRoadCell + (hf(i, j, salt + 29) - 0.5f) * 100.0f};
}
std::vector<std::pair<float, float>> roadEdgePts(const RoadNode& a, const RoadNode& b,
                                                 std::uint64_t edgeHash) {
    std::vector<std::pair<float, float>> pts;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::hypot(dx, dy) + 1e-3f;
    const float bend = (static_cast<float>(edgeHash & 1023u) / 1023.0f - 0.5f) *
                       std::min(46.0f, len * 0.30f);
    const float mx = (a.x + b.x) * 0.5f - dy * (bend / len);
    const float my = (a.y + b.y) * 0.5f + dx * (bend / len);
    const int n = 14;
    for (int k = 0; k <= n; ++k) {
        const float t = static_cast<float>(k) / n, u = 1.0f - t;
        pts.push_back({u * u * a.x + 2 * u * t * mx + t * t * b.x,
                       u * u * a.y + 2 * u * t * my + t * t * b.y});
    }
    return pts;
}

// ---------------------------------------------------------------------------
// Field parcels on a 128px lattice. Shared jittered corner points make each
// parcel an irregular quad whose boundaries match its neighbours, so fields
// never read as independent squares.
// ---------------------------------------------------------------------------
constexpr float kFieldCell = 128.0f;
struct FieldQuad {
    std::vector<std::pair<float, float>> pts;
    int kind = -1;  // 0 crop 1 stubble 2 plowed 3 pasture 4 fallow 5 orchard; -1 = none
};
FieldQuad makeField(int i, int j) {
    auto corner = [](int ci, int cj) {
        return std::pair<float, float>{
            static_cast<float>(ci) * kFieldCell + (hf(ci, cj, 0x1F1E01) - 0.5f) * 42.0f,
            static_cast<float>(cj) * kFieldCell + (hf(ci, cj, 0x1F1E02) - 0.5f) * 42.0f};
    };
    auto lerpP = [](std::pair<float, float> p, std::pair<float, float> o, float t) {
        return std::pair<float, float>{p.first + (o.first - p.first) * t,
                                       p.second + (o.second - p.second) * t};
    };
    const auto c00 = corner(i, j), c10 = corner(i + 1, j);
    const auto c11 = corner(i + 1, j + 1), c01 = corner(i, j + 1);
    const float t1 = 0.06f + hf(i, j, 0x1F1E03) * 0.10f;
    const float t2 = 0.06f + hf(i, j, 0x1F1E04) * 0.10f;
    FieldQuad q;
    q.pts = {lerpP(c00, c10, t1), lerpP(c10, c00, t2), lerpP(c11, c01, t1),
             lerpP(c01, c11, t2)};
    const std::uint32_t k = hsh(i, j, 0x1F1E05);
    q.kind = static_cast<int>(k % 8u);
    if (q.kind >= 6) q.kind = -1;  // 25% of parcels stay open ground
    return q;
}

// ---------------------------------------------------------------------------
// Occupancy grid (16px cells): stamps refuse to overlap each other.
// ---------------------------------------------------------------------------
struct Occ {
    // Occupancy grid over the canvas, 16px cells (flipped rows).
    static constexpr int kCols = kCanvasW / 16;
    static constexpr int kRows = kCanvasH / 16;
    std::vector<std::uint8_t> g{static_cast<std::size_t>(kCols) * kRows, 0};

    void box_(float x, float y, float r, int& cx0, int& cx1, int& cy0, int& cy1) const {
        cx0 = std::max(0, static_cast<int>(x - r + kPadX) / 16);
        cx1 = std::min(kCols - 1, static_cast<int>(x + r + kPadX) / 16);
        cy0 = std::max(0, (kCanvasH - static_cast<int>(y + r + kPadY)) / 16);
        cy1 = std::min(kRows - 1, (kCanvasH - 1 - static_cast<int>(y - r + kPadY)) / 16);
    }
    void mark(float x, float y, float r) {
        int cx0, cx1, cy0, cy1;
        box_(x, y, r, cx0, cx1, cy0, cy1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                g[static_cast<std::size_t>(cy) * kCols + cx] = 1;
    }
    bool blocked(float x, float y, float r) const {
        int cx0, cx1, cy0, cy1;
        box_(x, y, r, cx0, cx1, cy0, cy1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                if (g[static_cast<std::size_t>(cy) * kCols + cx]) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// One bake: compose the padded canvas for chunk (cx, cy) of the land segment
// [lo, hi), then crop+upload it into a texture slot.
// ---------------------------------------------------------------------------
struct BakeCtx {
    Renderer& r;
    std::unordered_map<std::string, int> sprId;   // name -> texture id

    // Returns pixel pointer + size for a registered env sprite.
    const std::uint32_t* pixels(const char* name, int& w, int& h) {
        const auto it = sprId.find(name);
        if (it == sprId.end() || it->second < 0) return nullptr;
        const std::vector<std::uint8_t>* bytes = r.envPixels(it->second);
        int pw = 0, ph = 0;
        if (!bytes || r.envPixelSize(it->second, pw, ph) == 0) return nullptr;
        w = pw; h = ph;
        return reinterpret_cast<const std::uint32_t*>(bytes->data());
    }
};

// (kEnvSpriteNames / kEnvSpriteCount live in EnvironmentArt.h)

void runBake(BakeCtx& ctx, PixelBuf& canvas, LandMask& land, int cy,
             float lo, float hi, const StageDef& stage) {
    const std::uint64_t salt =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(lo * 4.0))) *
        0x9E3779B97F4A7C15ull;
    const std::int64_t wyBase = static_cast<std::int64_t>(cy) * kChunk - kPadY;

    Raster ras;
    ras.px_ = canvas.px.data();
    ras.wyBase = wyBase;
    ras.land = &land;
    auto landAtW = [&](float x, float y) {
        return land.at(static_cast<std::int64_t>(x) + kPadX,
                       kCanvasHN - 1 - (static_cast<std::int64_t>(std::floor(y)) - wyBase));
    };

    // ---- 1. base ground materials --------------------------------------
    constexpr int MLC = kCanvasW / 4 + 1;  // material lattice cols
    constexpr int MLR = kCanvasH / 4 + 1;  // material lattice rows
    std::vector<float> m0(static_cast<std::size_t>(MLR) * MLC),
        m1(m0.size()), m2(m0.size()), m3(m0.size()), fW(m0.size());
    for (int gy = 0; gy < MLR; ++gy) {
        const float wy = static_cast<float>(wyBase) + gy * 4.0f;
        for (int gx = 0; gx < MLC; ++gx) {
            const float wx = gx * 4.0f - kPadX;
            const std::size_t k = static_cast<std::size_t>(gy) * MLC + gx;
            const float f = vnoise(wx, wy, 170.0f, SEED + 1);
            fW[k] = f;
            const float m = vnoise(wx, wy, 95.0f, SEED + 3) +
                            (vnoise(wx, wy, 38.0f, SEED + 4) - 0.5f) * 0.40f;
            float& g0 = m0[k]; float& g1 = m1[k]; float& g2 = m2[k]; float& g3 = m3[k];
            g0 = 1.0f; g1 = g2 = g3 = 0.0f;
            const int best = std::min(3, static_cast<int>(m * 4.0f));
            if (best == 1) { g0 = 0.0f; g1 = 1.0f; }
            else if (best == 2) { g0 = 0.0f; g2 = 1.0f; }
            else if (best == 3) { g0 = 0.0f; g3 = 1.0f; }
            if (f > 0.62f) {  // forest floor takes over in dense woodland
                const float w = std::min(1.0f, (f - 0.62f) / 0.20f);
                g3 = std::max(g3, w);
                if (w > g3 * 0.5f) g0 = 1.0f - w;
            }
        }
    }
    auto lat = [](const std::vector<float>& v, float fx, float fy) {
        int x0 = std::min(MLC - 1, std::max(0, static_cast<int>(fx)));
        int y0 = std::min(MLR - 1, std::max(0, static_cast<int>(fy)));
        const int x1 = std::min(MLC - 1, x0 + 1), y1 = std::min(MLR - 1, y0 + 1);
        const float tx = std::min(1.0f, std::max(0.0f, fx - x0));
        const float ty = std::min(1.0f, std::max(0.0f, fy - y0));
        const float a = v[static_cast<std::size_t>(y0) * MLC + x0];
        const float b = v[static_cast<std::size_t>(y0) * MLC + x1];
        const float c = v[static_cast<std::size_t>(y1) * MLC + x0];
        const float d = v[static_cast<std::size_t>(y1) * MLC + x1];
        return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
    };
    const std::uint32_t matGrass = col(0x4C6235);
    const std::uint32_t matDry = col(0x74713F);
    const std::uint32_t matDirt = col(0x7C6746);
    const std::uint32_t matFloor = col(0x33472A);
    const std::uint32_t sandC = col(0xC2A868);
    for (int py = 0; py < kCanvasH; ++py) {
        const std::int64_t wy = wyBase + (kCanvasH - 1 - py);
        const float gyf = static_cast<float>(kCanvasH - 1 - py) / 4.0f;
        for (int px = 0; px < kCanvasW; ++px) {
            const std::size_t pk = static_cast<std::size_t>(py) * kCanvasW + px;
            const int la = land.w[pk];
            if (!la) continue;
            std::uint32_t c = matGrass;
            c = mixPacked(c, matDry, lat(m1, px / 4.0f, gyf));
            c = mixPacked(c, matDirt, lat(m2, px / 4.0f, gyf));
            c = mixPacked(c, matFloor, lat(m3, px / 4.0f, gyf));
            // Static grain: one octave of coarse blotching + fine speckle.
            const float n1 = hf(px, wy, SEED + 5) - 0.5f;  // px includes padX: stable world hash
            const float n2 = hf(px / 4, wy / 4, SEED + 6) - 0.5f;
            c = shade(c, 1.0f + n1 * 0.06f + n2 * 0.10f);
            if (la < 255) c = mixPacked(sandC, c, la / 255.0f);
            canvas.px[pk] = c | 0xFFu;
        }
    }

    // ---- 2. farmland parcels --------------------------------------------
    const int f0 = static_cast<int>(std::floor(static_cast<float>(wyBase) / kFieldCell)) - 1;
    for (int fj = f0; fj <= f0 + 3; ++fj) {
        for (int fi = -1; fi <= 3; ++fi) {
            FieldQuad q = makeField(fi, fj);
            if (q.kind < 0) continue;
            const float mx = (q.pts[0].first + q.pts[2].first) * 0.5f;
            const float my = (q.pts[0].second + q.pts[2].second) * 0.5f;
            if (my < wyBase - 40.0f || my > wyBase + kCanvasH + 40.0f) continue;
            const float fa = vnoise(mx, my, 150.0f, SEED + 2);
            const float fo = vnoise(mx, my, 170.0f, SEED + 1);
            if (fa < 0.42f || fo > 0.55f) continue;
            std::uint32_t fillC;
            switch (q.kind) {
            case 0: fillC = col(0x54743A); break;   // green crop
            case 1: fillC = col(0x9A8A5C); break;   // harvested stubble
            case 2: fillC = col(0x6B5236); break;   // plowed earth
            case 3: fillC = col(0x667E42); break;   // pasture
            case 4: fillC = col(0x87764F); break;   // fallow
            default: fillC = col(0x4E6A3A); break;  // orchard ground
            }
            fillC = mixPacked(fillC, col(0x808070), 0.25f);  // muted: scenery, never a target
            ras.polyW(q.pts, fillC);
            // Crop rows / furrows clipped to the parcel.
            if (q.kind != 3) {
                const float ang = hf(fi, fj, SEED + 8) * 3.1415927f;
                const float dirX = std::cos(ang), dirY = std::sin(ang);
                const float rowGap = q.kind == 2 ? 3.5f : 3.0f;
                const std::uint32_t rowC =
                    shade(fillC, (q.kind == 1 || q.kind == 4) ? 1.12f : 0.84f);
                for (int k = -24; k <= 24; ++k) {
                    const float ox = mx - dirY * k * rowGap;
                    const float oy = my + dirX * k * rowGap;
                    ras.lineClipPoly(q.pts, ox - dirX * 70.0f, oy - dirY * 70.0f,
                                     ox + dirX * 70.0f, oy + dirY * 70.0f, 0.5f, rowC);
                }
            }
            // Boundary treatment per edge (keyed to the edge so both sides
            // of a boundary agree): hedgerow, fence, or open bank.
            for (int e = 0; e < 4; ++e) {
                const auto& a = q.pts[e];
                const auto& b = q.pts[(e + 1) % 4];
                const std::uint64_t eh =
                    hsh(static_cast<std::int64_t>(a.first * 8.0),
                        static_cast<std::int64_t>(a.second * 8.0), 0x3D7u);
                const int style = static_cast<int>(eh % 10u);
                if (style < 4) {  // hedgerow
                    ras.lineW(a.first, a.second, b.first, b.second, 1.6f, col(0x27381C));
                    const float len = std::hypot(b.first - a.first, b.second - a.second);
                    const int nb = std::max(2, static_cast<int>(len / 6.0f));
                    for (int k = 0; k <= nb; ++k) {
                        const float t = static_cast<float>(k) / nb;
                        ras.stampDiscW(
                            a.first + (b.first - a.first) * t,
                            a.second + (b.second - a.second) * t +
                                (hf(k, e, SEED + 9) - 0.5f) * 1.6f,
                            1.8f + hf(k, e, SEED + 10) * 1.2f,
                            hf(k, e, SEED + 11) > 0.5f ? col(0x37502A) : col(0x2C4222));
                    }
                } else if (style < 6) {  // post-and-rail fence
                    ras.lineW(a.first, a.second, b.first, b.second, 0.45f, col(0x6B5637));
                    const float len = std::hypot(b.first - a.first, b.second - a.second);
                    const int np = std::max(2, static_cast<int>(len / 8.0f));
                    for (int k = 0; k <= np; ++k) {
                        const float t = static_cast<float>(k) / np;
                        ras.setW(static_cast<std::int64_t>(a.first + (b.first - a.first) * t),
                                 static_cast<std::int64_t>(a.second + (b.second - a.second) * t),
                                 col(0x4E3D26));
                    }
                }
            }
        }
    }

    // Soft shade patches under dense canopy (depth for the forest floor).
    for (int j = f0; j <= f0 + 4; ++j)
        for (int i = -2; i <= 5; ++i) {
            const float cxw = (i + 0.5f) * 64.0f, cyw = (j + 0.5f) * 64.0f;
            if (cyw < wyBase - 40.0f || cyw > wyBase + kCanvasH + 40.0f) continue;
            const float fo = vnoise(cxw, cyw, 170.0f, SEED + 1);
            if (fo < 0.55f || hf(i, j, SEED + 12) > 0.60f) continue;
            softDiscW(ras, cxw, cyw, 18.0f + hf(i, j, SEED + 13) * 16.0f, 0x24361E, 90);
        }

    // ---- 3. road network --------------------------------------------------
    std::vector<std::uint8_t> roadMask(static_cast<std::size_t>(kCanvasH) * kCanvasW, 0);
    auto markRoad = [&](float x, float y) {
        const std::int64_t px = static_cast<std::int64_t>(std::floor(x)) + kPadX;
        const std::int64_t py = kCanvasHN - 1 - (static_cast<std::int64_t>(std::floor(y)) - wyBase);
        if (px >= 0 && py >= 0 && px < kCanvasWN && py < kCanvasHN)
            roadMask[static_cast<std::size_t>(py) * kCanvasW + px] = 1;
    };
    auto paintRoad = [&](const std::vector<std::pair<float, float>>& pts, float w,
                         bool paved, bool mainRoad) {
        const std::uint32_t core = paved ? col(0x7A7C78) : col(0x93764E);
        const std::uint32_t edge = paved ? col(0x5C5E5A) : col(0x6E5636);
        for (int s = 0; s + 1 < static_cast<int>(pts.size()); ++s) {
            ras.lineW(pts[s].first, pts[s].second, pts[s + 1].first, pts[s + 1].second,
                      w + 0.9f, edge);
            ras.lineW(pts[s].first, pts[s].second, pts[s + 1].first, pts[s + 1].second,
                      w, core);
            // faint wheel ruts on dirt
            if (!paved) {
                const float dx = pts[s + 1].first - pts[s].first;
                const float dy = pts[s + 1].second - pts[s].second;
                const float len = std::hypot(dx, dy) + 1e-3f;
                const float nx = -dy / len * w * 0.45f, ny = dx / len * w * 0.45f;
                ras.lineW(pts[s].first + nx, pts[s].second + ny,
                          pts[s + 1].first + nx, pts[s + 1].second + ny, 0.4f,
                          shade(core, 0.86f));
                ras.lineW(pts[s].first - nx, pts[s].second - ny,
                          pts[s + 1].first - nx, pts[s + 1].second - ny, 0.4f,
                          shade(core, 0.86f));
            }
            for (int k = 0; k <= 16; ++k) {
                const float t = static_cast<float>(k) / 16.0f;
                markRoad(pts[s].first + (pts[s + 1].first - pts[s].first) * t,
                         pts[s].second + (pts[s + 1].second - pts[s].second) * t);
            }
        }
        if (paved) {  // dashed centreline
            float dash = 0.0f;
            for (int s = 0; s + 1 < static_cast<int>(pts.size()); ++s) {
                const float len = std::hypot(pts[s + 1].first - pts[s].first,
                                             pts[s + 1].second - pts[s].second);
                const int n = std::max(1, static_cast<int>(len));
                for (int k = 0; k < n; ++k) {
                    const float t = static_cast<float>(k) / n;
                    dash += len / n;
                    if (std::fmod(dash, 11.0f) > 5.5f) continue;
                    ras.stampDiscW(pts[s].first + (pts[s + 1].first - pts[s].first) * t,
                                   pts[s].second + (pts[s + 1].second - pts[s].second) * t,
                                   0.45f, col(0xB9BCB6));
                }
            }
        }
        (void)mainRoad;
    };
    const int r0 = static_cast<int>(std::floor(static_cast<float>(wyBase) / kRoadCell)) - 1;
    auto edgeExists = [](int i, int j, bool vert) {
        return hsh(i, j, 0x0AD01) % 100u < (vert ? 74u : 66u);
    };
    auto mainTrunk = [](int i) {
        return i == 1 || hsh(i, 0, 0x7A7Au) % 100u < 34u;
    };
    for (int j = r0; j <= r0 + 2; ++j) {
        for (int i = -1; i <= 3; ++i) {
            const RoadNode a = roadNode(i, j, salt);
            if (hf(i, j, 0x0AD02) < 0.80f && edgeExists(i, j, false)) {
                const bool paved = !mainTrunk(i) && hf(i, j, 0x0AD03) < 0.30f;
                paintRoad(roadEdgePts(a, roadNode(i + 1, j, salt), hsh(i, j, 0x0AD04)),
                          paved ? 3.0f : 2.4f, paved, mainTrunk(i));
            }
            if ((mainTrunk(i) || edgeExists(i, j, true)) && hf(i, j, 0x0AD05) > 0.10f) {
                const bool paved = !mainTrunk(i) && hf(i, j, 0x0AD06) < 0.30f;
                paintRoad(roadEdgePts(a, roadNode(i, j + 1, salt), hsh(i, j, 0x0AD07)),
                          paved ? 3.0f : 2.4f, paved, mainTrunk(i));
            }
        }
    }

    // ---- 4. structures ------------------------------------------------------
    Occ occ;
    auto tryStamp = [&](const char* name, float x, float y, float clearR,
                        bool markOcc = true) -> bool {
        if (occ.blocked(x, y, clearR)) return false;
        if (landAtW(x, y) < 255) return false;
        if (y - clearR < lo + 14.0f || y + clearR > hi - 14.0f) return false;
        int sw = 0, sh = 0;
        const std::uint32_t* sp = ctx.pixels(name, sw, sh);
        if (!sp) return false;
        stampSprite(ras, sp, sw, sh, x, y, true);
        if (markOcc) occ.mark(x, y, clearR);
        return true;
    };
    // Trees never mark occupancy: their placement must be identical in every
    // chunk that sees the same world spot (order-independent, seam-free).
    auto tryTree = [&](const char* name, float x, float y, float clearR) {
        return tryStamp(name, x, y, clearR, false);
    };
    auto nearestRoad = [&](float x, float y, float& rx, float& ry) {
        int bestD = 999999;
        std::int64_t bx = 0, by = 0;
        const std::int64_t cpx = static_cast<std::int64_t>(x) + kPadX;
        const std::int64_t cpy = kCanvasHN - 1 - (static_cast<std::int64_t>(y) - wyBase);
        for (int d = 5; d <= 60 && bestD == 999999; d += 6)
            for (int ang = 0; ang < 28; ++ang) {
                const float a = static_cast<float>(ang) * 0.2244f;
                const std::int64_t sx2 = cpx + static_cast<std::int64_t>(std::cos(a) * d);
                const std::int64_t sy2 = cpy + static_cast<std::int64_t>(std::sin(a) * d);
                if (sx2 < 0 || sy2 < 0 || sx2 >= kCanvasWN || sy2 >= kCanvasHN) continue;
                if (!roadMask[static_cast<std::size_t>(sy2) * kCanvasW + sx2]) continue;
                const std::int64_t dd = (sx2 - cpx) * (sx2 - cpx) + (sy2 - cpy) * (sy2 - cpy);
                if (dd < bestD) { bestD = static_cast<int>(dd); bx = sx2; by = sy2; }
            }
        if (bestD == 999999) return false;
        rx = static_cast<float>(bx);
        ry = static_cast<float>(wyBase + (kCanvasHN - 1 - by));
        return true;
    };
    auto driveway = [&](float x, float y) {
        float rx, ry;
        if (!nearestRoad(x, y, rx, ry)) return;
        const float dx = rx - x, dy = ry - y;
        const float len = std::hypot(dx, dy);
        if (len < 2.0f || len > 62.0f) return;
        ras.lineW(x, y, x + dx * 0.96f, y + dy * 0.96f, 1.1f, col(0x8A6E48));
        ras.lineW(x, y, x + dx * 0.96f, y + dy * 0.96f, 0.4f, col(0x9C7F58));
    };

    // Set-piece bodies, shared by the random 128px-cell scatter below and
    // by scripted stage landmarks (kind=yaml below). i/j only feed variant
    // hashes; landmark callers pass world-derived keys.
    auto placeFarmstead = [&](std::int64_t i, std::int64_t j, std::uint64_t cellH,
                              float cxw, float cyw, bool jitter) {
        const float ox = jitter ? static_cast<float>((cellH >> 8) % 41) - 20.0f : 0.0f;
        const float oy = jitter ? static_cast<float>((cellH >> 14) % 41) - 20.0f : 0.0f;
        const float hx = cxw + ox, hy = cyw + oy;
        if (landAtW(hx, hy) < 255) return;
        softDiscW(ras, hx, hy, 16.0f, 0x77603F, 120);
        softDiscW(ras, hx - 4.0f, hy + 3.0f, 9.0f, 0x83704B, 110);
        occ.mark(hx, hy, 5.0f);
        const bool gotHouse =
            tryStamp("env_farmhouse_0", hx, hy, 11.0f) ||
            tryStamp("env_farmhouse_1", hx, hy, 11.0f);
        if (gotHouse) {
            const float ba = hf(i, j, 71) * 6.2831853f;
            tryStamp("env_barn_0", hx + std::cos(ba) * 22.0f,
                     hy + std::sin(ba) * 19.0f, 12.0f) ||
                tryStamp("env_barn_1", hx + std::cos(ba) * 22.0f,
                         hy + std::sin(ba) * 19.0f, 12.0f);
            if (hf(i, j, 73) < 0.70f)
                tryStamp("env_shed_0", hx + 16.0f, hy - 14.0f, 6.0f) ||
                    tryStamp("env_shed_1", hx + 16.0f, hy - 14.0f, 6.0f);
            if (hf(i, j, 79) < 0.65f)
                tryStamp("env_haystack", hx - 15.0f, hy - 13.0f, 4.0f);
            if (hf(i, j, 83) < 0.30f)
                tryStamp("env_well", hx + 6.0f, hy + 12.0f, 4.0f);
            driveway(hx, hy - 12.0f);
        }
    };
    auto placeHamlet = [&](std::int64_t i, std::int64_t j, std::uint64_t cellH,
                           float cxw, float cyw) {
        if (landAtW(cxw, cyw) < 255) return;
        const int houses = 3 + static_cast<int>((cellH >> 16) % 4u);
        int placed = 0;
        for (int k = 0; k < houses; ++k) {
            const float a = 6.2831853f * k / houses + hf(i, j, 91 + k) * 0.9f;
            const float d = 9.0f + hf(i, j, 101 + k) * 17.0f;
            const char* hn[3] = {"env_house_0", "env_house_1", "env_house_2"};
            if (tryStamp(hn[(cellH >> (k * 2)) % 3u],
                         cxw + std::cos(a) * d, cyw + std::sin(a) * d * 0.85f, 8.0f))
                ++placed;
            if (k == houses - 1 && hf(i, j, 111) < 0.45f)
                tryStamp("env_shed_1", cxw + std::cos(a) * (d + 12.0f),
                         cyw + std::sin(a) * (d + 10.0f), 5.0f);
        }
        if (placed >= 2) {
            if (hf(i, j, 121) < 0.40f) tryStamp("env_well", cxw, cyw, 4.0f);
            driveway(cxw, cyw + 14.0f);
        }
    };
    auto placeDepot = [&](std::int64_t i, std::int64_t j, std::uint64_t cellH,
                          float cxw, float cyw) {
        (void)i; (void)j; (void)cellH;
        if (landAtW(cxw, cyw) < 255) return;
        softDiscW(ras, cxw, cyw, 22.0f, 0x6E5F45, 150);
        softDiscW(ras, cxw + 5.0f, cyw - 4.0f, 13.0f, 0x7A6C50, 130);
        occ.mark(cxw, cyw, 4.0f);
        const bool gotWh =
            tryStamp("env_warehouse_0", cxw - 4.0f, cyw + 6.0f, 14.0f) ||
            tryStamp("env_warehouse_1", cxw - 4.0f, cyw + 6.0f, 14.0f);
        tryStamp("env_barracks_0", cxw + 10.0f, cyw - 12.0f, 10.0f) ||
            tryStamp("env_barracks_1", cxw + 10.0f, cyw - 12.0f, 10.0f);
        tryStamp("env_fuel_tank_0", cxw - 16.0f, cyw - 12.0f, 8.0f);
        tryStamp("env_fuel_tank_1", cxw - 4.0f, cyw - 16.0f, 8.0f);
        if (gotWh) driveway(cxw, cyw - 20.0f);
    };

    // ---- 4a. scripted landmarks: intentional set pieces from stage data ----
    // Stamped before the random scatter so their occupancy marks suppress
    // nearby random structures; positions are pure world data, so every
    // chunk that sees one paints it identically.
    for (const LandmarkDef& lm : stage.landmarks) {
        if (lm.y < lo + 20.0f || lm.y > hi - 20.0f) continue;
        if (lm.y < wyBase - 40.0f || lm.y > wyBase + kCanvasH + 40.0f) continue;
        const std::int64_t li = static_cast<std::int64_t>(std::lround(lm.x));
        const std::int64_t lj = static_cast<std::int64_t>(std::lround(lm.y));
        const std::uint64_t lh = hsh(li, lj, salt ^ 0x5EED1u);
        if (lm.kind == "farmstead") placeFarmstead(li, lj, lh, lm.x, lm.y, false);
        else if (lm.kind == "hamlet") placeHamlet(li, lj, lh, lm.x, lm.y);
        else if (lm.kind == "depot") placeDepot(li, lj, lh, lm.x, lm.y);
    }
    auto nearLandmark = [&](float x, float y) {
        for (const LandmarkDef& lm : stage.landmarks)
            if (std::fabs(lm.x - x) < 88.0f && std::fabs(lm.y - y) < 88.0f) return true;
        return false;
    };

    const int b0 = static_cast<int>(std::floor(static_cast<float>(wyBase) / 128.0f)) - 1;
    for (int j = b0; j <= b0 + 3; ++j) {
        for (int i = 0; i <= 3; ++i) {
            const std::uint64_t cellH = hsh(i, j, salt ^ 0x5EED1u);
            const float cxw = (i + 0.5f) * 128.0f + (hf(i, j, salt ^ 0x5EED2) - 0.5f) * 56.0f;
            const float cyw = (static_cast<float>(j) + 0.5f) * 128.0f +
                              (hf(i, j, salt ^ 0x5EED3) - 0.5f) * 56.0f;
            if (cyw < lo + 20.0f || cyw > hi - 20.0f) continue;
            if (cyw < wyBase - 40.0f || cyw > wyBase + kCanvasH + 40.0f) continue;
            if (nearLandmark(cxw, cyw)) continue;
            const float fa = vnoise(cxw, cyw, 150.0f, SEED + 2);
            const float fo = vnoise(cxw, cyw, 170.0f, SEED + 1);

            // Farmstead on farmland: yard pad, house, barn, shed, haystack.
            if (fa > 0.50f && fo < 0.55f && (cellH % 100u) < 46u) {
                placeFarmstead(i, j, cellH, cxw, cyw, true);
                continue;
            }
            // Hamlet on open ground: ring of houses, well/green, sheds.
            if (fa < 0.58f && fo < 0.50f && ((cellH >> 7) % 100u) < 34u) {
                placeHamlet(i, j, cellH, cxw, cyw);
                continue;
            }
            // Military depot (rare): cleared pad, warehouse, barracks, fuels.
            if (((cellH >> 11) % 100u) < 12u && fo < 0.60f) {
                placeDepot(i, j, cellH, cxw, cyw);
                continue;
            }
            // Big props: pond in open country, rock outcrops anywhere.
            if (((cellH >> 3) % 100u) < 16u && fo < 0.45f) {
                if (tryStamp("env_pond", cxw + 10.0f, cyw, 14.0f)) continue;
            }
            if (((cellH >> 5) % 100u) < 14u) {
                tryStamp("env_rock_0", cxw, cyw, 7.0f) ||
                    tryStamp("env_rock_1", cxw, cyw, 9.0f);
            }
        }
    }

    // ---- 5. trees & vegetation ------------------------------------------
    // Every hash key here is a WORLD coordinate, so two neighbouring chunks
    // that see the same candidate spot make the identical decision (no seam,
    // no duplicated tree). Trees deliberately do NOT mark the occupancy grid
    // (their decisions must stay order-independent across chunks); they only
    // avoid the structures/roads stamped before them.
    // The candidate grid must be aligned to absolute world coordinates: with
    // a canvas-row grid, adjacent chunks (whose padded canvases overlap) would
    // consider different world spots and clip trees at the chunk seam.
    const std::int64_t wyTop = wyBase + kCanvasHN - 1;
    const std::int64_t wyStart = wyBase - ((wyBase % 12 + 12) % 12);
    for (std::int64_t wyI = wyStart; wyI <= wyTop; wyI += 12) {
        const float wy = static_cast<float>(wyI);
        if (wy < lo + 5.0f || wy > hi - 5.0f) continue;
        for (int gx = -kPadX; gx < kSpanW + kPadX; gx += 12) {
            const float x = gx + (hf(gx, wyI, SEED + 21) - 0.5f) * 10.0f;
            const float y = wy + (hf(gx, wyI + 1, SEED + 22) - 0.5f) * 10.0f;
            const float fo = vnoise(x, y, 170.0f, SEED + 1);
            const float fa = vnoise(x, y, 150.0f, SEED + 2);
            const float patch = vnoise(x, y, 46.0f, SEED + 23);
            float dens = 0.16f + fo * 0.62f + (fa < 0.4f ? 0.10f : -0.16f) +
                         (patch - 0.5f) * 0.45f;
            if (hf(gx, wyI, SEED + 24) > dens) continue;
            const std::int64_t px = static_cast<std::int64_t>(x) + kPadX;
            const std::int64_t py =
                kCanvasHN - 1 - (static_cast<std::int64_t>(std::floor(y)) - wyBase);
            if (px < 0 || py < 0 || px >= kCanvasWN || py >= kCanvasHN) continue;
            if (land.w[static_cast<std::size_t>(py) * kCanvasW + px] < 255) continue;
            if (roadMask[static_cast<std::size_t>(py) * kCanvasW + px]) continue;
            const float roll = hf(px, static_cast<std::int64_t>(y), SEED + 25);
            if (roll < 0.10f && fo > 0.50f &&
                (tryTree("env_tree_conifer_0", x, y, 7.0f) ||
                 tryTree("env_tree_conifer_1", x, y, 7.0f)))
                continue;
            if (roll < 0.16f &&
                (tryTree("env_tree_palm_0", x, y, 7.0f) ||
                 tryTree("env_tree_palm_1", x, y, 7.0f)))
                continue;
            if (roll < 0.26f && tryTree("env_bush_0", x, y, 3.0f)) continue;
            if (roll < 0.34f && tryTree("env_bush_1", x, y, 3.0f)) continue;
            if (roll < 0.55f &&
                (tryTree("env_tree_small_0", x, y, 5.0f) ||
                 tryTree("env_tree_small_1", x, y, 5.0f)))
                continue;
            if (roll < 0.78f) {
                tryTree("env_tree_broadleaf_0", x, y, 8.0f) ||
                    tryTree("env_tree_broadleaf_1", x, y, 8.0f) ||
                    tryTree("env_tree_broadleaf_2", x, y, 8.0f);
                continue;
            }
            tryTree("env_tree_broadleaf_1", x, y, 8.0f) ||
                tryTree("env_tree_broadleaf_2", x, y, 8.0f);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
bool EnvironmentBaker::init(Renderer& r) {
    renderer_ = &r;
    slotTex_.assign(kPoolSize, -1);
    static const std::uint32_t zero = 0;
    for (int i = 0; i < kPoolSize; ++i) {
        // Create every slot once as a transparent placeholder; bakes replace
        // its pixels (createTextureFromPixels destroys the replaced texture
        // and keeps the id stable).
        slotTex_[static_cast<std::size_t>(i)] = r.createTextureFromPixels(
            "env_chunk_" + std::to_string(i),
            reinterpret_cast<const std::uint8_t*>(&zero), kSpanW, kChunk);
        if (slotTex_[static_cast<std::size_t>(i)] < 0) return false;
    }
    ready_ = true;
    return true;
}

void EnvironmentBaker::clear() { cache_.clear(); }

std::uint64_t EnvironmentBaker::makeKey(int cx, int cy) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx))) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) << 32);
}

int EnvironmentBaker::get(Renderer& r, const StageDef& stage, int cx, int cy) {
    if (!ready_) return -1;
    const std::uint64_t key = makeKey(cx, cy);
    ++useClock_;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.lastUse = useClock_;
        return slotTex_[static_cast<std::size_t>(it->second.slot)];
    }
    if (frameBakes_ >= kBakesPerFrame) return -1;  // spread bakes over frames
    ++frameBakes_;
    const float cyW = (static_cast<float>(cy) + 0.5f) * kChunk;
    const TerrainKind kind = terrainAt(stage, cyW);
    if (kind != TerrainKind::Island && kind != TerrainKind::Jungle) return -1;

    // Contiguous composed-land segment bounds around this chunk.
    float lo = 0.0f, hi = 1e9f;
    {
        const std::size_t n = stage.terrain.size();
        std::size_t seg = 0;
        for (std::size_t s = 0; s < n; ++s)
            if (cyW >= stage.terrain[s].fromY) seg = s;
        lo = stage.terrain[seg].fromY;
        std::size_t e = seg;
        while (e + 1 < n &&
               (stage.terrain[e + 1].kind == TerrainKind::Island ||
                stage.terrain[e + 1].kind == TerrainKind::Jungle) &&
               stage.terrain[e + 1].fromY <= stage.terrain[e].toY + 1.0f)
            ++e;
        hi = e + 1 < n ? stage.terrain[e + 1].fromY : stage.terrain[e].toY;
    }

    if (cache_.size() >= kPoolSize) {  // LRU eviction
        auto victim = cache_.begin();
        for (auto i2 = cache_.begin(); i2 != cache_.end(); ++i2)
            if (i2->second.lastUse < victim->second.lastUse) victim = i2;
        cache_.erase(victim);
    }

    // ---- bake ----
    static std::unordered_map<std::string, int> sprId;
    if (sprId.empty()) {
        for (const char* n : kEnvSpriteNames) sprId.emplace(n, r.texture(n));
    }
    BakeCtx ctx{r, sprId};
    PixelBuf canvas(kCanvasW, kCanvasH);
    LandMask land;
    for (int py = 0; py < kCanvasH; ++py) {
        const float wy = static_cast<float>(cy) * kChunk - kPadY + (kCanvasH - 1 - py);
        if (wy < lo || wy >= hi) continue;
        float f = 1.0f;
        if (lo > 1.0f) {  // coastline below (water behind)
            const float d = wy - lo;
            if (d < 14.0f) f = 0.0f;
            else if (d < 30.0f)
                f = (d - 14.0f) / 16.0f +
                    (hf(0, static_cast<std::int64_t>(wy), SEED) - 0.5f) * 0.55f;
        }
        if (hi < 1e8f) {  // coastline above (water ahead)
            const float d = hi - wy;
            if (d < 14.0f) f = 0.0f;
            else if (d < 30.0f)
                f = std::min(f, (d - 14.0f) / 16.0f +
                                    (hf(7, static_cast<std::int64_t>(wy), SEED) - 0.5f) * 0.55f);
        }
        if (f <= 0.0f) continue;
        const int a = std::min(255, static_cast<int>(f * 255.0f + 0.5f));
        for (int px = 0; px < kCanvasW; ++px)
            land.w[static_cast<std::size_t>(py) * kCanvasW + px] =
                static_cast<std::uint8_t>(a);
    }

    runBake(ctx, canvas, land, cy, lo, hi, stage);

#ifndef NDEBUG
    if (const char* dd = SDL_getenv("WWII_DUMP_CHUNKS")) {
        std::error_code ec;
        std::filesystem::create_directories(dd, ec);
        // RGBA byte image of the padded bake canvas (row 0 = top).
        std::string path = std::string(dd) + "/chunk_" + std::to_string(cy) + ".ppm";
        std::ofstream o(path, std::ios::binary);
        o << "P6\n" << kCanvasW << " " << kCanvasH << "\n255\n";
        for (int y = 0; y < kCanvasH; ++y)
            for (int x = 0; x < kCanvasW; ++x) {
                const std::uint32_t c = canvas.get(x, y);
                const char b[3] = {static_cast<char>((c >> 24) & 0xFF),
                                   static_cast<char>((c >> 16) & 0xFF),
                                   static_cast<char>((c >> 8) & 0xFF)};
                o.write(b, 3);
            }
    }
#endif

    // Crop the centre chunk out of the padded canvas and upload.
    static std::vector<std::uint32_t> outPx(static_cast<std::size_t>(kSpanW) * kChunk);
    for (int py = 0; py < kChunk; ++py)
        for (int px = 0; px < kSpanW; ++px)
            outPx[static_cast<std::size_t>(py) * kSpanW + px] =
                canvas.get(px + kPadX, kPadY + py);
    const int slot = slotNext_;
    const int tex = r.createTextureFromPixels(
        "env_chunk_" + std::to_string(slot),
        reinterpret_cast<const std::uint8_t*>(outPx.data()), kSpanW, kChunk);
    if (tex < 0) return -1;
    slotTex_[static_cast<std::size_t>(slot)] = tex;
    Entry e;
    e.slot = slot;
    e.lo = lo;
    e.hi = hi;
    e.lastUse = useClock_;
    cache_[key] = e;
    slotNext_ = (slotNext_ + 1) % kPoolSize;
    return tex;
}

}  // namespace ww
