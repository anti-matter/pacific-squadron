#include "graphics/BitmapFont.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ww {

// ---------------------------------------------------------------------------
// Arcade stroke font.
//
// Glyphs are defined as vector strokes (polylines + elliptical arcs) on a
// 7x11 pixel cell with a 6px design width, a cap line at y=1 and a baseline
// at y=9. Every stroke is rasterized at 8x supersampling with a constant
// pen weight and round caps/joins; per-pixel coverage becomes the glyph
// alpha. The result is inherently smooth curves, consistent stroke weight
// and clean diagonals — no post-filtering of a coarse bitmap.
//
// Lowercase letters are mapped onto the uppercase glyphs (classic arcade
// behaviour), so the whole printable ASCII set stays visually cohesive.
// ---------------------------------------------------------------------------
namespace {

constexpr int kCellW = 7;
constexpr int kCellH = 11;
constexpr int kCount = 95;  // ASCII 32..126
constexpr float kPen = 1.35f;   // stroke weight in cells

struct Pt { float x, y; };
struct Arc { float cx, cy, rx, ry, a0, a1; };  // degrees; may decrease

// One glyph's worth of vector geometry.
struct GlyphShape {
    std::vector<std::vector<Pt>> polys;   // stroked polylines
    std::vector<Arc> arcs;                // stroked arcs
    std::vector<Pt> dots;                 // filled dots (round, r = pen*0.62)
    float dotR = 0.9f;
};

// Poly helpers: keep the table readable.
inline std::vector<Pt> P(std::initializer_list<float> xy) {
    std::vector<Pt> v;
    auto it = xy.begin();
    while (it != xy.end()) {
        const float x = *it;
        ++it;
        if (it == xy.end()) break;
        const float y = *it;
        ++it;
        v.push_back({x, y});
    }
    return v;
}
inline Arc A(float cx, float cy, float rx, float ry, float a0, float a1) {
    return {cx, cy, rx, ry, a0, a1};
}

// Convert an arc to a dense polyline (samples every ~7 degrees max).
std::vector<Pt> flatten(const Arc& a) {
    const double d0 = static_cast<double>(a.a0) * 3.14159265358979 / 180.0;
    const double d1 = static_cast<double>(a.a1) * 3.14159265358979 / 180.0;
    const double span = d1 - d0;
    int n = static_cast<int>(std::ceil(std::fabs(span) / (7.0 * 3.14159265358979 / 180.0)));
    n = std::max(2, std::min(48, n));
    std::vector<Pt> pts;
    for (int i = 0; i <= n; ++i) {
        const double t = d0 + span * i / n;
        pts.push_back({static_cast<float>(a.cx + a.rx * std::cos(t)),
                       static_cast<float>(a.cy + a.ry * std::sin(t))});
    }
    return pts;
}

// The glyph table. Coordinates: x 0..6.4, cap top y=1, baseline y=9.
GlyphShape glyphShape(int ch) {
    GlyphShape g;
    switch (ch) {
        case ' ': break;
        case '!': g.polys = {P({3.2f,1,3.2f,6.3f})}; g.dots = {{3.2f,8.6f}}; break;
        case '"': g.polys = {P({2.2f,1,2.1f,2.7f}), P({4.3f,1,4.2f,2.7f})}; break;
        case '#': g.polys = {P({2.5f,1.4f,1.8f,9}), P({5.2f,1.4f,4.5f,9}),
                             P({1.0f,4.1f,6.1f,4.1f}), P({1.3f,6.5f,6.4f,6.5f})}; break;
        case '$': g = glyphShape('S'); g.polys.push_back(P({3.2f,0.3f,3.2f,9.7f})); break;
        case '%': g.polys = {P({5.7f,1.4f,0.9f,8.8f})};
                  g.arcs = {A(2.0f,2.6f,1.1f,1.4f,-90,270), A(4.5f,6.6f,1.1f,1.4f,-90,270)}; break;
        case '\'': g.polys = {P({3.2f,1,3.1f,2.7f})}; break;
        case '(': g.arcs = {A(5.9f,5,2.9f,4.3f,132,228)}; break;
        case ')': g.arcs = {A(0.7f,5,2.9f,4.3f,-48,48)}; break;
        case '*': g.polys = {P({3.2f,2.4f,3.2f,5.8f}), P({1.4f,3.2f,5.0f,5.0f}),
                             P({5.0f,3.2f,1.4f,5.0f})}; break;
        case '+': g.polys = {P({3.2f,2.6f,3.2f,6.8f}), P({1.1f,4.7f,5.3f,4.7f})}; break;
        case ',': g.dots = {{3.1f,8.6f}}; g.polys = {P({3.4f,8.9f,2.6f,10.4f})}; break;
        case '-': g.polys = {P({1.3f,4.9f,5.1f,4.9f})}; break;
        case '.': g.dots = {{3.2f,8.6f}}; break;
        case '/': g.polys = {P({5.6f,1,1.0f,9})}; break;
        case '0': g.arcs = {A(3.2f,5,2.7f,4.0f,-90,270)};
                  g.polys = {P({1.7f,7.5f,4.7f,2.5f})}; break;
        case '1': g.polys = {P({1.7f,2.4f,3.4f,1,3.4f,9}), P({1.9f,9,4.9f,9})}; break;
        case '2': g.arcs = {A(3.2f,3.1f,2.3f,2.1f,-176,12)};
                  g.polys = {P({5.45f,3.55f,1.1f,9,5.9f,9})}; break;
        case '3': g.arcs = {A(3.0f,3.0f,2.2f,2.0f,-90,90), A(3.0f,6.9f,2.3f,2.1f,-90,96)}; break;
        case '4': g.polys = {P({4.5f,1,0.8f,6.4f,6.2f,6.4f}), P({4.5f,1,4.5f,9})}; break;
        case '5': g.polys = {P({5.6f,1,1.6f,1,1.45f,4.5f})};
                  g.arcs = {A(3.2f,6.8f,2.3f,2.2f,-100,122)}; break;
        // 6/9: bowl plus an outward-sweeping tail that reaches the cap/baseline.
        case '6': g.arcs = {A(3.2f,6.5f,2.4f,2.5f,-90,270),
                            A(3.0f,4.6f,2.5f,3.6f,-90,-190)}; break;
        case '7': g.polys = {P({0.9f,1,6.0f,1,3.0f,9})}; break;
        case '8': g.arcs = {A(3.2f,3.0f,2.1f,2.0f,-90,270), A(3.2f,6.9f,2.3f,2.1f,-90,270)}; break;
        case '9': g.arcs = {A(3.2f,3.5f,2.4f,2.5f,-90,270),
                            A(3.4f,5.4f,2.5f,3.6f,90,-10)}; break;
        case ':': g.dots = {{3.2f,3.4f},{3.2f,8.4f}}; break;
        case ';': g.dots = {{3.1f,3.4f},{3.1f,8.6f}}; g.polys = {P({3.4f,8.9f,2.6f,10.4f})}; break;
        case '<': g.polys = {P({5.6f,2.3f,1.4f,4.9f,5.6f,7.5f})}; break;
        case '=': g.polys = {P({1.3f,3.9f,5.1f,3.9f}), P({1.3f,5.9f,5.1f,5.9f})}; break;
        case '>': g.polys = {P({0.9f,2.3f,5.1f,4.9f,0.9f,7.5f})}; break;
        case '?': g.arcs = {A(3.2f,3.1f,2.2f,2.1f,-90,112)};
                  g.polys = {P({2.4f,4.9f,3.2f,6.4f})}; g.dots = {{3.2f,8.6f}}; break;
        case '@': g.arcs = {A(3.2f,5,2.9f,4.0f,-90,270), A(3.2f,5,1.2f,1.9f,-90,270)};
                  g.polys = {P({4.4f,3.2f,4.4f,6.1f})}; break;
        case 'A': g.polys = {P({0.5f,9,3.2f,1,5.9f,9}), P({1.55f,6.2f,4.85f,6.2f})}; break;
        case 'B': g.polys = {P({1.1f,1,1.1f,9}), P({1.1f,1.0f,3.2f,1.0f}),
                             P({1.1f,4.8f,3.2f,4.8f}), P({1.1f,9.0f,3.4f,9.0f})};
                  g.arcs = {A(3.2f,2.9f,1.9f,1.9f,-90,90),
                            A(3.4f,6.9f,2.1f,2.1f,-90,90)}; break;
        case 'C': g.arcs = {A(3.4f,5,2.9f,4.0f,56,304)}; break;
        case 'D': g.polys = {P({1.1f,1,1.1f,9}), P({1.1f,1.0f,3.3f,1.0f}),
                             P({1.1f,9.0f,3.3f,9.0f})};
                  g.arcs = {A(3.3f,5.0f,3.0f,4.0f,-90,90)}; break;
        case 'E': g.polys = {P({5.8f,1,1.1f,1,1.1f,9,5.8f,9}), P({1.1f,5,5.0f,5})}; break;
        case 'F': g.polys = {P({5.8f,1,1.1f,1,1.1f,9}), P({1.1f,5,4.9f,5})}; break;
        case 'G': g.arcs = {A(3.4f,5,2.9f,4.0f,56,304)};
                  g.polys = {P({5.9f,5.4f,3.6f,5.4f}), P({5.9f,5.4f,5.9f,7.6f})}; break;
        case 'H': g.polys = {P({1.1f,1,1.1f,9}), P({5.3f,1,5.3f,9}), P({1.1f,5,5.3f,5})}; break;
        case 'I': g.polys = {P({3.2f,1,3.2f,9}), P({1.9f,1,4.5f,1}), P({1.9f,9,4.5f,9})}; break;
        case 'J': g.polys = {P({5.4f,1,5.4f,6.6f})};
                  g.arcs = {A(3.3f,6.6f,2.1f,2.3f,0,180)}; break;
        case 'K': g.polys = {P({1.1f,1,1.1f,9}), P({5.6f,1,1.5f,5.1f}), P({3.0f,4.2f,5.8f,9})}; break;
        case 'L': g.polys = {P({1.2f,1,1.2f,9,5.8f,9})}; break;
        case 'M': g.polys = {P({0.3f,9,0.4f,1,3.2f,5.4f,6.0f,1,6.1f,9})}; break;
        case 'N': g.polys = {P({1.1f,9,1.1f,1,5.5f,9,5.5f,1})}; break;
        case 'O': g.arcs = {A(3.2f,5,2.9f,4.0f,-90,270)}; break;
        case 'P': g.polys = {P({1.1f,9,1.1f,1}), P({1.1f,1.0f,3.7f,1.0f}),
                             P({1.1f,4.6f,3.7f,4.6f})};
                  g.arcs = {A(3.7f,2.8f,1.8f,1.8f,-90,90)}; break;
        case 'Q': g.arcs = {A(3.2f,5,2.9f,4.0f,-90,270)};
                  g.polys = {P({3.9f,6.6f,5.9f,9.4f})}; break;
        case 'R': g.polys = {P({1.1f,9,1.1f,1}), P({1.1f,1.0f,3.7f,1.0f}),
                             P({1.1f,4.6f,3.7f,4.6f}), P({3.0f,4.6f,5.9f,9})};
                  g.arcs = {A(3.7f,2.8f,1.8f,1.8f,-90,90)}; break;
        case 'S': g.arcs = {A(3.2f,3.05f,2.2f,2.05f,-38,-270), A(3.2f,6.95f,2.2f,2.05f,-90,184)}; break;
        case 'T': g.polys = {P({0.6f,1,5.8f,1}), P({3.2f,1,3.2f,9})}; break;
        case 'U': g.polys = {P({1.0f,1,1.0f,5.6f}), P({5.4f,5.6f,5.4f,1})};
                  g.arcs = {A(3.2f,5.6f,2.2f,3.4f,180,0)}; break;
        case 'V': g.polys = {P({0.4f,1,3.2f,9,6.0f,1})}; break;
        case 'W': g.polys = {P({0.0f,1,1.6f,9,3.2f,4.2f,4.8f,9,6.4f,1})}; break;
        case 'X': g.polys = {P({0.8f,1,5.6f,9}), P({5.6f,1,0.8f,9})}; break;
        case 'Y': g.polys = {P({0.7f,1,3.2f,5.0f}), P({5.7f,1,3.2f,5.0f,3.2f,9})}; break;
        case 'Z': g.polys = {P({0.9f,1,5.9f,1,1.0f,9,6.0f,9})}; break;
        case '[': g.arcs = {A(5.4f,5,3.2f,4.4f,138,222)}; break;
        case '\\': g.polys = {P({1.0f,1,5.6f,9})}; break;
        case ']': g.arcs = {A(1.0f,5,3.2f,4.4f,-42,42)}; break;
        case '^': g.polys = {P({1.6f,3.2f,3.2f,1.2f,4.8f,3.2f})}; break;
        case '_': g.polys = {P({0.8f,10.3f,5.6f,10.3f})}; break;
        case '`': g.polys = {P({2.6f,1.2f,3.6f,2.8f})}; break;
        case '{': g.arcs = {A(4.6f,2.8f,1.6f,2.2f,120,240), A(4.6f,7.2f,1.6f,2.2f,-60,60)}; break;
        case '|': g.polys = {P({3.2f,1,3.2f,9})}; break;
        case '}': g.arcs = {A(1.8f,2.8f,1.6f,2.2f,-60,60), A(1.8f,7.2f,1.6f,2.2f,120,240)}; break;
        case '~': g.polys = {P({1.0f,4.4f,2.4f,3.4f,4.0f,4.6f,5.4f,3.6f})}; break;
        default:  // unknown glyph: hollow box so missing art is visible.
            g.arcs = {}; g.polys = {P({1.1f,1.6f,5.3f,1.6f,5.3f,8.4f,1.1f,8.4f,1.1f,1.6f})};
            break;
    }
    return g;
}

// Rasterize one glyph into coverage values for a cell of
// (kCellW*sc) x (kCellH*sc) atlas pixels. Samples are mapped back into the
// glyph's base coordinate space, so shapes are resolution-independent.
//
// Hot path notes: the bake cost is dominated by samples that miss the
// strokes, so every segment carries a precomputed bounding box and the
// inside test compares squared distances (no sqrt). Pixels outside every
// segment/dot bbox reject on a handful of float compares, and the cell's
// rows/columns outside the union of all bboxes are skipped entirely.
void rasterGlyph(int ch, float sc, int ss, float* cov) {
    GlyphShape g = glyphShape(ch);

    struct Seg {
        float ax, ay, abx, aby, len2;
        float minx, maxx, miny, maxy;
    };
    struct Dot {
        float x, y, r2;
        float minx, maxx, miny, maxy;
    };
    std::vector<Seg> segs;
    std::vector<Dot> dots;
    const float r = kPen * 0.5f;

    auto addPts = [&](const std::vector<Pt>& s) {
        for (std::size_t i = 0; i + 1 < s.size(); ++i) {
            const Pt& a = s[i];
            const Pt& b = s[i + 1];
            const float abx = b.x - a.x, aby = b.y - a.y;
            segs.push_back({a.x, a.y, abx, aby, abx * abx + aby * aby,
                            std::min(a.x, b.x) - r, std::max(a.x, b.x) + r,
                            std::min(a.y, b.y) - r, std::max(a.y, b.y) + r});
        }
    };
    for (const auto& p : g.polys) addPts(p);
    for (const auto& a : g.arcs) addPts(flatten(a));
    for (const Pt& d : g.dots)
        dots.push_back({d.x, d.y, g.dotR * g.dotR, d.x - g.dotR, d.x + g.dotR,
                        d.y - g.dotR, d.y + g.dotR});

    const int cw = static_cast<int>(std::lround(kCellW * sc));
    const int chh = static_cast<int>(std::lround(kCellH * sc));
    std::memset(cov, 0, static_cast<std::size_t>(cw) * chh * sizeof(float));
    if (segs.empty() && dots.empty()) return;  // space: all zero

    // Union of all bboxes, in cell-fraction coordinates, for the row/col
    // skip range.
    float ux0 = 1e9f, ux1 = -1e9f, uy0 = 1e9f, uy1 = -1e9f;
    for (const Seg& s : segs) {
        ux0 = std::min(ux0, s.minx); ux1 = std::max(ux1, s.maxx);
        uy0 = std::min(uy0, s.miny); uy1 = std::max(uy1, s.maxy);
    }
    for (const Dot& d : dots) {
        ux0 = std::min(ux0, d.minx); ux1 = std::max(ux1, d.maxx);
        uy0 = std::min(uy0, d.miny); uy1 = std::max(uy1, d.maxy);
    }
    const int px0 = std::max(0, static_cast<int>(std::floor(ux0 * sc)));
    const int px1 = std::min(cw - 1, static_cast<int>(std::ceil(ux1 * sc)));
    const int py0 = std::max(0, static_cast<int>(std::floor(uy0 * sc)));
    const int py1 = std::min(chh - 1, static_cast<int>(std::ceil(uy1 * sc)));

    const float inv = 1.0f / sc;
    const float r2 = r * r;
    for (int py = py0; py <= py1; ++py) {
        float* row = cov + static_cast<std::size_t>(py) * cw;
        for (int px = px0; px <= px1; ++px) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy) {
                const float y = (py + (sy + 0.5f) / ss) * inv;
                for (int sx = 0; sx < ss; ++sx) {
                    const float x = (px + (sx + 0.5f) / ss) * inv;
                    bool in = false;
                    for (const Seg& s : segs) {
                        if (x < s.minx || x > s.maxx || y < s.miny ||
                            y > s.maxy)
                            continue;
                        float t = s.len2 > 1e-9f
                                      ? ((x - s.ax) * s.abx + (y - s.ay) * s.aby) /
                                            s.len2
                                      : 0.0f;
                        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                        const float dx = x - (s.ax + s.abx * t),
                                   dy = y - (s.ay + s.aby * t);
                        if (dx * dx + dy * dy <= r2) { in = true; break; }
                    }
                    if (!in) {
                        for (const Dot& d : dots) {
                            if (x < d.minx || x > d.maxx || y < d.miny ||
                                y > d.maxy)
                                continue;
                            const float dx = x - d.x, dy = y - d.y;
                            if (dx * dx + dy * dy <= d.r2) { in = true; break; }
                        }
                    }
                    if (in) ++hits;
                }
            }
            row[px] = static_cast<float>(hits) / static_cast<float>(ss * ss);
        }
    }
}

}  // namespace

int buildFontAtlas(Renderer& renderer, float px) {
    const int ss = px > 1.5f ? 4 : 8;  // keep per-bake cost bounded
    const int cw = static_cast<int>(std::lround(kCellW * px));
    const int chh = static_cast<int>(std::lround(kCellH * px));
    std::vector<std::uint32_t> atlas(static_cast<std::size_t>(kCount * cw) * chh, 0);
    std::vector<float> cov(static_cast<std::size_t>(cw) * chh);
    for (int ch = 32; ch <= 126; ++ch) {
        // Lowercase -> uppercase (arcade style), keeping the set cohesive.
        int src = (ch >= 'a' && ch <= 'z') ? ch - ('a' - 'A') : ch;
        rasterGlyph(src, px, ss, cov.data());
        for (int y = 0; y < chh; ++y) {
            for (int x = 0; x < cw; ++x) {
                int a = static_cast<int>(cov[y * cw + x] * 255.0f + 0.5f);
                if (a < 10) a = 0;                       // kill faint dust
                else if (a > 244) a = 255;               // snap solid interiors
                atlas[static_cast<std::size_t>(y) * kCount * cw +
                      static_cast<std::size_t>(ch - 32) * cw + x] =
                    0xFFFFFF00u | static_cast<std::uint32_t>(a);   // 0xRRGGBBAA
            }
        }
    }
    const std::string name =
        "arcade_font_" + std::to_string(static_cast<int>(std::lround(px * 16.0f)));
    return renderer.createTextureFromPixels(
        name, reinterpret_cast<const std::uint8_t*>(atlas.data()), kCount * cw, chh);
}

}  // namespace ww
