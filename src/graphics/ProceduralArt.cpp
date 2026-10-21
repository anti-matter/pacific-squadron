#include "graphics/ProceduralArt.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

#ifndef NDEBUG
#include <SDL3/SDL.h>
#include "stb_image_write.h"
#endif

#include "core/Rng.h"

namespace ww {
namespace {

#ifndef NDEBUG
// Debug hook shared by the WWII_DUMP_SPRITES dumps: writes a PixelBuf as an
// RGBA PNG for offline pixel inspection (development tool only).
void dumpSpriteDbg(const std::string& name, const PixelBuf& p) {
    const char* dumpDir = SDL_getenv("WWII_DUMP_SPRITES");
    if (!dumpDir) return;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(p.w) * p.h * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        const std::uint32_t v = p.px[i / 4];
        rgba[i] = static_cast<std::uint8_t>(v >> 24);
        rgba[i + 1] = static_cast<std::uint8_t>(v >> 16);
        rgba[i + 2] = static_cast<std::uint8_t>(v >> 8);
        rgba[i + 3] = static_cast<std::uint8_t>(v);
    }
    const std::string path = std::string(dumpDir) + "/" + name + ".png";
    stbi_write_png(path.c_str(), p.w, p.h, 4, rgba.data(), p.w * 4);
}
#endif

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
// Player P-38 drab, sampled from the reference (assets/P-38.png): a warm,
// slightly yellow olive with a broad light band and a deep green shadow.
const std::uint32_t kDrab = col(0x686C40);
const std::uint32_t kDrabHi = col(0x8B8F5E);
const std::uint32_t kDrabDk = col(0x444728);
const std::uint32_t kInsigniaBlue = col(0x24407A);
const std::uint32_t kWhite = col(0xF2F2F2);

const std::uint32_t kEnGreen = col(0x4E5A38);
const std::uint32_t kEnGreenDk = col(0x38422A);
const std::uint32_t kEnYellow = col(0xC8A83C);
const std::uint32_t kEnGrey = col(0x5A5F63);
const std::uint32_t kEnGreyDk = col(0x40444A);
const std::uint32_t kConcrete = col(0x8C8C80);
const std::uint32_t kConcreteDk = col(0x6A6A60);

std::uint32_t shade(std::uint32_t c, float f) {
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
        const int y0 = static_cast<int>((std::min(ay, by) - r) * SS), y1 = static_cast<int>((std::max(ay, by) + r) * SS) + 1;
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
void outlineSilhouette(PixelBuf& p, std::uint32_t ink) {
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
void softBlendEllipse(PixelBuf& p, float cx, float cy, float rx, float ry,
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

// Re-anti-alias the two vertical silhouette borders of a hull built from a
// half-width profile: each border pixel's alpha is re-derived analytically
// from the continuous profile curve (not filtered from neighbouring pixels),
// so the flank contours read as smooth sweeps instead of staircases.
template <typename Fn>
void profileRim(PixelBuf& p, float cx, int y0, int y1, Fn halfAt) {
    for (int y = y0; y <= y1; ++y) {
        const float hw = halfAt(y);
        for (const int side : {-1, 1}) {
            const float xb = cx + side * hw;         // continuous boundary
            const int xi = static_cast<int>(std::lround(xb));
            if (!(p.get(xi, y) & 0xFF)) continue;    // gap/foam, not hull
            if (p.get(xi + side, y) & 0xFF) continue;  // interior, not border
            const float cov = (side > 0) ? (xb + 0.5f - xi) : (xi + 0.5f - xb);
            if (cov > 0.97f) continue;
            const std::uint32_t c = p.get(xi, y);
            int a = static_cast<int>(cov * 255.0f + 0.5f);
            if (a < 40) a = 40;
            p.set(xi, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
        }
    }
}

// ---------------------------------------------------------------------------
// Aircraft builder (drawn nose-up, mirrored for downward-facing enemies)
// ---------------------------------------------------------------------------
struct PlaneStyle {
    int span;          // total wingspan in px
    int len;           // fuselage length
    std::uint32_t body, bodyHi, bodyDk;
    bool twin = false; // twin engine
    bool redDot = false; // enemy marking
    bool amberMark = false; // elite accent (amber nose band + wingtip caps)
};

PixelBuf makePlane(const PlaneStyle& st, int bankRows = 0) {
    const int W = 52, H = 52;
    const float cx = 26.0f, cy = 26.0f;
    const float half = st.span * 0.5f;
    const float lenH = st.len * 0.5f;
    Hi hi(W, H);

    // --- wings (painted first; the fuselage overlaps them) ----------------
    const float tipLift = 1.2f;  // built-in sweep: tips sit slightly aft
    for (int side = -1; side <= 1; side += 2) {
        const float tipShift = bankRows * side;
        const float rootLE = cy - 2.6f, rootTE = cy + 5.4f;
        const float tipLE = cy + tipLift + tipShift;
        const float tipTE = tipLE + std::max(2.2f, half * 0.24f);
        hi.fillPoly({{cx, rootLE}, {cx + side * half * 0.5f, rootLE + 0.6f},
                     {cx + side * half, tipLE}, {cx + side * half, tipTE},
                     {cx + side * half * 0.55f, rootTE}, {cx, rootTE}},
                    st.body);
        // Leading-edge highlight and trailing-edge shading.
        hi.stroke({{cx + side * 0.6f, rootLE + 0.15f},
                   {cx + side * half * 0.5f, rootLE + 0.75f},
                   {cx + side * (half - 0.4f), tipLE + 0.25f}},
                  1.0f, st.bodyHi);
        hi.stroke({{cx + side * half * 0.62f, rootTE - 0.4f},
                   {cx + side * (half - 0.5f), tipTE - 0.3f}},
                  0.9f, st.bodyDk);
        // Aileron seam outboard.
        hi.stroke({{cx + side * half * 0.72f, tipLE + 1.1f + (tipTE - tipLE) * 0.15f},
                   {cx + side * (half - 0.7f), tipLE + 1.1f + (tipTE - tipLE) * 0.2f}},
                  0.55f, st.bodyDk);
        if (st.amberMark)  // elite wingtip cap
            hi.fillPoly({{cx + side * (half - 1.6f), tipLE},
                         {cx + side * half, tipLE}, {cx + side * half, tipTE},
                         {cx + side * (half - 1.6f), tipTE}}, col(0xE8A832));
    }

    // --- fuselage: tapered capsule ----------------------------------------
    {
        std::vector<std::pair<float, float>> pts;
        const float hwMax = st.twin ? 3.6f : 2.6f;
        for (int i = 0; i <= 10; ++i) {  // right side, nose -> tail
            const float t = i / 10.0f;
            const float hw = (t < 0.12f) ? 0.7f + hwMax * (t / 0.12f) * 0.9f
                               : (t < 0.72f) ? hwMax
                                             : hwMax * (1.0f - 0.55f * (t - 0.72f) / 0.28f);
            pts.push_back({cx + hw, cy - lenH + st.len * t});
        }
        for (int i = 10; i >= 0; --i) {  // left side, tail -> nose
            const float t = i / 10.0f;
            const float hw = (t < 0.12f) ? 0.7f + hwMax * (t / 0.12f) * 0.9f
                               : (t < 0.72f) ? hwMax
                                             : hwMax * (1.0f - 0.55f * (t - 0.72f) / 0.28f);
            pts.push_back({cx - hw * 0.96f, cy - lenH + st.len * t});
        }
        hi.fillPoly(pts, st.body);
        // Soft top-light: bright spine left, shaded flank right.
        hi.stroke({{cx - hwMax * 0.45f, cy - lenH + 2.0f}, {cx - hwMax * 0.55f, cy + lenH * 0.55f}},
                  1.05f, st.bodyHi);
        hi.stroke({{cx + hwMax * 0.62f, cy - lenH + 2.2f}, {cx + hwMax * 0.5f, cy + lenH * 0.6f}},
                  1.05f, st.bodyDk);
    }

    // --- engine cowling + prop blur ----------------------------------------
    const float noseY = cy - lenH;
    hi.fillEllipse(cx, noseY + 3.1f, st.twin ? 2.4f : 2.9f, 3.4f, st.bodyDk);
    hi.fillEllipse(cx, noseY + 2.4f, st.twin ? 1.5f : 1.9f, 2.0f, col(0x1C2126));
    hi.fillEllipse(cx, noseY + 1.7f, 2.6f, 1.0f, col(0xC8D0D8, 90));  // prop disc
    hi.fillEllipse(cx, noseY + 1.6f, 0.9f, 0.9f, col(0xE8EEF4));      // spinner
    if (st.amberMark)  // elite nose band
        hi.stroke({{cx - 2.4f, noseY + 5.4f}, {cx + 2.4f, noseY + 5.4f}}, 1.5f, col(0xE8A832));

    // --- twin nacelles --------------------------------------------------------
    if (st.twin) {
        const float nx = half * 0.5f;
        for (int side = -1; side <= 1; side += 2) {
            const float ex = cx + side * nx;
            hi.fillRoundRect(ex - 2.3f, cy - 7.4f, 4.6f, 12.0f, 2.1f, st.body);
            hi.fillEllipse(ex, cy - 6.2f, 1.9f, 1.9f, st.bodyDk);
            hi.fillEllipse(ex, cy - 7.0f, 1.6f, 0.8f, col(0xC8D0D8, 90));  // prop blur
            hi.fillEllipse(ex, cy - 7.4f, 0.7f, 0.7f, col(0x20262C));      // hub
            hi.stroke({{ex - 1.4f, cy - 4.0f}, {ex - 1.4f, cy + 3.4f}}, 0.8f, st.bodyHi);
        }
    }

    // --- tailplane + fin -------------------------------------------------------
    {
        const float tailY = cy + lenH - 5.5f;
        const float tl = st.span * 0.20f;
        hi.fillPoly({{cx, tailY - 1.6f}, {cx + tl, tailY - 0.4f}, {cx + tl, tailY + 1.8f},
                     {cx, tailY + 2.6f}, {cx - tl, tailY + 1.8f}, {cx - tl, tailY - 0.4f}},
                    st.body);
        hi.stroke({{cx - tl * 0.9f, tailY - 0.3f}, {cx + tl * 0.9f, tailY - 0.3f}}, 0.7f, st.bodyHi);
        hi.fillPoly({{cx - 0.9f, tailY - 1.0f}, {cx + 0.9f, tailY - 1.0f},
                     {cx + 1.3f, tailY + 4.6f}, {cx - 0.6f, tailY + 4.6f}}, st.bodyDk);
    }

    // --- cockpit ---------------------------------------------------------------
    hi.fillEllipse(cx, cy - 0.8f, 1.9f, 3.3f, col(0x1E2A36));
    hi.fillEllipse(cx - 0.55f, cy - 1.7f, 0.9f, 1.7f, col(0x9FD0E8, 215));  // glass gloss
    hi.stroke({{cx - 1.9f, cy - 0.8f}, {cx + 1.9f, cy - 0.8f}}, 0.5f, col(0x141A20, 160));

    // --- markings ---------------------------------------------------------------
    if (st.redDot) {
        for (int side = -1; side <= 1; side += 2) {
            const float mx = cx + side * half * 0.62f, my = cy + 2.1f + tipLift * 0.62f;
            hi.fillEllipse(mx, my, 1.9f, 1.9f, col(0xE8E4DC));
            hi.fillEllipse(mx, my, 1.35f, 1.35f, col(0xC23A2A));
        }
    }

    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x0A0E12, 165));
    return p;
}

PixelBuf cropToContent(const PixelBuf& src, int pad = 1) {
    int x0 = src.w, y0 = src.h, x1 = -1, y1 = -1;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if (src.get(x, y) & 0xFF) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    if (x1 < 0) return PixelBuf(1, 1);
    x0 = std::max(0, x0 - pad); y0 = std::max(0, y0 - pad);
    x1 = std::min(src.w - 1, x1 + pad); y1 = std::min(src.h - 1, y1 + pad);
    PixelBuf out(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) out.set(x - x0, y - y0, src.get(x, y));
    return out;
}

void flipY(PixelBuf& p) {
    for (int y = 0; y < p.h / 2; ++y)
        for (int x = 0; x < p.w; ++x)
            std::swap(p.px[static_cast<std::size_t>(y) * p.w + x],
                      p.px[static_cast<std::size_t>(p.h - 1 - y) * p.w + x]);
}

// ---------------------------------------------------------------------------
// Mitsubishi A6M "Zero" — top view, nose-up (the primary enemy fighter, and
// the styling basis shared with the yellow interceptor).
//
// Design cues follow the TOP VIEW plate of zero.png: long slim fuselage
// tapering to a point, compact dark engine cowl ring with a bright spinner
// cone, broad rounded elliptical wings (leading edge sweeps back gently,
// trailing edge sweeps forward harder) with the folded-wingtip seam the
// A6M2 carried, red hinomaru outboard, a long multi-framed greenhouse
// canopy with a pale gloss set just aft of the wing, rounded horizontal
// stabilizers, and a red band on the aft fuselage. Built on the shared Hi
// supersampled raster with the house outline, so it belongs to the same
// art language as the P-38 and the bosses. The prop plane sits at
// noseY + kZeroPropDy; the animated spin is the pulsing prop_blur_e
// streak the renderer overlays there.
// ---------------------------------------------------------------------------
constexpr float kZeroPropDy = 1.4f;  // prop plane ahead of the cowl, nose-up

struct ZeroStyle {
    float span, len;
    std::uint32_t body, bodyHi, bodyDk;
    bool hinomaru = true;
};

PixelBuf makeZero(const ZeroStyle& z) {
    const int W = 52, H = 52;
    const float cx = 26.0f;
    const float noseY = 26.0f - z.len * 0.5f, tailY = 26.0f + z.len * 0.5f;
    const float half = z.span * 0.5f;
    Hi hi(W, H);
    const std::uint32_t body = z.body, bodyHi = z.bodyHi, bodyDk = z.bodyDk;

    // --- wings: elliptical planform with rounded tips -----------------------
    const float wy = 24.5f;  // wing band centre (sits slightly forward)
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        hi.fillPoly({{cx, wy - 4.6f},
                     {cx + s * 4.0f, wy - 4.8f}, {cx + s * 9.0f, wy - 4.6f},
                     {cx + s * 13.5f, wy - 3.9f}, {cx + s * (half - 2.5f), wy - 2.7f},
                     {cx + s * (half - 0.7f), wy - 1.2f}, {cx + s * half, wy + 0.3f},
                     {cx + s * (half - 0.8f), wy + 1.6f}, {cx + s * (half - 2.4f), wy + 2.4f},
                     {cx + s * 12.0f, wy + 3.4f}, {cx + s * 7.0f, wy + 4.6f},
                     {cx + s * 3.0f, wy + 5.6f}, {cx, wy + 6.0f}},
                    body);
        // Leading-edge highlight and trailing-edge shade follow the curve.
        hi.stroke({{cx + s * 1.0f, wy - 4.7f}, {cx + s * 9.0f, wy - 4.5f},
                   {cx + s * 14.0f, wy - 3.7f}, {cx + s * (half - 1.6f), wy - 1.6f}},
                  0.85f, bodyHi);
        hi.stroke({{cx + s * 3.0f, wy + 5.5f}, {cx + s * 8.0f, wy + 4.3f},
                   {cx + s * 13.0f, wy + 3.0f}, {cx + s * (half - 1.8f), wy + 1.5f}},
                  0.85f, bodyDk);
        // Folded-wingtip seam (A6M2 carry surface) and aileron seam.
        hi.stroke({{cx + s * (half - 3.6f), wy - 2.6f},
                   {cx + s * (half - 3.6f), wy + 1.9f}}, 0.5f, bodyDk);
        hi.stroke({{cx + s * 14.5f, wy + 2.3f}, {cx + s * 8.0f, wy + 3.9f}},
                  0.45f, bodyDk);
        // Hinomaru outboard: dark rim disc plus the bright centre.
        if (z.hinomaru) {
            hi.fillEllipse(cx + s * 11.8f, wy, 2.9f, 2.9f, col(0x9E2620));
            hi.fillEllipse(cx + s * 11.8f, wy, 2.6f, 2.6f, col(0xC8302A));
        }
    }

    // --- fuselage: long, slim, tapering to a point --------------------------
    {
        auto hwAt = [](float t) {  // t: 0 nose .. 1 tail
            if (t < 0.06f) return 0.2f + t * 21.7f;
            if (t < 0.22f) return 1.5f + (t - 0.06f) / 0.16f * 0.6f;
            if (t < 0.62f) return 2.1f;
            return 2.1f - 1.8f * (t - 0.62f) / 0.38f;
        };
        std::vector<std::pair<float, float>> pts;
        for (int i = 0; i <= 24; ++i)
            pts.push_back({cx + hwAt(i / 24.0f), noseY + z.len * i / 24.0f});
        for (int i = 24; i >= 0; --i)
            pts.push_back({cx - hwAt(i / 24.0f), noseY + z.len * i / 24.0f});
        hi.fillPoly(pts, body);
        // Top-light spine, shaded right flank.
        hi.stroke({{cx - 0.85f, noseY + 6.0f}, {cx - 1.0f, tailY - 9.0f}},
                  0.7f, bodyHi);
        hi.stroke({{cx + 1.0f, noseY + 6.0f}, {cx + 0.9f, tailY - 11.0f}},
                  0.7f, bodyDk);
    }

    // --- cowl, exhausts and spinner -----------------------------------------
    hi.fillEllipse(cx, noseY + 3.1f, 2.25f, 3.5f, col(0x33393E));   // cowl ring
    hi.fillEllipse(cx, noseY + 2.0f, 1.95f, 1.8f, col(0x232A2F));   // front face
    hi.stroke({{cx - 1.9f, noseY + 4.4f}, {cx - 1.55f, noseY + 1.5f}},
              0.5f, col(0x6A7278));                                 // cowl glint
    for (int k = 0; k < 3; ++k)  // exhaust stubs, right flank of the cowl
        hi.stroke({{cx + 1.5f, noseY + 3.6f + k * 1.4f},
                   {cx + 2.6f, noseY + 4.1f + k * 1.4f}}, 0.5f, col(0x191D20));
    hi.fillEllipse(cx, noseY + 0.7f, 0.9f, 1.2f, col(0xB9C1C7));    // spinner
    hi.fillEllipse(cx - 0.3f, noseY + 0.4f, 0.35f, 0.5f, col(0xE6ECF0));

    // --- canopy: long multi-framed greenhouse, just aft of the wing ---------
    hi.fillEllipse(cx, 28.2f, 1.55f, 4.7f, col(0x1C2E38));
    hi.fillEllipse(cx, 24.0f, 1.2f, 1.0f, col(0x35586A));            // windshield
    for (int k = 0; k < 4; ++k)  // glazing ribs
        hi.stroke({{cx - 1.4f, 25.6f + k * 1.8f}, {cx + 1.4f, 25.6f + k * 1.8f}},
                  0.45f, col(0xA9A392));
    hi.fillEllipse(cx - 0.45f, 27.2f, 0.6f, 2.0f, col(0x9FD0E8, 190));  // gloss

    // --- tail: rounded stabilizers, elevator seams, rudder hint -------------
    for (int side = -1; side <= 1; side += 2) {
        const float s = static_cast<float>(side);
        hi.fillEllipse(cx + s * 5.0f, tailY - 4.6f, 5.0f, 2.2f, body);
        hi.stroke({{cx + s * 4.4f, tailY - 6.5f}, {cx + s * 9.2f, tailY - 5.4f}},
                  0.55f, bodyHi);
        hi.stroke({{cx + s * 1.3f, tailY - 2.9f}, {cx + s * 9.4f, tailY - 4.2f}},
                  0.45f, bodyDk);
    }
    hi.stroke({{cx, tailY - 4.4f}, {cx, tailY - 0.9f}}, 0.45f, bodyDk);

    // Red identification band on the aft fuselage.
    hi.stroke({{cx - 1.3f, tailY - 7.6f}, {cx + 1.3f, tailY - 7.6f}}, 1.1f,
              col(0xB4322A));

    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x0A0E12, 165));

    // Static edge-on prop disc at the cowl nose (the animated prop_blur_e
    // streak pulses over it in flight). Translucent, so post-bake; kept
    // softer than the player's P-38 so the nose reads clean.
    const float propY = noseY + kZeroPropDy;
    softBlendEllipse(p, cx, propY, 5.6f, 2.1f, 0xC8D4DC, 58);
    softBlendEllipse(p, cx, propY, 3.4f, 1.35f, 0xD2DEE6, 48);
    return p;
}

// ---------------------------------------------------------------------------
// Enemy aircraft sprites + spinning-prop anchors.
//
// Every enemy plane bakes a flat edge-on prop disc (makeZero / makePlane),
// exactly like the player's P-38 bakes its own. The renderer pulses the
// prop_blur_e streak over those discs, so it needs each engine's position in
// the final nose-down sprite. Those anchors are derived here from the same
// geometry the art is built with, mapped through the crop and the vertical
// flip, so they can never drift from the silhouettes. buildEnemyPlanes runs
// on every launch — cached art included — while `emit` only uploads
// textures when the art is actually being rasterised.
// ---------------------------------------------------------------------------
std::unordered_map<std::string, std::vector<EnemyPropAnchor>> gEnemyProps;

// Engine prop planes in the nose-up makePlane canvas; must mirror makePlane:
// a single engine sits on the fuselage nose (noseY + 1.7), twins on the
// nacelles at half-span-quarter, cy - 7.
std::vector<std::pair<float, float>> propPlanesNoseUp(const PlaneStyle& st) {
    std::vector<std::pair<float, float>> e;
    if (st.twin) {
        const float nx = st.span * 0.25f;
        e.push_back({26.0f - nx, 26.0f - 7.0f});
        e.push_back({26.0f + nx, 26.0f - 7.0f});
    } else {
        e.push_back({26.0f, 26.0f - st.len * 0.5f + 1.7f});
    }
    return e;
}

void buildEnemyPlanes(
    const std::function<void(const std::string&, const PixelBuf&)>& emit) {
    gEnemyProps.clear();

    // Crop + flip exactly like the old addDown pipeline, recording the
    // centre-relative screen-space anchor for every engine (anchors given in
    // the nose-up 52x52 canvas coordinates).
    auto addPlaneRaw = [&](const std::string& name, PixelBuf canvas,
                           const std::vector<std::pair<float, float>>& noseUp,
                           float scale, bool mirror) {
        int x0 = canvas.w, y0 = canvas.h, x1 = -1, y1 = -1;
        for (int y = 0; y < canvas.h; ++y)
            for (int x = 0; x < canvas.w; ++x)
                if (canvas.get(x, y) & 0xFF) {
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        x0 = std::max(0, x0 - 1); y0 = std::max(0, y0 - 1);  // cropToContent pad
        const int cw = std::min(canvas.w - 1, x1 + 1) - x0 + 1;
        const int ch = std::min(canvas.h - 1, y1 + 1) - y0 + 1;

        PixelBuf p = cropToContent(canvas);
        flipY(p);
        // Prop planes sit above centre in the nose-up canvas; the flip puts
        // them below centre, where a nose-down enemy's nose is.
        const float ccx = x0 + cw * 0.5f, ccy = y0 + ch * 0.5f;
        std::vector<EnemyPropAnchor> anchors;
        for (const auto& [ex, ey] : noseUp)
            anchors.push_back({ex - ccx, ccy - ey, scale});
        if (mirror) {
            p.mirrorX();
            for (auto& a : anchors) a.dx = -a.dx;
        }
        gEnemyProps[name] = anchors;
        emit(name, p);
#ifndef NDEBUG
        dumpSpriteDbg(name, p);
#endif
    };
    auto addPlane = [&](const std::string& name, const PlaneStyle& st,
                        PixelBuf canvas, bool mirror) {
        addPlaneRaw(name, std::move(canvas), propPlanesNoseUp(st),
                    st.twin ? 0.75f : 1.0f, mirror);
    };
    auto red = [](PlaneStyle st) { st.redDot = true; return st; };

    {  // primary fighter: A6M "Zero" (the formation variant shares the sprite)
        const ZeroStyle z{38, 37, col(0xBDB6A3), col(0xD8D2C0), col(0x8C8571)};
        addPlaneRaw("enemy_fighter_light", makeZero(z),
                    {{26.0f, 26.0f - z.len * 0.5f + kZeroPropDy}}, 0.72f, false);
    }
    {  // yellow interceptor: slimmer, faster Zero variant; the mirrored frame
       // keeps mirrored anchors
        const ZeroStyle z{31, 33, kEnYellow, shade(kEnYellow, 1.2f),
                          shade(kEnYellow, 0.7f)};
        const float propY = 26.0f - z.len * 0.5f + kZeroPropDy;
        addPlaneRaw("enemy_interceptor", makeZero(z), {{26.0f, propY}}, 0.7f,
                    false);
        addPlaneRaw("enemy_interceptor_m", makeZero(z), {{26.0f, propY}}, 0.7f,
                    true);
    }
    {  // elite green heavy fighter
        PlaneStyle st =
            red({40, 30, kEnGreen, shade(kEnGreen, 1.2f), kEnGreenDk, true});
        st.amberMark = true;
        addPlane("enemy_heavy_fighter", st, makePlane(st), false);
    }
    {
        // Carrier escort fighter: steel-blue twin with crimson nose band and
        // bolt-on root armour — visibly tougher than anything it escorts.
        PlaneStyle st =
            red({34, 26, col(0x55606D), col(0x707C8A), col(0x39424D), true});
        PixelBuf pf = makePlane(st);
        const int pcx = 26;
        for (int x = -2; x <= 2; ++x) {           // crimson band under the nose
            pf.set(pcx + x, 16, col(0xB03A28));
            pf.set(pcx + x, 17, col(0x8C2A1C));
        }
        for (int sx : {-1, 1})                    // root armour plates
            for (int d = 4; d <= 11; ++d)
                for (int c = 1; c <= 3; ++c)
                    pf.set(pcx + sx * d, 27 + c, d == 11 ? col(0x2C343D) : col(0x3F4956));
        for (int sx : {-1, 1})                    // wing gun pods
            for (int t = 0; t < 4; ++t)
                pf.set(pcx + sx * 13, 27 + t, col(0x22272C));
        addPlane("enemy_fighter_carrier", st, std::move(pf), false);
    }
    {  // grey twin bomber
        const PlaneStyle st =
            red({56, 40, kEnGrey, shade(kEnGrey, 1.2f), kEnGreyDk, true});
        addPlane("enemy_bomber", st, makePlane(st), false);
    }
#ifndef NDEBUG
    // WWII_DUMP_SPRITES also records the anchors so offline inspection can
    // overlay the streak exactly where the renderer will.
    if (SDL_getenv("WWII_DUMP_SPRITES")) {
        std::ofstream out(std::string(SDL_getenv("WWII_DUMP_SPRITES")) +
                          "/enemy_prop_anchors.txt");
        for (const auto& [name, list] : gEnemyProps)
            for (const auto& a : list)
                out << name << ' ' << a.dx << ' ' << a.dy << ' ' << a.scale
                    << '\n';
    }
#endif
}

// ---------------------------------------------------------------------------
// 1942/1943-style explosion animation sheets.
//
// Reference is the CPS-era 1942/1943 kill effect: every detonation is a
// CLUSTER of small "puffs" — cauliflower fireballs with a yellow core,
// orange body and a scalloped red outline. Each puff pops in small, swells,
// then fades as its core burns out. The cluster's puffs are born in
// staggered waves, so bright new puffs keep erupting over cooling red ones
// and the whole fireball churns. Flames deepen to dark red and pop away —
// no grey smoke — while ember dots flicker around the cluster.
//
//   tier 0  tiny pop: one puff          (player damage tick)
//   tier 1  small: centre + 6-puff ring flower (fighters, AA, small craft)
//   tier 2  large: two-wave churning cluster    (bombers, ships, bunkers)
//   tier 3  xlarge: three waves + stragglers    (boss finale secondaries)
// ---------------------------------------------------------------------------

// Blend two 0xRRGGBB colors.
std::uint32_t mix24(std::uint32_t a, std::uint32_t b, float t) {
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

struct PuffDef {
    float ang, dist, rad, born, life, spin, hue;
    float lobe[12];  // scallop weights around the rim
};

// Draw one puff at lifetime phase p (0 = just born, 1 = gone).
void drawPuff(PixelBuf& sheet, float cx, float cy, float r, float p,
              const PuffDef& d) {
    if (p < 0.0f || p >= 1.0f || r < 0.4f) return;
    // Pop small, swell fast, then shrink away (CPS-style pop-out, keeps the
    // flames in the red family instead of alpha-muddying over the sea).
    const float grow = p < 0.60f ? 0.45f + 0.55f * std::sqrt(p / 0.60f)
                                 : 1.0f - 0.80f * (p - 0.60f) / 0.40f;
    const float rr = r * grow;
    // The yellow core burns out during the puff's second half.
    const float coreF = p < 0.55f ? 0.52f : 0.52f * (1.0f - (p - 0.55f) / 0.45f);
    // Age palette (hue offsets p so neighbouring puffs differ slightly).
    const float q = p + d.hue * 0.18f;
    const std::uint32_t coreC = mix24(0xFFF6C0, 0xFF9028, q);
    const std::uint32_t bodyC = mix24(0xFFB62E, 0xC23A12, q);
    const std::uint32_t rimC  = mix24(0xF25A1E, 0x9E2812, q);
    const std::uint32_t lineC = mix24(0xC0300E, 0x701A08, q);
    const int fade =
        p > 0.93f ? static_cast<int>(255.0f * (1.0f - (p - 0.93f) / 0.07f)) : 255;
    if (fade <= 0) return;
    const int x0 = static_cast<int>(cx - rr - 1.5f);
    const int x1 = static_cast<int>(cx + rr + 1.5f);
    const int y0 = static_cast<int>(cy - rr - 1.5f);
    const int y1 = static_cast<int>(cy + rr + 1.5f);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx;
            const float dy = y + 0.5f - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            // Scalloped (cauliflower) rim from the 12 lobe weights.
            float ang = std::atan2(dy, dx) + d.spin;
            ang = std::fmod(ang + 6.2832f, 6.2832f);
            const float lw = ang * (12.0f / 6.2832f);
            const int li = static_cast<int>(lw) % 12;
            const float lf = lw - std::floor(lw);
            const float lobe = d.lobe[li] * (1.0f - lf) +
                               d.lobe[(li + 1) % 12] * lf;
            const float edge = rr * (0.80f + 0.20f * lobe);
            // Soft 1px coverage ramp on the scalloped rim (rasterization
            // edge AA, not a post-filter): kills the stair-stepped contour.
            const float cov = (edge - dist) / 1.15f;
            if (cov <= 0.0f) continue;
            const float te = dist / edge;
            // Continuous heat ramp: pale core -> gold -> orange -> deep red,
            // replacing the old four hard-edged bands.
            std::uint32_t c;
            // mix24 takes 24-bit 0xRRGGBB inputs; coreC..lineC are 32-bit
            // col() words, so strip the alpha byte before blending.
            if (te < coreF)
                c = mix24(coreC >> 8, bodyC >> 8, coreF > 0.02f ? te / coreF : 1.0f);
            else if (te < 0.62f)
                c = mix24(bodyC >> 8, rimC >> 8, (te - coreF) / std::max(0.02f, 0.62f - coreF));
            else if (te < 0.86f)
                c = mix24(rimC >> 8, lineC >> 8, (te - 0.62f) / 0.24f);
            else
                c = mix24(lineC >> 8, 0x5A1206u, (te - 0.86f) / 0.14f);
            const float acov = cov > 1.0f ? 1.0f : cov;
            const int a = static_cast<int>(fade * acov + 0.5f);
            if (a <= 0) continue;
            sheet.blend(x, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
        }
    }
}

PixelBuf makeBoomSheet(int frames, int size, std::uint64_t seed, int tier,
                       bool water = false) {
    PixelBuf sheet(frames * size, size);
    Rng rng(seed);
    const float H = size * 0.5f;
    std::vector<PuffDef> puffs;
    auto addPuff = [&](float ang, float dist, float rad, float born,
                       float life) {
        PuffDef d;
        d.ang = ang; d.dist = dist; d.rad = rad; d.born = born; d.life = life;
        d.spin = rng.range(0.0f, 6.2832f);
        d.hue = rng.range(-0.5f, 0.5f);
        for (float& v : d.lobe) v = rng.range(0.70f, 1.15f);
        puffs.push_back(d);
    };
    // Puff rosters are built once so the animation is stable across frames.
    // All distances are fractions of the frame half-size; all times are
    // fractions of the clip.
    if (tier == 0) {  // tiny pop
        addPuff(0.0f, 0.0f, 0.42f, 0.00f, 0.95f);
    } else if (tier == 1) {  // small kill: ring flower (reference 'small')
        addPuff(0.0f, 0.0f, 0.34f, 0.02f, 0.92f);
        const float a0 = rng.range(0.0f, 1.05f);
        for (int k = 0; k < 6; ++k)
            addPuff(a0 + k * 6.2832f / 6.0f + rng.range(-0.12f, 0.12f),
                    rng.range(0.40f, 0.52f), rng.range(0.28f, 0.34f),
                    rng.range(0.05f, 0.16f), rng.range(0.75f, 0.95f));
    } else {  // tier 2/3: churning multi-wave cluster (reference 'large')
        addPuff(0.0f, 0.0f, 0.36f, 0.00f, 0.55f);
        const float a1 = rng.range(0.0f, 1.05f);
        for (int k = 0; k < 6; ++k)
            addPuff(a1 + k * 6.2832f / 6.0f + rng.range(-0.15f, 0.15f),
                    rng.range(0.30f, 0.44f), rng.range(0.26f, 0.34f),
                    rng.range(0.04f, 0.16f), rng.range(0.52f, 0.72f));
        const float a2 = rng.range(0.0f, 1.05f);
        for (int k = 0; k < 8; ++k)
            addPuff(a2 + k * 6.2832f / 8.0f + rng.range(-0.18f, 0.18f),
                    rng.range(0.52f, 0.70f), rng.range(0.24f, 0.32f),
                    rng.range(0.26f, 0.44f), rng.range(0.48f, 0.70f));
        if (tier >= 3) {
            const float a3 = rng.range(0.0f, 1.05f);
            for (int k = 0; k < 8; ++k)
                addPuff(a3 + k * 6.2832f / 8.0f + rng.range(-0.2f, 0.2f),
                        rng.range(0.72f, 0.88f), rng.range(0.20f, 0.28f),
                        rng.range(0.34f, 0.55f), rng.range(0.38f, 0.60f));
        }
        for (int k = 0; k < 4; ++k)  // late stragglers keep it churning
            addPuff(rng.range(0.0f, 6.2832f), rng.range(0.55f, 0.80f),
                    rng.range(0.18f, 0.26f), rng.range(0.45f, 0.62f),
                    rng.range(0.33f, 0.52f));
    }
    // Ember dots flickering around the cluster (white droplets over water).
    struct Emb { float ang, v0, v1; std::uint32_t seedv; };
    const int nEmber = tier <= 0 ? 2 : tier == 1 ? 5 : tier == 2 ? 9 : 14;
    std::vector<Emb> embers;
    for (int k = 0; k < nEmber; ++k)
        embers.push_back({rng.range(0.0f, 6.2832f), rng.range(0.18f, 0.35f),
                          rng.range(0.55f, 0.95f), rng.nextU32()});
    // Water spray: flattened white crown of droplets spreading outward.
    struct Spray { float ang, spd; };
    std::vector<Spray> spray;
    if (water)
        for (int k = 0; k < 16; ++k)
            spray.push_back({rng.range(0.0f, 6.2832f), rng.range(0.45f, 1.0f)});

    for (int f = 0; f < frames; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(frames - 1);
        const float cx = f * size + H;
        const float cy = H;
        // Spray crown first (behind everything).
        if (water && t < 0.90f) {
            const float sf = t < 0.55f ? 1.0f : 1.0f - (t - 0.55f) / 0.35f;
            const int sa = static_cast<int>(230.0f * sf);
            if (sa > 0) {
                const std::uint32_t c = col(0xF4FAFF, static_cast<std::uint8_t>(sa));
                for (const Spray& s : spray) {
                    const float d = (0.18f + 0.85f * s.spd * (0.35f + 0.75f * t)) * H;
                    const int px = static_cast<int>(cx + std::cos(s.ang) * d);
                    const int py = static_cast<int>(cy + std::sin(s.ang) * d * 0.55f);
                    sheet.blend(px, py, c);
                    if (t < 0.40f) sheet.blend(px, py + 1, c);
                }
            }
        }
        // Embers next (behind the flames).
        for (int k = 0; k < nEmber; ++k) {
            const Emb& e = embers[static_cast<std::size_t>(k)];
            const float d = (e.v0 + (e.v1 - e.v0) * t) * H;
            // Deterministic flicker: some frames blink off.
            if (((e.seedv + static_cast<std::uint32_t>(f) * 7u) >> 2) % 3u == 0u)
                continue;
            const int px = static_cast<int>(cx + std::cos(e.ang) * d);
            const int py = static_cast<int>(cy + std::sin(e.ang) * d);
            const std::uint32_t c = water
                ? (t < 0.45f ? col(0xF4FAFF, 235) : col(0xC8DCF0, 200))
                : (t < 0.45f ? col(0xFFD870, 235) : col(0xF0681E, 210));
            sheet.blend(px, py, c);
            if (t < 0.30f) sheet.blend(px + 1, py, c);
        }
        // Oldest puffs first: newer, brighter puffs paint over red ones.
        for (const PuffDef& d : puffs) {
            const float p = (t - d.born) / d.life;
            // Rings creep gently outward while the clip runs.
            const float drift = d.dist * (0.75f + 0.25f * t);
            const float px = cx + std::cos(d.ang) * drift * H;
            const float py = cy + std::sin(d.ang) * drift * H;
            drawPuff(sheet, px, py, d.rad * H, p, d);
        }
    }
    return sheet;
}

// ---------------------------------------------------------------------------
// Boss-death explosion: white detonation flash into a huge three-wave puff
// cluster (same visual language as the ship/aircraft kills, far bigger).
// ---------------------------------------------------------------------------
PixelBuf makeBossExplosionSheet() {
    PixelBuf sheet = makeBoomSheet(19, 128, 0xB5DEA1u, 3);
    const int FR = 19, S = 128;
    for (int f = 0; f < FR; ++f) {
        const float t = static_cast<float>(f) / static_cast<float>(FR - 1);
        if (t > 0.10f) break;
        // White-hot detonation core, burnt off within the first frames.
        const float cx = f * S + S * 0.5f, cy = S * 0.5f;
        const float r = S * (0.13f + 0.22f * (t / 0.10f));
        const float fd = 1.0f - t / 0.10f;
        const int a = static_cast<int>(255.0f * fd * fd);
        for (int y = static_cast<int>(cy - r); y <= static_cast<int>(cy + r); ++y)
            for (int x = static_cast<int>(cx - r); x <= static_cast<int>(cx + r); ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > r) continue;
                const int aa = static_cast<int>(a * (1.0f - (dist / r) * 0.4f));
                sheet.blend(x, y, col(t < 0.07f ? 0xFFFFF0u : 0xFFF0B0u,
                                      static_cast<std::uint8_t>(aa)));
            }
    }
    return sheet;
}

// Pure-white silhouette of a sprite, used for the armor-break flash.
PixelBuf toFlashSilhouette(const PixelBuf& src) {
    PixelBuf p(src.w, src.h);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if (((src.get(x, y)) & 0xFF) > 170)   // skip water shadow/soft foam
                p.set(x, y, col(0xFFFFFF, 235));
    return p;
}

PixelBuf makeMuzzleSheet() {
    PixelBuf sheet(16 * 3, 16);
    for (int f = 0; f < 3; ++f) {
        const float r = 4.0f + f * 2.5f;
        const float cx = f * 16 + 8.0f, cy = 8.0f;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                // 4-point star.
                const float ang = std::atan2(dy, dx);
                const float star = r * (0.55f + 0.45f * std::pow(std::abs(std::cos(2.0f * ang)), 6.0f));
                if (d < star)
                    sheet.set(x, y, d < r * 0.35f ? col(0xFFFFFF, 255) : col(0xFFD860, 220));
            }
    }
    return sheet;
}

// ---------------------------------------------------------------------------
// Ground / sea units
// ---------------------------------------------------------------------------
PixelBuf makeAATurret() {
    Hi hi(32, 32);
    const float cx = 16.0f;
    // Cast shadow anchors the emplacement to the ground.
    hi.fillEllipse(cx + 1.0f, 23.5f, 15.5f, 11.8f, col(0x0A0C06, 80));
    // Sandbag ring: two stacked courses with individually shaded bags so
    // the rim reads as fabric, not a solid blob.
    hi.fillEllipse(cx, 21, 15.0f, 12.0f, col(0x46381F));           // lower course
    hi.fillEllipse(cx, 20, 14.0f, 11.0f, col(0x6E5C3C));
    for (int k = 0; k < 11; ++k) {
        const float a = k * 0.5712f + 0.28f;
        const float bx = cx + 13.2f * std::cos(a);
        const float by = 20.0f + 10.3f * std::sin(a);
        const bool lit = std::sin(a) < -0.15f || std::cos(a) < -0.3f;
        hi.fillEllipse(bx, by, 3.0f, 2.35f, lit ? col(0x8A7A52) : col(0x615234));
        hi.fillEllipse(bx - 0.7f, by - 0.8f, 1.6f, 1.0f,
                       lit ? col(0x9E8E64) : col(0x746240));       // bag sheen
    }
    // Amber hazard dashes (armored-target language shared with bosses).
    for (int k = 0; k < 16; ++k) {
        if (k % 2) continue;
        const float a = k * 0.3927f + 0.15f;
        hi.fillEllipse(cx + 14.4f * std::cos(a), 20.4f + 11.4f * std::sin(a),
                       0.9f, 0.7f, col(0xC8A83C));
    }
    // Concrete gun platform with a lit top face and cast inner shadow.
    hi.fillEllipse(cx, 19, 10.2f, 8.2f, col(0x505048));
    hi.fillEllipse(cx, 18, 9.0f, 7.0f, col(0x8C8C80));
    hi.fillEllipse(cx - 2, 15.6f, 5.4f, 3.4f, col(0xA2A296));      // lit top
    hi.fillEllipse(cx + 3, 20.6f, 4.6f, 2.8f, col(0x6E6E64));      // shade side
    // Ready-service depth: ammo cans with red labels behind the gun.
    hi.fillRoundRect(21.5f, 18.0f, 6.0f, 5.0f, 1.0f, col(0x4A5236));
    hi.fillRoundRect(22.1f, 18.6f, 4.8f, 1.6f, 0.6f, col(0x5E6844));
    hi.fillRoundRect(23.0f, 20.8f, 3.0f, 1.4f, 0.4f, col(0x9E3026));
    hi.fillRoundRect(5.5f, 19.5f, 4.6f, 4.2f, 0.9f, col(0x4A5236));
    hi.fillRoundRect(6.0f, 20.0f, 3.6f, 1.3f, 0.5f, col(0x5E6844));
    // Mount base ring + rotating turret body.
    hi.fillEllipse(cx, 19.4f, 5.6f, 4.4f, col(0x3C4046));
    hi.fillEllipse(cx, 18.6f, 4.6f, 3.6f, col(0x6A7076));
    hi.fillEllipse(cx - 1.2f, 17.4f, 2.6f, 1.8f, col(0x8A9094));   // lit crown
    // Armored gun shield: beveled plate with sight slit and rim light.
    hi.fillRoundRect(cx - 8.0f, 8.0f, 16.0f, 7.4f, 1.8f, col(0x3E444A));
    hi.fillRoundRect(cx - 7.2f, 8.7f, 14.4f, 5.6f, 1.4f, col(0x6A727A));
    hi.fillRoundRect(cx - 6.4f, 9.2f, 12.8f, 2.2f, 1.0f, col(0x8E969C));
    hi.stroke({{cx - 4.4f, 12.4f}, {cx + 4.4f, 12.4f}}, 1.2f, col(0x10141A));
    // Twin AA barrels with ribbed cooling jackets + flash hiders.
    for (const float bx : {12.7f, 17.7f}) {
        hi.stroke({{bx, 1.6f}, {bx, 13.0f}}, 2.0f, col(0x10141A));
        hi.stroke({{bx - 0.5f, 2.4f}, {bx - 0.5f, 12.2f}}, 0.6f, col(0x5A6066));
        for (int r = 0; r < 3; ++r)
            hi.stroke({{bx - 1.2f, 4.5f + r * 2.6f}, {bx + 1.2f, 4.5f + r * 2.6f}},
                      0.7f, col(0x2C3238));
        hi.fillRoundRect(bx - 1.4f, 0.8f, 2.8f, 2.6f, 0.8f, col(0x22262A));
        hi.fillRoundRect(bx - 1.0f, 1.1f, 1.1f, 1.9f, 0.5f, col(0x4A5056));
    }
    // Ladder down the rear face + boot prints in the dirt.
    hi.stroke({{7.6f, 26.0f}, {10.4f, 26.0f}}, 0.8f, col(0x33332C));
    hi.stroke({{7.6f, 28.6f}, {10.4f, 28.6f}}, 0.8f, col(0x33332C));
    hi.stroke({{8.0f, 25.2f}, {8.0f, 29.4f}}, 0.6f, col(0x33332C));
    hi.stroke({{10.0f, 25.2f}, {10.0f, 29.4f}}, 0.6f, col(0x33332C));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x120E08, 160));
    return p;
}

PixelBuf makeTank() {
    Hi hi(40, 28);
    const float cx = 20.0f;
    // Cast shadow under the hull grounds the vehicle.
    hi.fillEllipse(cx + 1.0f, 15.5f, 18.0f, 12.4f, col(0x0A0C06, 70));
    // Tracks: dark capsules with individual links, road wheels, drive
    // sprockets and an idler, plus a fender lip catching the light.
    for (const float tx : {6.4f, 33.6f}) {
        hi.fillRoundRect(tx - 3.6f, 2.6f, 7.2f, 22.8f, 3.4f, col(0x101316));
        hi.fillRoundRect(tx - 2.8f, 3.4f, 5.6f, 21.2f, 2.8f, col(0x2A2E33));
        for (int k = 0; k < 8; ++k)                       // link gaps
            hi.stroke({{tx - 2.8f, 4.4f + k * 2.6f}, {tx + 2.8f, 4.4f + k * 2.6f}},
                      0.7f, col(0x171A1E));
        for (int k = 0; k < 5; ++k) {                     // road wheels
            hi.fillEllipse(tx, 6.8f + k * 3.5f, 1.7f, 1.7f, col(0x3E444A));
            hi.fillEllipse(tx - 0.4f, 6.4f + k * 3.5f, 0.8f, 0.8f, col(0x5A6066));
        }
        hi.fillEllipse(tx, 4.4f, 2.0f, 2.0f, col(0x4A5056));       // sprocket
        hi.fillEllipse(tx, 4.4f, 0.8f, 0.8f, col(0x22262A));
        hi.stroke({{tx - 3.1f, 6.0f}, {tx - 3.1f, 22.0f}}, 0.6f, col(0x40464C));
    }
    // Mud flung along the outer track edges.
    for (int k = 0; k < 6; ++k) {
        hi.fillEllipse(2.0f, 6.0f + k * 3.4f, 0.8f, 0.6f, col(0x5A4A2E, 160));
        hi.fillEllipse(38.0f, 7.4f + k * 3.4f, 0.8f, 0.6f, col(0x5A4A2E, 160));
    }
    // Hull: slab with a front glacis bevel (light from top-left), deck
    // seam, side bolt strip and rear engine grille.
    hi.fillRoundRect(9.4f, 3.6f, 21.2f, 21.4f, 3.6f, col(0x2E3823));
    hi.fillRoundRect(10.4f, 4.6f, 19.2f, 18.4f, 3.0f, col(0x4E5A38));
    hi.fillRoundRect(11.4f, 5.4f, 17.2f, 6.4f, 2.4f, col(0x5E6C44));  // lit deck
    hi.fillRoundRect(11.4f, 17.4f, 17.2f, 4.6f, 2.0f, col(0x42502F)); // glacis shade
    hi.stroke({{11.0f, 16.6f}, {29.0f, 16.6f}}, 0.7f, col(0x38422A)); // deck seam
    for (int k = 0; k < 5; ++k) {                                      // bolt strip
        hi.fillEllipse(11.6f, 7.6f + k * 2.6f, 0.5f, 0.5f, col(0x6A7850));
        hi.fillEllipse(28.4f, 7.6f + k * 2.6f, 0.5f, 0.5f, col(0x3A4630));
    }
    for (int k = 0; k < 4; ++k)                                        // grille
        hi.stroke({{15.0f + k * 0.0f, 22.6f}, {25.0f, 22.6f + k * 0.9f}},
                  0.5f, col(0x2A3322));
    // Stowage bin and jack on the rear deck.
    hi.fillRoundRect(11.8f, 4.4f, 5.4f, 3.0f, 0.8f, col(0x3E4A2C));
    hi.fillRoundRect(12.2f, 4.8f, 4.6f, 1.0f, 0.5f, col(0x56643C));
    hi.stroke({{23.4f, 5.4f}, {28.0f, 5.4f}}, 0.8f, col(0x5A4A2E));
    // Turret: octagonal ring shadow, crown highlight, commander hatch.
    hi.fillEllipse(cx, 12.6f, 8.0f, 7.2f, col(0x28321F));
    hi.fillEllipse(cx, 11.8f, 7.4f, 6.6f, col(0x46523A));
    hi.fillEllipse(cx - 1.4f, 10.2f, 4.8f, 3.8f, col(0x5C6A42));       // crown
    hi.stroke({{cx - 5.6f, 13.6f}, {cx + 5.6f, 13.6f}}, 0.6f, col(0x36422A));
    hi.fillEllipse(cx + 3.4f, 9.4f, 1.8f, 1.5f, col(0x3A4630));        // hatch
    hi.fillEllipse(cx + 3.1f, 9.1f, 0.9f, 0.7f, col(0x56643C));
    // Hinomaru-style marking on the turret flank for target readability.
    hi.fillEllipse(cx - 4.2f, 12.4f, 1.7f, 1.5f, col(0xD8D4C8));
    hi.fillEllipse(cx - 4.2f, 12.4f, 1.1f, 1.0f, col(0xB03428));
    // Mantlet + main gun: barrel down at the player, collar, muzzle brake.
    hi.fillRoundRect(cx - 2.8f, 15.0f, 5.6f, 4.0f, 1.3f, col(0x36422A));
    hi.fillRoundRect(cx - 2.2f, 15.4f, 4.4f, 1.4f, 0.7f, col(0x4E5A38));
    hi.stroke({{cx, 16.8f}, {cx, 26.4f}}, 2.2f, col(0x10141A));
    hi.stroke({{cx - 0.6f, 17.6f}, {cx - 0.6f, 25.4f}}, 0.6f, col(0x4E545A));
    hi.stroke({{cx - 1.4f, 20.2f}, {cx + 1.4f, 20.2f}}, 0.6f, col(0x2A3036));
    hi.fillRoundRect(cx - 1.8f, 25.2f, 3.6f, 2.4f, 0.7f, col(0x33383D));
    hi.fillRoundRect(cx - 1.3f, 25.6f, 1.2f, 1.6f, 0.5f, col(0x5A6066));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x101408, 160));
    return p;
}

PixelBuf makeBunker() {
    Hi hi(32, 32);
    // Cast shadow + earth berm with grass fringe on the far edge.
    hi.fillEllipse(16, 21, 15.2f, 11.8f, col(0x3A3526));
    hi.fillEllipse(16, 19.6f, 14.0f, 10.8f, col(0x524A34));
    for (int k = 0; k < 9; ++k) {                       // grass fringe on berm
        const float gx = 4.5f + k * 2.9f;
        const float dx = (gx - 16.0f) / 14.0f;
        const float rim = 19.6f - 10.8f * std::sqrt(std::max(0.0f, 1.0f - dx * dx));
        const float gy = rim - 0.6f + 0.8f * std::sin(k * 1.7f + 0.4f);
        hi.fillEllipse(gx, gy, 1.5f, 0.9f, k % 2 ? col(0x3E5A26) : col(0x354E20));
        hi.fillEllipse(gx - 0.4f, gy - 0.4f, 0.7f, 0.45f, col(0x4E7030));
    }
    // Concrete dome: slab base, curved shell, panel seams, weather streaks.
    hi.fillEllipse(16, 18, 12.6f, 10.2f, col(0x5E5E54));
    hi.fillEllipse(16, 16.6f, 11.6f, 9.2f, col(0x8C8C80));
    hi.fillEllipse(12.0f, 12.6f, 5.6f, 3.4f, col(0x9E9E90));          // lit crown
    for (int k = 0; k < 3; ++k) {                                     // panel rings
        const float rr = 4.2f + k * 2.9f;
        hi.stroke({{16.0f - rr, 15.0f}, {16.0f, 15.0f - rr * 0.78f}}, 0.5f,
                  col(0x72726A, 220), false);
        hi.stroke({{16.0f + rr, 15.0f}, {16.0f, 15.0f - rr * 0.78f}}, 0.5f,
                  col(0x72726A, 220), false);
    }
    hi.stroke({{8.0f, 20.0f}, {7.0f, 24.5f}}, 0.7f, col(0x55554D, 170));  // stains
    hi.stroke({{24.0f, 20.5f}, {25.0f, 24.0f}}, 0.7f, col(0x55554D, 170));
    // Embrasure: heavy steel frame around a black slot with stub gun.
    hi.fillRoundRect(6.6f, 14.6f, 18.8f, 5.8f, 2.0f, col(0x4A4E52));
    hi.fillRoundRect(7.4f, 15.2f, 17.2f, 4.6f, 1.6f, col(0x0C0E10));
    hi.fillRoundRect(8.4f, 15.6f, 15.2f, 1.2f, 0.5f, col(0x2A2E32));  // lintel lit
    hi.stroke({{16.0f, 16.2f}, {16.0f, 19.8f}}, 1.6f, col(0x3A4046));
    hi.stroke({{15.6f, 16.6f}, {15.6f, 19.4f}}, 0.5f, col(0x7A8086));
    hi.fillEllipse(16, 19.6f, 1.4f, 0.9f, col(0x1A1E22));             // muzzle ring
    // Sandbags stacked along the base of the front face.
    for (int k = 0; k < 5; ++k) {
        const float bx = 6.5f + k * 4.8f;
        hi.fillEllipse(bx, 23.6f, 2.5f, 1.7f, k % 2 ? col(0x7A6A46) : col(0x665838));
        hi.fillEllipse(bx - 0.5f, 23.1f, 1.2f, 0.8f, col(0x8E7E58));
    }
    // Antenna whip + base insulator.
    hi.stroke({{26.5f, 4.0f}, {25.6f, 11.5f}}, 0.55f, col(0x2A2E32));
    hi.fillEllipse(25.6f, 12.0f, 1.0f, 0.8f, col(0x5E5E54));
    hi.fillEllipse(26.5f, 3.8f, 0.6f, 0.6f, col(0x8A8A80));
    // Heavy base slab with lit top edge.
    hi.fillRoundRect(4.6f, 26.0f, 22.8f, 3.4f, 1.3f, col(0x4E4E48));
    hi.fillRoundRect(5.2f, 26.2f, 21.6f, 1.0f, 0.5f, col(0x7A7A70));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x121008, 160));
    return p;
}

PixelBuf makeShip(int w, int h, bool destroyer) {
    Hi hi(w, h);
    const float cx = w * 0.5f;
    // Long, narrow warship silhouette (top-down, bow at the bottom): a
    // nearly straight belt amidships, a softly rounded stern and a sharp
    // CLIPPED bow taking the lower 30% of the hull length — the profile
    // reads as a warship at a glance.
    const float hw = w * 0.5f - 1.5f;
    auto halfAt = [&](float t) {
        if (t > 0.70f) return hw * std::max(0.06f, 1.0f - (t - 0.70f) / 0.30f * 0.94f);
        if (t < 0.05f) return hw * (0.72f + t / 0.05f * 0.28f);
        return hw;
    };
    std::vector<std::pair<float, float>> hull;
    for (int i = 0; i <= 18; ++i) hull.push_back({cx - halfAt(i / 18.0f) + 0.4f, 1.0f + (h - 2) * i / 18.0f});
    for (int i = 18; i >= 0; --i) hull.push_back({cx + halfAt(i / 18.0f), 1.0f + (h - 2) * i / 18.0f});
    hi.fillPoly(hull, col(0x565D64));
    // Belt shading + deck inset.
    std::vector<std::pair<float, float>> deck;
    for (int i = 0; i <= 16; ++i) {
        const float t = 0.05f + 0.9f * i / 16.0f;
        deck.push_back({cx - halfAt(t) + 2.4f, 1.0f + (h - 2) * t});
    }
    for (int i = 16; i >= 0; --i) {
        const float t = 0.05f + 0.9f * i / 16.0f;
        deck.push_back({cx + halfAt(t) - 2.6f, 1.0f + (h - 2) * t});
    }
    hi.fillPoly(deck, col(0x6A7178));
    hi.stroke({{cx - halfAt(0.4f) + 1.0f, 4.0f}, {cx - halfAt(0.4f) + 1.0f, h - 20.0f}},
              1.2f, col(0x7C848B));                                  // lit flank
    hi.stroke({{cx + halfAt(0.4f) - 1.0f, 4.0f}, {cx + halfAt(0.4f) - 1.0f, h - 20.0f}},
              1.2f, col(0x3A4046));                                  // shaded flank
    // Deck plank lines for weathered steel.
    for (int i = 0; i < 7; ++i) {
        const float t = 0.08f + 0.8f * i / 6.0f;
        const float yy = 1.0f + (h - 2) * t;
        hi.stroke({{cx - halfAt(t) + 2.0f, yy}, {cx + halfAt(t) - 2.2f, yy}},
                  0.5f, col(0x5D646B));
    }
    // Superstructure fore-aft: (destroyer) aft gun - funnel - bridge -
    // forward gun; barrels face the player.
    auto turret = [&](float tx, float ty, float s, float blen) {
        hi.fillEllipse(tx, ty + 0.5f * s, 3.9f * s, 3.9f * s, col(0x24282C));
        hi.fillEllipse(tx, ty, 3.7f * s, 3.7f * s, col(0x454B51));
        hi.fillEllipse(tx - 0.6f * s, ty - 0.6f * s, 2.3f * s, 2.3f * s,
                       col(0x6E757C));
        hi.stroke({{tx, ty + 1.4f * s}, {tx, ty + 1.4f * s + blen}}, 1.6f * s,
                  col(0x10141A));
        hi.stroke({{tx - 0.4f * s, ty + 1.8f * s}, {tx - 0.4f * s, ty + 1.4f * s + blen - 0.6f}},
                  0.5f, col(0x565C62));
    };
    auto bridge = [&](float by, float bw) {
        hi.fillRoundRect(cx - bw - 0.7f, by - 2.2f, bw * 2.0f + 1.4f, 6.4f, 1.3f,
                         col(0x33383D));
        hi.fillRoundRect(cx - bw, by - 3.0f, bw * 2.0f, 6.2f, 1.3f, col(0x7E858C));
        hi.fillRoundRect(cx - bw + 0.8f, by - 2.4f, bw * 2.0f - 1.6f, 1.8f, 0.7f,
                         col(0x9AA1A8));                            // lit top
        hi.fillRoundRect(cx - 2.0f, by - 0.2f, 4.0f, 2.2f, 0.7f, col(0x3A5A6E));
        hi.fillRoundRect(cx - 1.6f, by + 0.1f, 1.5f, 1.3f, 0.5f, col(0x6E90A4));
        hi.fillEllipse(cx, by + 4.2f, 1.4f, 1.2f, col(0xE8E4D0));   // searchlight
        hi.fillEllipse(cx, by + 4.2f, 0.65f, 0.6f, col(0xFFFFE8));
    };
    if (destroyer) {
        // h=64: aft AA y4.5, aft gun y14, funnel y21.5, bridge y30, fwd gun
        // y45 on the bow shoulder.
        for (const float sx : {-1.0f, 1.0f}) {
            hi.fillEllipse(cx + sx * 4.6f, 4.6f, 1.6f, 1.4f, col(0x3A4046));
            hi.stroke({{cx + sx * 4.6f, 3.9f}, {cx + sx * 4.6f, 2.4f}}, 0.9f,
                      col(0x10141A));
        }
        turret(cx, 14.0f, 0.85f, 4.6f);                              // aft gun
        hi.fillEllipse(cx, 22.0f, 2.7f, 2.4f, col(0x2E3338));        // funnel
        hi.fillEllipse(cx, 21.6f, 1.8f, 1.5f, col(0x4E555C));
        bridge(30.5f, 6.2f);
        hi.fillRoundRect(cx - 1.4f, 38.6f, 2.8f, 2.8f, 0.6f, col(0x565D64));
        turret(cx, 45.0f, 1.0f, 5.0f);                               // forward gun
        hi.fillEllipse(cx, 55.0f, 0.9f, 0.9f, col(0x3A4046));        // bow anchor
    } else {
        // h=44 patrol boat: depth-charge racks at the stern, mast, bridge,
        // funnel, light forward gun on the bow. Same bow-down orientation
        // as the destroyer (no flip).
        hi.fillRoundRect(cx - 6.2f, 2.0f, 2.0f, 5.6f, 0.9f, col(0x2E3338));
        hi.fillRoundRect(cx + 4.2f, 2.0f, 2.0f, 5.6f, 0.9f, col(0x2E3338));
        hi.fillEllipse(cx - 5.2f, 3.4f, 0.7f, 0.7f, col(0x4E555C));
        hi.fillEllipse(cx + 5.2f, 3.4f, 0.7f, 0.7f, col(0x4E555C));
        hi.stroke({{cx, 9.6f}, {cx, 12.4f}}, 0.6f, col(0x2A2E32));   // mast
        hi.fillEllipse(cx, 9.3f, 0.7f, 0.7f, col(0xC8C4B0));
        bridge(17.0f, 4.2f);
        hi.fillEllipse(cx, 25.5f, 2.0f, 1.8f, col(0x2E3338));        // funnel
        hi.fillEllipse(cx, 25.2f, 1.2f, 1.1f, col(0x4E555C));
        turret(cx, 32.5f, 0.75f, 3.2f);                              // forward gun
    }
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x10141A, 150));
    return p;
}

// ---------------------------------------------------------------------------
// P-38 Lightning (player fighter), top-down, nose-up.
//
// The silhouette follows the reference airframe (assets/P-38.png): a long
// gun-nose centre nacelle with a framed greenhouse canopy ending forward of
// the stabilizer, slightly swept tapered wings carrying star-and-bars on
// both panels, and bulged engine cowlings on slim booms that run straight
// aft into the fins with an H-tail slung between them. Engines/booms sit at
// fixed offsets in every bank frame so the animated propeller overlay can
// share one anchor pair. Bank frames shift only the wingtips and tail
// outboard (house bank language from makePlane); the banked frame is
// mirrored for the other side.
// ---------------------------------------------------------------------------
// Forward-facing propeller blur for the P-38, drawn the way a spinning
// propeller actually reads in this view: the plane flies up-screen, so the
// prop disc sits edge-on to the camera and appears as a FLAT spanwise
// ellipse across each nacelle's nose — never a full circle, and never a
// blade pointing anywhere. This texture is the animated half: a soft silver
// wash with brighter blade-tip caps at each end. The aircraft sprite bakes
// a matching static prop disc (so splash/squash frames still read as
// prop-driven); the renderer pulses this streak over it to suggest rotation.
//
// Straight alpha (4x4 coverage sampling): blending translucent ink onto an
// empty PixelBuf would store premultiplied colours, which SDL then
// dark-squares.
PixelBuf makePropStreak() {
    const int S = 11;
    PixelBuf p(S, S);
    const float cc = S * 0.5f;
    const float rx = 4.6f, ry = 1.8f;  // edge-on disc: wide, flat
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float aBody = 0.0f;
            float aTip = 0.0f;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = x + (sx + 0.5f) / 4.0f - cc;
                    const float dy = y + (sy + 0.5f) / 4.0f - cc;
                    const float d2 = (dx * dx) / (rx * rx) +
                                     (dy * dy) / (ry * ry);
                    if (d2 >= 1.0f) continue;
                    const float fall = 1.0f - d2;  // mild centre→edge falloff
                    aBody += (70.0f + 46.0f * fall) *
                             (0.55f + 0.45f * (1.0f - std::sqrt(d2)));
                    // Yellow caps where the blade tips sweep the rim (the
                    // reference airframe carries yellow-painted tips).
                    const float ax = std::fabs(dx) - (rx - 1.1f);
                    const float tb = 1.0f - (ax * ax) / 1.3f -
                                     (dy * dy) / 1.5f;
                    if (tb > 0.0f) aTip += 235.0f * tb;
                }
            }
            aBody /= 16.0f;
            aTip /= 16.0f;
            const float ao = std::min(215.0f, std::max(aBody, aTip));
            if (ao < 0.5f) continue;
            // Wash 0xB4C2CC, tip caps warming to yellow 0xF2D878.
            const float w = aTip / (aBody + aTip + 0.001f);
            const std::uint32_t cr = static_cast<std::uint32_t>(
                (0xB4 + (0xF2 - 0xB4) * w) + 0.5f);
            const std::uint32_t cg = static_cast<std::uint32_t>(
                (0xC2 + (0xD8 - 0xC2) * w) + 0.5f);
            const std::uint32_t cb = static_cast<std::uint32_t>(
                (0xCC + (0x78 - 0xCC) * w) + 0.5f);
            p.set(x, y, (cr << 24) | (cg << 16) | (cb << 8) |
                            static_cast<std::uint32_t>(ao + 0.5f));
        }
    }
    return p;
}

// Enemy-scale version of the edge-on prop streak: same construction and
// punch as makePropStreak, sized to overhang the smaller baked prop discs
// in makePlane (the streak must extend beyond the disc or the pulse has no
// contrast against it) and in a bright silver that reads over the tan,
// yellow and green enemy finishes and their dark shadows. One texture
// serves every enemy airframe; the renderer draws it scaled per engine.
PixelBuf makePropStreakEnemy() {
    const int S = 10;
    PixelBuf p(S, S);
    const float cc = S * 0.5f;
    const float rx = 3.9f, ry = 1.8f;  // edge-on disc: wide, flat
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            float aBody = 0.0f;
            float aTip = 0.0f;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = x + (sx + 0.5f) / 4.0f - cc;
                    const float dy = y + (sy + 0.5f) / 4.0f - cc;
                    const float d2 = (dx * dx) / (rx * rx) +
                                     (dy * dy) / (ry * ry);
                    if (d2 >= 1.0f) continue;
                    const float fall = 1.0f - d2;  // mild centre→edge falloff
                    aBody += (150.0f + 90.0f * fall) *
                             (0.6f + 0.4f * (1.0f - std::sqrt(d2)));
                    // Bright caps where the blade tips sweep the rim.
                    const float ax = std::fabs(dx) - (rx - 1.3f);
                    const float tb = 1.0f - (ax * ax) / 1.6f -
                                     (dy * dy) / 1.8f;
                    if (tb > 0.0f) aTip += 255.0f * tb;
                }
            }
            aBody /= 16.0f;
            aTip /= 16.0f;
            const float ao = std::min(250.0f, std::max(aBody, aTip));
            if (ao < 0.5f) continue;
            // Wash 0xC6D2DA, tip caps whiter toward 0xF0F6FA.
            const float w = aTip / (aBody + aTip + 0.001f);
            const std::uint32_t cr = static_cast<std::uint32_t>(
                (0xC6 + (0xF0 - 0xC6) * w) + 0.5f);
            const std::uint32_t cg = static_cast<std::uint32_t>(
                (0xD2 + (0xF6 - 0xD2) * w) + 0.5f);
            const std::uint32_t cb = static_cast<std::uint32_t>(
                (0xDA + (0xFA - 0xDA) * w) + 0.5f);
            p.set(x, y, (cr << 24) | (cg << 16) | (cb << 8) |
                            static_cast<std::uint32_t>(ao + 0.5f));
        }
    }
    return p;
}

PixelBuf makeP38(int bankRows = 0) {
    const int W = 56, H = 56;
    const float cx = 28.0f, cy = 28.0f;
    Hi hi(W, H);
    const std::uint32_t body = kDrab, bodyHi = kDrabHi, bodyDk = kDrabDk;
    const std::uint32_t steelDk = col(0x2A3138);
    const std::uint32_t steelMid = col(0x4A535C);
    const std::uint32_t seam = col(0x51532F);    // panel seam (darker drab)
    const std::uint32_t seamLt = col(0x8F9260);  // lit rivet seam
    const float engX = kPlayerEngineX;
    const float propY = cy + kPlayerPropY;

    // --- wing: slightly swept leading edge, trailing edge raking forward to
    // broad rounded tips; aileron seam and a mid-chord rivet line ----------
    const float tipShift = static_cast<float>(bankRows);  // +side applied per wing
    for (int side = -1; side <= 1; side += 2) {
        const float sh = tipShift * side;
        const float rootLE = cy - 4.2f, tipLE = cy - 3.4f + sh * 0.6f;
        const float rootTE = cy + 6.4f, tipTE = cy + 3.6f + sh;
        const float tipX = cx + side * 23.0f;
        hi.fillPoly({{cx - 1.0f, rootLE}, {cx + side * 9.6f, rootLE - 0.2f},
                     {tipX - side * 0.8f, tipLE + 0.3f}, {tipX, tipLE + 1.2f},
                     {tipX, tipTE - 0.8f}, {tipX - side * 0.9f, tipTE + 0.3f},
                     {cx + side * 12.4f, rootTE - 0.5f}, {cx - 1.0f, rootTE}},
                    body);
        // Leading-edge highlight, trailing-edge shade, aileron seam.
        hi.stroke({{cx + side * 1.2f, rootLE + 0.2f},
                   {cx + side * 9.6f, rootLE},
                   {tipX - side * 1.1f, tipLE + 0.7f}}, 0.9f, bodyHi);
        hi.stroke({{cx + side * 12.8f, rootTE - 0.9f},
                   {tipX - side * 1.3f, tipTE + 0.1f}}, 0.85f, bodyDk);
        hi.stroke({{cx + side * 19.0f, tipTE + 1.1f},
                   {tipX - side * 1.7f, tipTE - 0.1f}}, 0.45f, seam);
        hi.stroke({{cx + side * 14.2f, cy + 1.6f},
                   {tipX - side * 2.0f, cy + 0.3f}}, 0.4f, seamLt);
    }

    // --- national insignia: star-and-bars on both wings (Pacific theatre) --
    for (int side = -1; side <= 1; side += 2) {
        const float ix = cx + side * 17.0f, iy = cy + 0.2f;
        hi.fillRoundRect(ix - 2.7f, iy - 0.65f, 5.4f, 1.3f, 0.65f, kWhite);
        hi.fillEllipse(ix, iy, 2.4f, 2.4f, kInsigniaBlue);
        std::vector<std::pair<float, float>> star;
        for (int i = 0; i < 10; ++i) {
            const float a = -1.5708f + static_cast<float>(i) * 0.6283f;
            const float r = (i % 2 == 0) ? 1.8f : 0.72f;
            star.push_back({ix + r * std::cos(a), iy + r * std::sin(a)});
        }
        hi.fillPoly(star, kWhite);
    }

    // --- engines + booms: long bulged cowl running from the prop plane back
    // over the wing, slim boom aft into the fin; exhaust stubs + red band ---
    for (int side = -1; side <= 1; side += 2) {
        const float ex = cx + side * engX;
        const float sh = tipShift * side * 0.6f;
        hi.fillRoundRect(ex - 1.3f, cy - 6.5f, 2.6f, 22.0f, 1.3f, body);   // boom
        hi.fillRoundRect(ex - 2.5f, propY + 0.4f, 5.0f, 12.6f, 2.4f, body);  // cowl
        hi.fillEllipse(ex, propY + 1.0f, 1.9f, 1.5f, steelDk);             // cowl face
        hi.fillEllipse(ex, propY + 1.3f, 1.3f, 0.9f, steelMid);
        for (int i = 0; i < 3; ++i)  // exhaust stubs on the outer flank
            hi.strokeLine(ex + side * 1.9f, cy - 7.8f + i * 1.3f,
                          ex + side * 2.6f, cy - 7.2f + i * 1.3f, 0.3f,
                          col(0x232A2E));
        hi.stroke({{ex - 1.45f, cy - 1.5f}, {ex - 1.45f, cy + 13.4f}},
                  0.8f, bodyHi);                                      // lit rail
        hi.stroke({{ex + 1.45f, cy - 1.5f}, {ex + 1.45f, cy + 13.8f}},
                  0.8f, bodyDk);                                      // shaded rail
        hi.stroke({{ex - 2.0f, cy + 4.6f}, {ex + 2.0f, cy + 4.6f}},
                  0.4f, seam);                                        // cowl join
        hi.stroke({{ex - 1.25f, cy + 12.4f}, {ex + 1.25f, cy + 12.4f}},
                  0.7f, col(0x8E2B22));  // recognition band (reference marking)
        // Vertical fin on the boom end (top-down: a rounded tab leaning out).
        hi.fillRoundRect(ex - 1.25f + (side > 0 ? sh : -sh * 0.5f),
                         cy + 11.0f, 2.5f, 5.2f, 1.1f, body);
        hi.stroke({{ex + (side > 0 ? 1.0f : -1.0f) + (side > 0 ? sh : -sh * 0.5f),
                    cy + 11.9f},
                   {ex + (side > 0 ? 1.0f : -1.0f) + (side > 0 ? sh : -sh * 0.5f),
                    cy + 15.5f}}, 0.5f, side > 0 ? bodyDk : seam);
        // Spinner on the prop plane, dark with a lit shoulder.
        hi.fillEllipse(ex, propY, 1.0f, 1.05f, col(0x39424A));
        hi.fillEllipse(ex - 0.28f, propY - 0.32f, 0.38f, 0.33f, col(0x93A4B0));
    }

    // --- H-tail stabilizer slung between the booms, tips just outboard ----
    {
        const float le = cy + 12.6f, te = cy + 16.0f;
        hi.fillPoly({{cx - 4.0f, le - 0.4f}, {cx + 4.0f, le - 0.4f},
                     {cx + 11.4f + tipShift, le + 0.4f},
                     {cx + 13.4f + tipShift, le + 1.1f},
                     {cx + 13.1f + tipShift, te - 0.5f},
                     {cx + 6.0f, te + 0.6f}, {cx - 6.0f, te + 0.6f},
                     {cx - 13.1f - tipShift, te - 0.5f},
                     {cx - 13.4f - tipShift, le + 1.1f},
                     {cx - 11.4f - tipShift, le + 0.4f}}, body);
        // Elevator hinge seam and lit leading edge.
        hi.stroke({{cx - 12.9f - tipShift, le + 0.8f}, {cx - 5.0f, le + 0.0f},
                   {cx + 5.0f, le + 0.0f}, {cx + 12.9f + tipShift, le + 0.8f}},
                  0.6f, bodyHi);
        hi.stroke({{cx - 12.6f - tipShift, te - 1.3f},
                   {cx + 12.6f + tipShift, te - 1.3f}}, 0.45f, seam);
        hi.stroke({{cx - 12.2f - tipShift, te - 0.6f},
                   {cx + 12.2f + tipShift, te - 0.6f}}, 0.55f, bodyDk);
    }

    // --- centre nacelle: long gun nose (guns, not a prop), greenhouse
    // canopy, tail cone tapering to a point just aft of the wing -----------
    {
        std::vector<std::pair<float, float>> pts;
        const float yNose = cy - 11.8f, yTail = cy + 8.4f;
        auto hwAt = [](float t) {  // t: 0 nose .. 1 tail
            if (t < 0.08f) return 0.25f + t * 14.0f;        // gun tip
            if (t < 0.42f) return 1.37f + 0.68f * (t - 0.08f) / 0.34f;
            if (t < 0.60f) return 2.05f;                    // cockpit shoulders
            return 2.05f - 1.5f * (t - 0.60f) / 0.40f;      // tail cone
        };
        for (int i = 0; i <= 12; ++i)
            pts.push_back({cx + hwAt(i / 12.0f), yNose + (yTail - yNose) * i / 12.0f});
        for (int i = 12; i >= 0; --i)
            pts.push_back({cx - hwAt(i / 12.0f), yNose + (yTail - yNose) * i / 12.0f});
        hi.fillPoly(pts, body);
        hi.stroke({{cx - 1.3f, yNose + 3.0f}, {cx - 1.6f, cy + 4.5f}}, 0.9f, bodyHi);
        hi.stroke({{cx + 1.4f, yNose + 3.0f}, {cx + 1.7f, cy + 5.0f}}, 0.9f, bodyDk);
        // Raised gun deck with the four nose-gun ports.
        hi.fillEllipse(cx, cy - 8.4f, 1.0f, 2.0f, bodyDk);
        for (const float gy : {-10.1f, -9.3f})
            for (const float gx : {-0.5f, 0.5f})
                hi.fillEllipse(cx + gx, cy + gy, 0.27f, 0.3f, steelDk);
        // Walkway strip aft of the gun deck.
        hi.stroke({{cx, cy - 5.6f}, {cx, cy - 4.2f}}, 0.5f, seamLt);
        // Greenhouse canopy: dark frame, blue glass, tan ribs, gloss.
        hi.fillEllipse(cx, cy + 0.6f, 1.8f, 4.2f, col(0x31401E));
        hi.fillEllipse(cx, cy + 0.6f, 1.35f, 3.7f, col(0x43719C));
        hi.fillEllipse(cx - 0.45f, cy - 0.6f, 0.6f, 1.5f, col(0x9FCBE8, 210));
        for (const float ry : {-1.9f, 0.4f, 2.6f})
            hi.stroke({{cx - 1.1f, cy + ry}, {cx + 1.1f, cy + ry}}, 0.4f,
                      col(0x8A7A4A));
        // Antenna mast base just aft of the canopy.
        hi.fillEllipse(cx + 0.9f, cy + 5.8f, 0.32f, 0.32f, steelDk);
    }

    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x0A0E12, 165));

    // Baked propeller disc at each engine: an edge-on spin blur — a dark
    // smoke disc with blade hints and yellow tip caps, centred on the prop
    // plane. The animated streak (prop_blur_p) pulses over these in flight.
    // Post-bake because Hi.bake() flattens translucent sources.
    for (int side = -1; side <= 1; side += 2) {
        const float ex = cx + side * engX;
        softBlendEllipse(p, ex, propY, 4.7f, 1.9f, 0x1E262D, 105);  // dark disc
        softBlendEllipse(p, ex, propY, 3.0f, 1.15f, 0x2C353D, 90);
        softBlendEllipse(p, ex - 1.6f, propY - 0.45f, 1.5f, 0.35f, 0x10161B, 85);
        softBlendEllipse(p, ex + 1.2f, propY + 0.5f, 1.3f, 0.32f, 0x10161B, 75);
        softBlendEllipse(p, ex - 3.9f, propY, 0.75f, 0.95f, 0xF2D878, 220);
        softBlendEllipse(p, ex + 3.9f, propY, 0.75f, 0.95f, 0xF2D878, 220);
        softBlendEllipse(p, ex, propY, 0.85f, 0.85f, 0xB7C3CB, 245);  // spinner
    }
    return p;
}

// ---------------------------------------------------------------------------
// Power-ups / bullets / misc
// ---------------------------------------------------------------------------
// Power-up icons (pass 3): rebuilt on the Hi supersampled raster, so every
// silhouette, cone and chevron is a smooth vector contour instead of 1px
// pixel staircases (which doubled into blockiness at 2-3x canvas upscale).
// Same designs, same palette; canvas stays 22x22 and core mass <= ~14px so
// the HUD bomb counter (14px pitch) does not overlap.
// ---------------------------------------------------------------------------

// Weapon upgrade: brass bullet riding up through a green chevron arrow.
PixelBuf makePowerupUpgrade() {
    Hi hi(22, 22);
    const std::uint32_t gr = col(0x38A038), grHi = col(0x8CD868), grDk = col(0x1E6E22);
    // Green up-chevron: round-capped strokes with dark under-edge and a lit
    // top edge tracing the arms.
    hi.stroke({{6.7f, 10.1f}, {11.0f, 4.2f}}, 1.9f, grDk);
    hi.stroke({{15.3f, 10.1f}, {11.0f, 4.2f}}, 1.9f, grDk);
    hi.stroke({{6.7f, 9.4f}, {11.0f, 3.5f}}, 2.0f, gr);
    hi.stroke({{15.3f, 9.4f}, {11.0f, 3.5f}}, 2.0f, gr);
    hi.stroke({{7.3f, 8.4f}, {11.0f, 3.4f}}, 0.6f, grHi);
    // Bullet along the shaft: copper tip, brass body, lit left rail.
    hi.fillEllipse(11.0f, 10.4f, 2.15f, 2.35f, col(0xE8A040));
    hi.fillEllipse(10.3f, 9.6f, 0.6f, 0.75f, col(0xF8E0A8));
    hi.fillRoundRect(9.0f, 11.4f, 4.0f, 5.6f, 0.7f, col(0xC8902C));
    hi.stroke({{9.65f, 12.1f}, {9.65f, 16.5f}}, 0.85f, col(0xE8C060));
    hi.stroke({{12.35f, 12.1f}, {12.35f, 16.5f}}, 0.85f, col(0x8A6420));
    hi.fillRoundRect(8.9f, 16.8f, 4.2f, 1.3f, 0.45f, col(0x6E4E18));  // base rim
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    PixelBuf out(22, 22);
    softBlendEllipse(out, 12.5f, 14.0f, 8.5f, 8.0f, 0x000000, 60);    // soft shadow
    out.blit(p, 0, 0);
    return out;
}

// Spread gun: three bullets fanning out of a steel hub.
PixelBuf makePowerupSpread() {
    Hi hi(22, 22);
    const double pi = 3.14159265358979;
    auto bullet = [&](double angDeg) {
        const double a = angDeg * pi / 180.0;
        const float dx = static_cast<float>(std::sin(a));
        const float dy = static_cast<float>(-std::cos(a));
        const float nx = -dy, ny = dx;                             // perpendicular
        const float hx = 11.0f, hy = 17.5f;
        hi.strokeLine(hx + dx * 1.0f, hy + dy * 1.0f,
                      hx + dx * 9.8f, hy + dy * 9.8f, 1.5f, col(0x1A1A1A));   // casing
        hi.strokeLine(hx + dx * 1.0f, hy + dy * 1.0f,
                      hx + dx * 10.3f, hy + dy * 10.3f, 1.1f, col(0xC8902C)); // brass
        hi.strokeLine(hx - nx * 0.45f + dx * 1.6f, hy - ny * 0.45f + dy * 1.6f,
                      hx - nx * 0.45f + dx * 8.8f, hy - ny * 0.45f + dy * 8.8f,
                      0.5f, col(0xE8C060));                                   // lit seam
        hi.fillEllipse(hx + dx * 10.5f, hy + dy * 10.5f, 1.15f, 1.15f,
                       col(0xC07828));                                          // copper nose
        hi.fillEllipse(hx + dx * 10.75f - nx * 0.35f, hy + dy * 10.75f - ny * 0.35f,
                       0.45f, 0.45f, col(0xF0E0B0));                             // tip shine
    };
    bullet(-32.0); bullet(0.0); bullet(32.0);
    // Hub plate with the classic magenta supply mark.
    hi.fillEllipse(11.0f, 17.5f, 4.6f, 4.2f, col(0x1A1A1A));
    hi.fillEllipse(11.0f, 17.0f, 3.5f, 3.1f, col(0x7C838A));
    hi.fillEllipse(10.6f, 16.5f, 2.0f, 1.8f, col(0xC868B8));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Heavy cannon: fat armour-piercing shell, copper driving band, iron nose.
PixelBuf makePowerupCannon() {
    Hi hi(22, 22);
    hi.fillEllipse(11.0f, 9.8f, 6.1f, 8.2f, col(0x1A1D20));          // silhouette ring
    hi.fillEllipse(11.0f, 9.8f, 5.1f, 7.2f, col(0x6E757C));          // steel body
    hi.fillEllipse(11.0f, 5.6f, 3.4f, 3.4f, col(0x9AA2A8));          // hardened nose
    hi.fillEllipse(10.1f, 4.8f, 0.85f, 1.05f, col(0xE8EEF2));        // nose shine
    hi.fillRoundRect(6.3f, 9.9f, 9.4f, 2.9f, 0.9f, col(0xB87830));   // copper band
    hi.stroke({{7.0f, 10.5f}, {15.0f, 10.5f}}, 0.6f, col(0xE0A858)); // band lit top
    hi.stroke({{7.0f, 12.4f}, {15.0f, 12.4f}}, 0.5f, col(0x7A4E1C)); // band seam
    hi.fillRoundRect(8.2f, 12.7f, 5.6f, 3.7f, 1.0f, col(0x565C61));  // tapered base
    hi.stroke({{14.9f, 6.2f}, {14.9f, 9.6f}}, 0.7f, col(0x2A2E33));  // dark edge
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    PixelBuf out(22, 22);
    softBlendEllipse(out, 12.0f, 12.0f, 7.0f, 8.5f, 0x000000, 60);   // soft shadow
    out.blit(p, 0, 0);
    return out;
}

// Rocket: red nose, grey body, swept fins, live exhaust flame.
PixelBuf makePowerupRocket() {
    Hi hi(22, 22);
    const std::uint32_t red = col(0xC23A2A), redHi = col(0xE85A48), redDk = col(0x8A2418);
    const std::uint32_t gyl = col(0xD8DCE0), gylHi = col(0xF4F6F8), gylDk = col(0x9AA0A6);
    // Swept fins first; the body overlaps their roots.
    hi.fillPoly({{9.2f, 11.2f}, {4.5f, 16.3f}, {4.8f, 17.8f}, {9.2f, 15.2f}},
                col(0x8A2A2A));
    hi.fillPoly({{12.8f, 11.2f}, {17.5f, 16.3f}, {17.2f, 17.8f}, {12.8f, 15.2f}},
                col(0x762424));
    // Nose cone, shaded on the right flank.
    hi.fillPoly({{11.0f, 1.6f}, {8.6f, 6.4f}, {13.4f, 6.4f}}, red);
    hi.fillPoly({{11.0f, 1.6f}, {11.0f, 6.4f}, {13.4f, 6.4f}}, redDk);
    hi.stroke({{10.3f, 3.0f}, {9.1f, 5.9f}}, 0.6f, redHi);           // cone shine
    // Body with lit/shaded rails.
    hi.fillRoundRect(8.8f, 5.6f, 4.4f, 10.2f, 0.6f, gyl);
    hi.stroke({{9.35f, 6.3f}, {9.35f, 15.1f}}, 0.9f, gylHi);
    hi.stroke({{12.65f, 6.3f}, {12.65f, 15.1f}}, 0.9f, gylDk);
    hi.fillRoundRect(8.8f, 7.6f, 4.4f, 1.5f, 0.5f, col(0x3A5A6E));   // cockpit band
    hi.fillRoundRect(9.6f, 15.4f, 2.8f, 1.4f, 0.5f, col(0x565C61));  // nozzle
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    // Exhaust: white core, gold, then a fading orange tongue (soft blend,
    // so the flame keeps its translucent tips instead of a hard pixel blob).
    softBlendEllipse(p, 11.0f, 19.9f, 1.15f, 1.4f, 0xF06820, 150);
    softBlendEllipse(p, 11.0f, 18.3f, 1.85f, 1.6f, 0xFFB028, 220);
    softBlendEllipse(p, 11.0f, 16.9f, 1.15f, 1.05f, 0xFFF4C8, 240);
    return p;
}

// Loop bomb: falling aerial bomb, steel-blue body, tail fins, yellow cap.
PixelBuf makePowerupBomb() {
    Hi hi(22, 22);
    // Tail assembly (top).
    hi.fillRoundRect(9.1f, 1.3f, 3.8f, 1.3f, 0.5f, col(0xC8A83C));  // service cap
    hi.fillRoundRect(8.3f, 2.5f, 5.4f, 3.3f, 0.7f, col(0x2E3338));  // tail block
    hi.stroke({{11.0f, 3.0f}, {11.0f, 5.2f}}, 0.6f, col(0x6E757C)); // lit rib
    hi.fillPoly({{6.5f, 2.7f}, {8.4f, 3.3f}, {8.4f, 4.5f}, {6.5f, 4.9f}},
                col(0x2E3338));                                     // left fin
    hi.fillPoly({{15.5f, 2.7f}, {13.6f, 3.3f}, {13.6f, 4.5f}, {15.5f, 4.9f}},
                col(0x2E3338));                                     // right fin
    // Body: dark ring, blue shell, lit and shaded crescents from offset
    // re-cuts — the smooth stand-in for the old single-column edge lines.
    hi.fillEllipse(11.0f, 10.0f, 5.0f, 6.6f, col(0x14181E));
    hi.fillEllipse(11.0f, 10.0f, 4.0f, 5.6f, col(0x5A7EC0));
    hi.fillEllipse(9.9f, 10.0f, 3.4f, 5.0f, col(0x9AC0E8));
    hi.fillEllipse(11.25f, 10.0f, 3.05f, 4.9f, col(0x5A7EC0));
    hi.fillEllipse(12.9f, 10.0f, 2.35f, 4.3f, col(0x2E4468));
    hi.fillEllipse(11.9f, 10.0f, 2.2f, 4.2f, col(0x5A7EC0));
    hi.fillEllipse(9.9f, 7.4f, 0.7f, 1.05f, col(0xF0F6FF));         // shine
    // Nose cone (pointing down = falling) with fuse tip.
    hi.fillPoly({{7.4f, 14.6f}, {14.6f, 14.6f}, {11.0f, 19.6f}}, col(0x4A6AA8));
    hi.fillPoly({{11.0f, 14.6f}, {14.6f, 14.6f}, {11.0f, 19.6f}}, col(0x2E4468));
    hi.fillEllipse(11.0f, 19.3f, 0.65f, 0.65f, col(0x14181E));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Repair: white first-aid crate with red cross, handle and latches.
PixelBuf makePowerupRepair() {
    Hi hi(22, 22);
    // Carry handle.
    hi.fillRoundRect(8.8f, 2.6f, 4.4f, 3.4f, 0.8f, col(0x3A4046));
    hi.fillRoundRect(10.0f, 3.7f, 2.0f, 2.3f, 0.6f, col(0x10141A));
    // Case.
    hi.fillRoundRect(4.0f, 5.0f, 14.0f, 12.2f, 1.4f, col(0x1A1A1A));  // outline
    hi.fillRoundRect(5.1f, 6.1f, 11.8f, 10.0f, 0.9f, col(0xE8E8E0));
    hi.fillRoundRect(5.1f, 6.1f, 11.8f, 1.3f, 0.65f, col(0xFFFFF8));  // lid shine
    hi.stroke({{5.5f, 9.0f}, {16.5f, 9.0f}}, 0.7f, col(0xB0B0A8));    // lid seam
    hi.stroke({{5.5f, 15.2f}, {16.5f, 15.2f}}, 0.8f, col(0x909088));  // base shade
    hi.stroke({{5.8f, 10.0f}, {5.8f, 14.6f}}, 0.7f, col(0xFFFFF8));   // lit corner
    // Red cross with under-shade.
    hi.fillRoundRect(10.2f, 10.1f, 1.9f, 4.9f, 0.5f, col(0xC23A2A));
    hi.fillRoundRect(8.1f, 11.6f, 6.1f, 1.9f, 0.5f, col(0xC23A2A));
    hi.fillRoundRect(10.2f, 14.2f, 1.9f, 0.8f, 0.35f, col(0x8A2418));
    hi.fillRoundRect(13.5f, 11.6f, 0.7f, 1.9f, 0.3f, col(0x8A2418));
    // Latches on the seam.
    hi.fillRoundRect(6.6f, 8.4f, 1.1f, 2.1f, 0.4f, col(0x60605A));
    hi.fillRoundRect(14.3f, 8.4f, 1.1f, 2.1f, 0.4f, col(0x60605A));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    PixelBuf out(22, 22);
    softBlendEllipse(out, 11.0f, 18.4f, 7.6f, 1.5f, 0x000000, 60);  // ground shadow
    out.blit(p, 0, 0);
    return out;
}

// Extra life: miniature golden fighter, nose-up like the player.
PixelBuf makePowerupLife() {
    Hi hi(22, 22);
    // Propeller.
    hi.stroke({{7.4f, 1.6f}, {14.6f, 1.6f}}, 0.9f, col(0x3A4046));
    hi.fillEllipse(11.0f, 1.6f, 0.7f, 0.95f, col(0x101010));
    // Wings: dark leading edge, lit trailing edge, white star dots.
    hi.fillRoundRect(1.8f, 8.4f, 18.4f, 3.2f, 1.2f, col(0xD8B040));
    hi.stroke({{2.8f, 9.0f}, {19.2f, 9.0f}}, 0.8f, col(0x8A6420));
    hi.stroke({{2.8f, 11.0f}, {19.2f, 11.0f}}, 0.7f, col(0xF4DC88));
    hi.fillEllipse(4.4f, 10.0f, 0.62f, 0.62f, kWhite);
    hi.fillEllipse(17.6f, 10.0f, 0.62f, 0.62f, kWhite);
    // Fuselage capsule with lit/shaded rails.
    hi.fillRoundRect(9.1f, 2.6f, 3.8f, 14.0f, 1.7f, col(0xF0D878));
    hi.stroke({{9.75f, 4.6f}, {9.75f, 15.2f}}, 0.8f, col(0xFFF4B0));
    hi.stroke({{12.25f, 4.6f}, {12.25f, 15.2f}}, 0.8f, col(0xB88E28));
    hi.fillRoundRect(9.7f, 3.0f, 2.6f, 2.0f, 0.7f, col(0x8A6420));  // spinner cowl
    hi.fillRoundRect(9.8f, 6.8f, 2.4f, 2.8f, 0.8f, col(0x3A5A6E));  // canopy
    // Tailplane + rudder.
    hi.fillRoundRect(5.6f, 14.6f, 10.8f, 2.2f, 0.9f, col(0xD8B040));
    hi.stroke({{6.3f, 15.1f}, {15.7f, 15.1f}}, 0.6f, col(0x8A6420));
    hi.fillRoundRect(10.1f, 16.3f, 1.8f, 1.3f, 0.5f, col(0xB88E28));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

PixelBuf makeBulletPlayer() {
    Hi hi(4, 10);
    hi.fillEllipse(2.0f, 4.6f, 1.7f, 4.6f, col(0xFFB028, 110));   // warm glow
    hi.fillEllipse(2.0f, 3.8f, 0.95f, 3.6f, col(0xFFE870));       // tracer body
    hi.fillEllipse(2.0f, 2.9f, 0.6f, 2.3f, col(0xFFFFF0));        // hot core
    return hi.bake();
}
PixelBuf makeBulletCannon() {
    Hi hi(7, 13);
    hi.fillEllipse(3.5f, 5.0f, 3.2f, 4.8f, col(0xE85A20, 120));   // ember glow
    hi.fillEllipse(3.5f, 4.2f, 2.1f, 4.0f, col(0xFF9830));        // shell fire
    hi.fillEllipse(3.5f, 3.0f, 1.0f, 2.4f, col(0xFFF4D8));        // white core
    return hi.bake();
}
PixelBuf makeBulletRocket() {
    Hi hi(6, 14);
    hi.fillEllipse(3.0f, 11.0f, 1.5f, 3.0f, col(0xFF7020, 130));  // exhaust plume
    hi.fillEllipse(3.0f, 10.0f, 0.9f, 2.0f, col(0xFFE8A0, 220));  // flame core
    hi.fillPoly({{1.0f, 8.2f}, {3.0f, 7.0f}, {5.0f, 8.2f}, {5.0f, 9.6f},
                 {3.0f, 8.8f}, {1.0f, 9.6f}}, col(0x8A2A2A));     // fins
    hi.fillRoundRect(2.0f, 1.0f, 2.0f, 8.0f, 0.9f, col(0xD8DCE0)); // body
    hi.fillEllipse(3.0f, 1.6f, 1.0f, 1.5f, col(0xE05040));        // nose
    hi.stroke({{2.3f, 2.6f}, {2.3f, 7.6f}}, 0.6f, col(0xF6F8FA)); // lit edge
    return hi.bake();
}
PixelBuf makeBulletEnemy() {
    Hi hi(5, 5);
    hi.fillEllipse(2.5f, 2.5f, 2.5f, 2.5f, col(0xE8482A, 110));   // glow halo
    hi.fillEllipse(2.5f, 2.5f, 1.45f, 1.45f, col(0xFF7A40));
    hi.fillEllipse(2.3f, 2.2f, 0.7f, 0.7f, col(0xFFE8C8));        // hot spot
    return hi.bake();
}
PixelBuf makeShellEnemy() {
    Hi hi(8, 8);
    hi.fillEllipse(4, 4, 3.9f, 3.9f, col(0x2A2A30));
    hi.fillEllipse(4, 4, 2.6f, 2.6f, col(0xB85A20));
    hi.fillEllipse(3.3f, 3.3f, 1.15f, 1.15f, col(0xFFE8A0));
    return hi.bake();
}
PixelBuf makeFlak() {
    Hi hi(8, 8);
    hi.fillEllipse(4.0f, 4.2f, 3.7f, 3.5f, col(0x505050, 150));   // smoke body
    hi.fillEllipse(3.1f, 3.2f, 2.1f, 1.9f, col(0x9C9C9C, 190));   // lit puff
    hi.fillEllipse(2.5f, 2.6f, 0.9f, 0.8f, col(0xD8D8D4, 210));
    return hi.bake();
}

PixelBuf makeShadow() {
    Hi hi(20, 12);
    hi.fillEllipse(10, 6, 9, 5, col(0x000000, 70));
    hi.fillEllipse(10, 6, 6, 3, col(0x000000, 50));
    return hi.bake();
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------
// Seamless value noise on a 32px lattice (wraps exactly at tile borders).
float tileNoise(int x, int y, std::uint64_t seed) {
    auto h = [&](int ix, int iy) {
        std::uint64_t v = seed + static_cast<std::uint64_t>(ix & 31) * 374761393ull +
                          static_cast<std::uint64_t>(iy & 31) * 668265263ull;
        v = (v ^ (v >> 13)) * 1274126177ull;
        v ^= v >> 16;
        return static_cast<float>(v & 0xFFFF) / 65535.0f;
    };
    const float fx = x / 8.0f, fy = y / 8.0f;   // 8px cells -> period 32
    const int x0 = static_cast<int>(std::floor(fx)) & 3, y0 = static_cast<int>(std::floor(fy)) & 3;
    const int x1 = (x0 + 1) & 3, y1 = (y0 + 1) & 3;
    const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    const float a = h(x0, y0) * (1 - sx) + h(x1, y0) * sx;
    const float b = h(x0, y1) * (1 - sx) + h(x1, y1) * sx;
    return a * (1 - sy) + b * sy;
}

// Blend a tile colour brighter/darker by n (-1..1).
std::uint32_t shadeBy(std::uint32_t c, float n) {
    const float f = 1.0f + n;
    return shade(c, f);
}

PixelBuf makeOceanTile(int /*variant*/) {
    // Solid deep-blue open-ocean base: deliberately featureless so no tile
    // seams or per-tile patterns are ever visible. All surface character
    // comes from the separate drifting glint layer (tile_ocean_glint),
    // whose soft light wave lines flow down the screen over this field.
    PixelBuf p(32, 32);
    p.fill(col(0x153763));
    return p;
}

PixelBuf makeOceanGlintTile() {
    // Seamless 128px surface-glint overlay: a handful of long, feathered,
    // low-alpha highlight strokes (wide crests catching sunlight), each at
    // its own height, length and brightness, plus a few very faint short
    // twinkle dashes. Positions are deliberately scattered rather than
    // evenly spaced so the eye never locks onto the tile lattice. The layer
    // drifts slowly downward over open water, which is what gives the ocean
    // its gentle living surface. Peak alpha stays far below any in-game
    // bullet, flame or pickup.
    PixelBuf p(128, 128);   // starts fully transparent
    Rng rng(0x611A70u);
    auto wset = [&](int x, int y, std::uint32_t c) {
        p.set(((x % 128) + 128) % 128, ((y % 128) + 128) % 128, c);
    };
    const float k = 6.2831853f / 128.0f;
    for (int i = 0; i < 5; ++i) {
        const int y0 = rng.rangeInt(6, 118);          // scattered rows
        const float ph = rng.range(0.0f, 6.2831853f);
        const float amp = rng.range(1.5f, 3.0f);
        const int x0 = rng.rangeInt(0, 110);
        const int len = rng.rangeInt(42, 88);
        const float bright = rng.range(0.62f, 1.0f);  // per-stroke strength
        const int wig = rng.chance(0.5f) ? 2 : 1;     // undulation speed
        for (int q = 0; q < len; ++q) {
            const int x = x0 + q;
            // Undulation period divides the tile width, so strokes that
            // wrap across the border continue seamlessly.
            const int yy = y0 + static_cast<int>(
                                   std::sin(x * wig * k + ph) * amp + 0.5f);
            const float t = static_cast<float>(q) / static_cast<float>(len);
            const float fade = std::sin(3.1415927f * t) * bright;  // soft ends
            auto A = [&](float m) {
                return static_cast<std::uint8_t>(
                    std::clamp(fade * m, 0.0f, 255.0f));
            };
            wset(x, yy - 1, col(0x9CC2E0, A(40)));    // crest highlight
            wset(x, yy, col(0x6E9AC4, A(22)));        // body
            wset(x, yy + 2, col(0x0E2A4E, A(26)));    // soft trough shadow
        }
    }
    // A few faint short twinkle dashes for sparkle variety.
    for (int i = 0; i < 5; ++i) {
        const int x0 = rng.rangeInt(0, 120), y0 = rng.rangeInt(0, 127);
        const int len = rng.rangeInt(4, 9);
        const std::uint8_t a = static_cast<std::uint8_t>(rng.rangeInt(14, 24));
        for (int q = 0; q < len; ++q) wset(x0 + q, y0, col(0x8FB4D6, a));
    }
    return p;
}

PixelBuf makeOceanGlintFarTile() {
    // Second, larger glint sheet (192px): three long, very faint swells at
    // a different spacing than the near layer. The two layers drift at
    // different speeds, so their overlap never settles into a visible
    // lattice — the wave lines appear at irregular gaps like real sea.
    PixelBuf p(192, 192);
    Rng rng(0x51F7Au);
    auto wset = [&](int x, int y, std::uint32_t c) {
        p.set(((x % 192) + 192) % 192, ((y % 192) + 192) % 192, c);
    };
    const float k = 6.2831853f / 192.0f;
    for (int i = 0; i < 3; ++i) {
        const int y0 = rng.rangeInt(10, 178);
        const float ph = rng.range(0.0f, 6.2831853f);
        const float amp = rng.range(2.0f, 4.0f);
        const int x0 = rng.rangeInt(0, 150);
        const int len = rng.rangeInt(70, 150);
        const float bright = rng.range(0.6f, 1.0f);
        const int wig = rng.chance(0.5f) ? 2 : 1;
        for (int q = 0; q < len; ++q) {
            const int x = x0 + q;
            const int yy = y0 + static_cast<int>(
                                   std::sin(x * wig * k + ph) * amp + 0.5f);
            const float t = static_cast<float>(q) / static_cast<float>(len);
            const float fade = std::sin(3.1415927f * t) * bright;
            auto A = [&](float m) {
                return static_cast<std::uint8_t>(
                    std::clamp(fade * m, 0.0f, 255.0f));
            };
            wset(x, yy - 1, col(0x9CC2E0, A(30)));    // faint crest
            wset(x, yy, col(0x6E9AC4, A(16)));        // body
            wset(x, yy + 2, col(0x0E2A4E, A(20)));    // soft trough
        }
    }
    return p;
}

PixelBuf makeSandTile(int variant) {
    PixelBuf p(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float n = tileNoise(x + variant * 7, y + variant * 11, 0xBEEF01u);
            const float m = tileNoise(x * 2 + variant, y * 2, 0xBEEF02u);
            p.set(x, y, shadeBy(col(0xC2A868), (n - 0.5f) * 0.18f + (m - 0.5f) * 0.07f));
        }
    Rng rng(0x5ABDu + variant);
    for (int i = 0; i < 26; ++i)   // sparse grit
        p.set(rng.rangeInt(0, 31), rng.rangeInt(0, 31),
              rng.chance(0.5f) ? col(0xD4BC80) : col(0xA88E50));
    return p;
}

PixelBuf makeJungleTile(int variant) {
    PixelBuf p(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float n = tileNoise(x + variant * 5, y, 0xD0D001u);
            p.set(x, y, shadeBy(col(0x2E4A20), (n - 0.5f) * 0.24f));
        }
    Rng rng(0x1DFFu + variant * 101);
    // Tree canopies with grounded shadow rims and layered crowns.
    const int trees = 2 + (variant % 2);
    for (int t = 0; t < trees; ++t) {
        const int tx = rng.rangeInt(6, 26), ty = rng.rangeInt(6, 26);
        const float r = rng.range(4.0f, 6.5f);
        p.fillEllipse(static_cast<float>(tx) + 1, static_cast<float>(ty) + 1.5f, r, r * 0.8f,
                      col(0x14260C, 200));                       // cast shadow
        p.fillEllipse(static_cast<float>(tx), static_cast<float>(ty), r, r, col(0x1E3814));
        p.fillEllipse(static_cast<float>(tx - 1), static_cast<float>(ty - 1), r * 0.7f, r * 0.7f, col(0x2C4E1E));
        p.fillEllipse(static_cast<float>(tx - 2), static_cast<float>(ty - 2), r * 0.35f, r * 0.35f, col(0x4A7031));
    }
    return p;
}

PixelBuf makeShoreTile(bool oceanAtTop) {
    // One coastline per tile: water on one side, sand on the other, with a
    // foam line between. oceanAtTop=false is the land->sea direction
    // (leaving an island). Band filling (Application::renderTerrain) uses
    // this tile for exactly one row, so a shore shows a single coastline.
    PixelBuf p(32, 32);
    const PixelBuf ocean = makeOceanTile(0);
    const PixelBuf sand = makeSandTile(1);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const int ly = 14 + (x / 8);
            const bool isOcean = oceanAtTop ? (y < ly) : (y > ly + 2);
            p.set(x, y, isOcean ? ocean.get(x, y) : sand.get(x, y));
        }
    for (int x = 0; x < 32; ++x) {
        const int ly = 14 + (x / 8);
        if (oceanAtTop) {
            p.set(x, ly, kWhite);
            p.set(x, ly - 1, col(0xC8DCF0));
        } else {
            p.set(x, ly + 2, kWhite);
            p.set(x, ly + 3, col(0xC8DCF0));
        }
    }
    return p;
}

PixelBuf makeRoadTile(bool vertical) {
    PixelBuf p = makeSandTile(7);
    if (vertical) {
        p.fillRect(10, 0, 12, 32, col(0x70706A));
        p.fillRect(10, 0, 1, 32, col(0x585852));
        p.fillRect(21, 0, 1, 32, col(0x585852));
        p.fillRect(15, 2, 2, 10, kWhite);  // dashed centerline
        p.fillRect(15, 18, 2, 10, kWhite);
    } else {
        p.fillRect(0, 10, 32, 12, col(0x70706A));
        p.fillRect(0, 15, 10, 2, kWhite);
        p.fillRect(18, 15, 14, 2, kWhite);
    }
    return p;
}

PixelBuf makeAirfieldTile() {
    // Apron concrete: seamless slab panels with subtle tone drift, so the
    // airfield reads as one poured surface instead of a grey checkerboard.
    PixelBuf p(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float n = tileNoise(x, y, 0xAF1u) - 0.5f;
            p.set(x, y, shadeBy(col(0x84847E), n * 0.10f));
        }
    // Slab seams every 16px with a lit lower lip.
    for (int s = 0; s < 32; s += 16) {
        for (int q = 0; q < 32; ++q) {
            p.set(q, s, col(0x6E6E68));
            p.set(s, q, col(0x6E6E68));
            p.set(q, (s + 1) & 31, col(0x90908A, 160));
        }
    }
    Rng rng(0xAF2);
    for (int i = 0; i < 8; ++i)  // oil stains / patch spewl
        p.fillEllipse(static_cast<float>(rng.rangeInt(2, 29)),
                      static_cast<float>(rng.rangeInt(2, 29)),
                      rng.range(0.8f, 1.8f), rng.range(0.6f, 1.2f), col(0x767670, 120));
    return p;
}

PixelBuf makeRunwayTile() {
    // The airfield's centre lanes: dark asphalt with one continuous
    // centreline dash per tile (drawn only on the runway columns).
    PixelBuf p(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float n = tileNoise(x, y, 0xAF3u) - 0.5f;
            p.set(x, y, shadeBy(col(0x54565A), n * 0.10f));
        }
    p.vline(1, 1, 31, col(0x484A4E));    // lane seams at the tile edge pair
    p.vline(30, 1, 31, col(0x484A4E));
    p.fillRect(14, 6, 4, 20, col(0xE8E8E2));  // centreline dash
    p.fillRect(14, 6, 1, 20, col(0xD8D8D2));
    return p;
}

PixelBuf makeFortressTile() {
    PixelBuf p(32, 32);
    p.fill(kConcreteDk);
    for (int y = 0; y < 32; y += 8) {
        const int off = (y / 8) % 2 == 0 ? 0 : 8;
        for (int x = -8; x < 32; x += 16) {
            // Per-brick tone drift keeps the wall from reading as a grid.
            const float n = tileNoise(x + off + 8, y, 0xF02u) - 0.5f;
            p.fillRect(x + off + 1, y + 1, 14, 6, shade(kConcrete, 0.95f + n * 0.10f));
            p.fillRect(x + off + 1, y + 1, 14, 1, shade(kConcrete, 1.15f + n * 0.08f));
            p.fillRect(x + off + 1, y + 6, 14, 1, shade(kConcreteDk, 1.08f));  // soft mortar
        }
    }
    for (int i = 0; i < 10; ++i)  // sparse weathering speckle
        p.set(static_cast<int>(tileNoise(i * 3, i * 7, 0xF03u) * 31.0f),
              static_cast<int>(tileNoise(i * 5, i * 2, 0xF04u) * 31.0f),
              col(0x5E5E56, 140));
    return p;
}

PixelBuf makeGrassEdgeTile() {
    // Island interior edge: sand ring blended into jungle.
    PixelBuf p = makeJungleTile(4);
    for (int x = 0; x < 32; ++x) {
        p.set(x, 0, col(0xC2A868));
        p.set(x, 1, col(0xC2A868));
    }
    return p;
}

// ---------------------------------------------------------------------------
// Bosses
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Battleship Yamashiro-class (pass 3): 208x168 canvas, hull centred, bow
// DOWN. damage: 0 intact, 1 light, 2 heavy, 3 wreck. Reads as a classic
// steel capital ship at a glance: long narrow 2:1 silhouette, grey steel
// decks (no teak), raked fine bow, rounded cruiser stern, single funnel,
// aft catapult + floatplane, triple-gun turrets with long protruding
// barrels. Component legibility comes from the sprites' own silhouettes —
// no painted-on target rings.
// ---------------------------------------------------------------------------
namespace ship {
constexpr int W = 208, H = 168;
constexpr int yTop = 6, yBot = 162;
constexpr float L = static_cast<float>(yBot - yTop);

inline float half(float t) {
    // 0 = stern (top) .. 1 = bow tip (bottom). Long narrow warship hull.
    if (t < 0.04f) return 30.0f + 8.0f * std::sqrt(std::max(0.0f, t / 0.04f));
    if (t < 0.55f) return 38.0f;                                   // straight waist
    if (t < 0.70f) return 38.0f - 4.0f * ((t - 0.55f) / 0.15f);    // sheer forward
    if (t < 0.86f) return 34.0f - 14.0f * ((t - 0.70f) / 0.16f);   // foredeck taper
    return std::max(0.0f, 20.0f * (1.0f - (t - 0.86f) / 0.14f));   // raked bow
}
}  // namespace ship

PixelBuf makeBattleshipHull(int damage) {
    using namespace ship;
    const int cx = W / 2;
    PixelBuf p(W, H);
    auto halfAt = [&](int y) { return half(static_cast<float>(y - yTop) / L); };

    // --- water shadow -------------------------------------------------------
    for (int y = yTop; y <= yBot + 5; ++y) {
        const int hw = static_cast<int>(halfAt(std::min(y - 4, yBot))) + 3;
        for (int x = -hw; x <= hw; ++x)
            p.set(cx + x + 4, y + 5, col(0x050F1A, 110));
    }
    // --- bow wave, spray & side foam ----------------------------------------
    for (int k = 0; k < 64; ++k) {
        const float a = static_cast<float>(k) / 63.0f;
        const float bowY = yBot - 4.0f - 40.0f * a;
        const float spread = 4.0f + 30.0f * a;
        const std::uint32_t foam = col(0xE8F2FA, static_cast<std::uint8_t>(200 - 160 * a));
        for (int sx : {-1, 1}) {
            const int fx = cx + static_cast<int>(sx * (halfAt(static_cast<int>(bowY)) + spread));
            p.set(fx, static_cast<int>(bowY), foam);
            p.set(fx + sx, static_cast<int>(bowY) + 1, col(0xC8DCF0, 110));
            if (k % 7 == 0) p.set(fx + sx * 3, static_cast<int>(bowY) - 2, col(0xFFFFFF, 150));
        }
    }
    for (int y = yTop + 6; y < yBot - 8; y += 3) {
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw - 2, y, col(0xC8DCF0, 70));
        p.set(cx + hw + 2, y + 1, col(0xC8DCF0, 70));
    }

    // --- AA sponsons first (shoulders sticking out past the belt) -----------
    for (int sx : {-1, 1}) {
        const int ex = cx + sx * 46;                 // sponson outer edge
        const int ix = cx + sx * 34;
        for (int y = 74; y <= 94; ++y)
            for (int x = std::min(ix, ex); x <= std::max(ix, ex); ++x) {
                const int edge = sx > 0 ? ex - x : x - ex;
                std::uint32_t c = col(0x4E555C);
                if (edge <= 1) c = col(0x23282D);
                else if (y == 74 || y == 94) c = col(0x2E343A);
                else if ((y % 5) == 0) c = col(0x434A50);
                p.set(x, y, c);
            }
        // Support struts where the sponson overhangs the belt.
        for (int y : {78, 84, 90}) p.set(cx + sx * (halfAt(y) + 3), y, col(0x1E2226));
    }

    // --- hull body with armour-belt plates ----------------------------------
    for (int y = yTop; y <= yBot; ++y) {
        const int hw = static_cast<int>(halfAt(y));
        for (int x = -hw; x <= hw; ++x) {
            const int dEdge = hw - std::abs(x);
            std::uint32_t c = col(0x4E555C);
            if (dEdge <= 1) c = col(0x23282D);
            else if (dEdge <= 4) {
                // Alternating belt plates (12px blocks) with dark seams.
                const int plate = (y / 12) % 2;
                c = plate ? col(0x3E444A) : col(0x495057);
                if (y % 12 == 0) c = col(0x2C3136);
                if (dEdge <= 2) c = col(0x33383D);
            }
            else if (x < 0 && y < yTop + static_cast<int>(L) / 2) c = col(0x596067);
            p.set(cx + x, y, c);
        }
    }

    // --- decks: weathered steel with panel seams -----------------------------
    for (int y = yTop + 3; y <= yBot - 3; ++y) {
        const bool forecastle = y > yTop + static_cast<int>(L) * 3 / 4;
        const int inset = forecastle ? 6 : 5;
        const int hw = static_cast<int>(halfAt(y)) - inset;
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x626970);
            if ((y % 4) == 0) c = col(0x525A61);
            else if ((x + y) % 17 == 0) c = col(0x6E757C);
            p.set(cx + x, y, c);
        }
        // Forecastle break edge (step in deck height).
        if (!forecastle && y == yTop + static_cast<int>(L) * 3 / 4)
            for (int x = -hw; x <= hw; ++x) p.set(cx + x, y, col(0x23282D));
    }
    for (int y = yTop + 6; y < yBot - 6; ++y)
        if (std::abs(y % 4) != 3) p.set(cx, y, col(0x7C838A));   // centreline walkway

    // --- barbette collars for the main turrets (match JSON offsets) ----------
    for (int dy : {-48, 44}) {
        const float by = static_cast<float>(H / 2 + dy);
        // Cast shadow sells the turret riding ABOVE the deck.
        p.fillEllipse(static_cast<float>(cx) + 3.0f, by + 4.0f, 21.0f, 21.0f,
                      col(0x0A0806, 90));
        p.fillEllipse(static_cast<float>(cx), by, 20.5f, 20.5f, col(0x2E3338));
        p.fillEllipse(static_cast<float>(cx), by, 18.0f, 18.0f, col(0x474E54));
        p.fillEllipse(static_cast<float>(cx), by - 1, 13.0f, 13.0f, col(0x33383D));
        for (int r = 0; r < 12; ++r) {                     // collar bolts
            const float a = r * 0.5236f;
            p.set(cx + static_cast<int>(19.5f * std::cos(a)),
                  static_cast<int>(by + 19.5f * std::sin(a)), col(0x6E757C));
        }
    }

    // --- AA pedestals on the sponsons (match JSON offsets) -------------------
    for (int sx : {-1, 1}) {
        const float ax = static_cast<float>(cx + sx * 30);
        p.fillEllipse(ax, 85.0f, 13.0f, 12.0f, col(0x23282D));
        p.fillEllipse(ax, 84.0f, 11.5f, 10.5f, col(0x4E555C));
    }

    // --- funnel amidships with cap, platforms and catwalks -------------------
    p.fillEllipse(static_cast<float>(cx) + 2.0f, 102.0f, 9.5f, 10.5f, col(0x0A0806, 90));
    p.fillEllipse(static_cast<float>(cx), 100.0f, 9.0f, 10.0f, col(0x3A4046));
    p.fillEllipse(static_cast<float>(cx), 99.0f, 7.0f, 8.0f, col(0x17191B));
    p.fillEllipse(static_cast<float>(cx - 2), 97.0f, 2.5f, 2.5f, col(0x565C61));
    p.hline(cx - 15, cx + 15, 106, col(0x8A9094, 140));
    for (int sx : {-1, 1})                                 // wing platforms
        p.fillRect(cx + (sx > 0 ? 9 : -14), 96, 5, 8, col(0x4E555C));

    // --- aft catapult deck with floatplane (stern, clear of rear barbette) ---
    p.fillRect(cx - 6, yTop + 3, 12, 18, col(0x3E444A));   // catapult track plate
    p.vline(cx - 4, yTop + 3, yTop + 20, col(0x2C3136));
    p.vline(cx + 4, yTop + 3, yTop + 20, col(0x2C3136));
    {   // floatplane silhouette, wings folded-ish, pointing to starboard
        const int px0 = cx, py0 = yTop + 11;
        p.fillRect(px0 - 7, py0 - 1, 13, 3, col(0x9AA2A8));    // fuselage
        p.fillRect(px0 - 2, py0 - 4, 4, 9, col(0x8A9298));     // wings
        p.fillRect(px0 - 7, py0 - 3, 2, 2, col(0x7A8288));     // tail
        p.set(px0 + 6, py0, col(0x2C3136));                    // prop disc
    }
    for (int sx : {-20, 20}) p.set(cx + sx, yTop + 4, col(0x858C93));

    // --- boat deck: two whalers + crane on the starboard waist ---------------
    for (int by : {52, 66}) {
        p.fillEllipse(static_cast<float>(cx + 24), static_cast<float>(by), 3.0f, 6.5f,
                      col(0x6E757C));
        p.fillEllipse(static_cast<float>(cx + 23), static_cast<float>(by - 1), 1.8f, 4.5f,
                      col(0x8A9298));
        p.hline(cx + 20, cx + 28, by, col(0x33383D));          // davit
    }
    p.fillRect(cx - 26, 60, 4, 4, col(0x565C61));              // crane pedestal
    p.fillRect(cx - 26, 56, 2, 8, col(0x7C838A));              // crane arm

    // --- casemate secondary guns along the belt ------------------------------
    for (int dy : {-20, -10, 12, 22}) {
        const int y = H / 2 + dy - 4;
        for (int sx : {-1, 1}) {
            const int hw = static_cast<int>(halfAt(y));
            const int gx = cx + sx * (hw - 4);
            p.fillRect(gx - 1, y - 2, 3, 5, col(0x2B2F33));
            p.set(gx + sx * 2, y, col(0x9AA2A8));              // lit muzzle tip
        }
    }

    // --- bow & stern fittings --------------------------------------------------
    for (int sx : {-1, 1}) {                                  // anchor recesses
        const int ax = cx + sx * 16;
        p.fillRect(ax - 2, yBot - 36, 5, 4, col(0x33383D));
        p.set(ax, yBot - 34, col(0x858C93));
        for (int i = 1; i <= 5; ++i)                          // anchor chain
            p.set(ax + sx * (1 + i / 2), yBot - 38 - i, col(0x6E757C));
    }
    for (int sx : {-1, 1})                                    // breakwater strokes
        for (int i = 0; i < 7; ++i)
            p.set(cx + sx * (10 - i), yBot - 24 - i * 2, col(0x7C838A, 180));
    for (int sx : {-22, 22}) p.set(cx + sx, yTop + 4, col(0x858C93));
    // Vents & hatches scatter (tertiary, low contrast).
    for (int k = 0; k < 14; ++k) {
        const int vx = cx - 28 + (k * 61) % 57;
        const int vy = yTop + 16 + (k * 97) % (static_cast<int>(L) - 32);
        if (k % 3 == 0) { p.fillRect(vx, vy, 2, 3, col(0x858C93, 200)); }
        else if (k % 3 == 1) { p.fillRect(vx, vy, 3, 3, col(0x4E555C)); p.set(vx, vy, col(0x7C838A)); }
    }
    // Rust streaks.
    for (int k = 0; k < 14; ++k) {
        const int rx = cx - 32 + (k * 47) % 65;
        const int ry = yTop + 12 + (k * 83) % (static_cast<int>(L) - 24);
        p.set(rx, ry, col(0x6E4A2C, 90));
        p.set(rx, ry + 1, col(0x6E4A2C, 55));
    }

    // --- damage tiers -----------------------------------------------------------
    auto scorch = [&](int dx, int dy, int r, bool ember) {
        // Keep blast marks on the (narrow) deck: clamp toward the centreline.
        const int yy = H / 2 + dy;
        const int lim = std::max(4, static_cast<int>(halfAt(yy)) - r - 2);
        dx = std::max(-lim, std::min(lim, dx));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      static_cast<float>(r), static_cast<float>(r), col(0x14100C, 145));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      r * 0.5f, r * 0.5f, col(0x0A0806, 195));
        if (ember) {
            p.set(cx + dx, yy, col(0xFF8A30));
            p.set(cx + dx + 1, yy, col(0xC25A18));
        }
        for (int q = 1; q <= 6; ++q)
            p.fillEllipse(static_cast<float>(cx + dx - q / 2),
                          static_cast<float>(yy - r - q * 2),
                          2.0f, 2.0f, col(0x2A2A2A, 75 - q * 9));
    };
    if (damage >= 1) { scorch(24, 26, 7, false); scorch(-30, -18, 6, false); }
    if (damage >= 2) {
        scorch(30, -30, 8, true); scorch(-12, 42, 7, true); scorch(36, 12, 7, true);
        // Debris spall around heavy hits.
        for (int k = 0; k < 10; ++k)
            p.set(cx + 18 + (k * 29) % 26, H / 2 - 36 + (k * 13) % 14, col(0x1E1A16, 170));
    }
    if (damage >= 3) {
        scorch(-20, -46, 10, true); scorch(14, -60, 8, true); scorch(-4, 16, 9, true);
        // Hull breach starboard waist.
        for (int i = 0; i < 13; ++i) {
            const int by = H / 2 + 2 + i;
            const int hw = static_cast<int>(halfAt(by));
            for (int x = hw - 7 + (i % 3); x <= hw; ++x) {
                p.set(cx + x, by, col(0x141210));
                if ((i + x) % 3 == 0) p.set(cx + x - 1, by, col(0x8A2A10));
            }
        }
        // Cracked foredeck seam.
        for (int i = 0; i < 16; ++i)
            p.set(cx - 14 + i + (i % 3), yTop + 108 + (i % 4) - 2, col(0x241C14, 200));
    }
    // Smooth flank contours against the sea: rim coverage from the
    // analytic hull profile, not from pixels.
    profileRim(p, static_cast<float>(cx), yTop + 1, yBot - 1,
               [&](int yy) { return halfAt(yy); });

    return p;
}

// Small hull canvas for the Sky Fortress damage variant overlays.
PixelBuf addScorchTo(PixelBuf src, int damage) {
    if (damage == 0) return src;
    struct Scorch { int dx, dy, r; };
    const Scorch a[] = {{-34, -10, 7}, {26, 14, 6}, {8, -22, 6}};
    const Scorch b[] = {{-12, 18, 8}, {40, -8, 7}, {-44, 8, 7}, {18, 6, 8}};
    for (const auto& s : a)
        for (int pass = 0; pass < 2; ++pass)
            src.fillEllipse(static_cast<float>(src.w / 2 + s.dx),
                            static_cast<float>(src.h / 2 + s.dy),
                            s.r - pass * 3.0f, s.r - pass * 3.0f,
                            pass == 0 ? col(0x14100C, 140) : col(0x0A0806, 180));
    if (damage >= 2)
        for (const auto& s : b) {
            src.fillEllipse(static_cast<float>(src.w / 2 + s.dx),
                            static_cast<float>(src.h / 2 + s.dy),
                            static_cast<float>(s.r), static_cast<float>(s.r), col(0x0E0B08, 170));
            src.fillEllipse(static_cast<float>(src.w / 2 + s.dx),
                            static_cast<float>(src.h / 2 + s.dy),
                            s.r * 0.4f, s.r * 0.4f, col(0xFF8A30));
            src.fillEllipse(static_cast<float>(src.w / 2 + s.dx),
                            static_cast<float>(src.h / 2 + s.dy),
                            s.r * 0.2f, s.r * 0.2f, col(0x060504, 220));
        }
    return src;
}

// Main gun turret 48x48: octagonal armoured face on a steel barbette collar,
// TRIPLE barrels with muzzle brakes protruding well past the body so the
// guns read instantly at gameplay scale. Flat facets lit upper-left.
PixelBuf makeBigTurret() {
    PixelBuf p(48, 48);
    const float cx = 24.0f, cy = 24.0f;
    // Barbette collar riding in its ring (no target markers — the silhouette
    // itself says "gun").
    p.fillEllipse(cx, cy + 1, 22.0f, 22.0f, col(0x23282C));
    p.fillEllipse(cx, cy, 20.5f, 20.5f, col(0x4A5056));
    p.fillEllipse(cx, cy, 18.5f, 18.5f, col(0x33383D));
    // Octagonal turret body (scanline widths).
    const int oct[20] = {7, 12, 15, 17, 17, 17, 17, 17, 16, 15,
                         16, 17, 17, 17, 17, 17, 17, 15, 12, 7};
    for (int r = 0; r < 20; ++r) {
        const int y = 14 + r;
        const int hw = oct[static_cast<std::size_t>(r)];
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x666D73);
            const int dEdge = hw - std::abs(x);
            if (dEdge <= 1) c = col(0x1E2226);                 // dark facet edge
            else if (x <= 0 && r <= 10) c = col(0x8F979E);     // lit facets
            else if (x >= 0 && r > 10) c = col(0x3E4449);      // shaded facets
            p.set(static_cast<int>(cx) + x, y, c);
        }
    }
    // Roof hatch + sight hood.
    p.fillEllipse(cx, cy - 4, 5.0f, 4.0f, col(0x51575C));
    p.fillEllipse(cx - 1, cy - 5, 2.5f, 2.0f, col(0x8A9094));
    p.fillRect(31, 15, 3, 3, col(0x1B1E21));                   // rangefinder pod
    // Mantlet bar across the barrel bases.
    p.fillRect(10, 31, 28, 6, col(0x3C4247));
    p.fillRect(10, 31, 28, 1, col(0x8A9094));
    p.fillRect(10, 36, 28, 1, col(0x1B1E21));
    // Triple barrels below the mantlet: bright gunmetal with a lit rail so
    // the guns read clearly against the deck at gameplay scale.
    for (int bx : {11, 22, 33}) {
        p.fillRect(bx + 4, 37, 2, 11, col(0x000000, 80));   // deck shadow
        p.fillRect(bx, 36, 4, 12, col(0x41474D));           // barrel body
        p.fillRect(bx, 36, 1, 12, col(0x9AA2A8));           // lit rail
        p.fillRect(bx + 3, 36, 1, 12, col(0x23272B));       // dark edge
        p.fillRect(bx - 1, 44, 6, 2, col(0x7C838A));        // muzzle brake
        p.fillRect(bx - 1, 46, 6, 1, col(0x23272B));
    }
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// AA mount 24x24: octagonal platform, shielded twin barrels pointing up.
PixelBuf makeBossAA() {
    PixelBuf p(28, 28);
    const float cx = 14.0f, cy = 16.0f;
    p.fillEllipse(cx, cy + 1, 12.5f, 11.0f, col(0x23282C));
    p.fillEllipse(cx, cy, 11.0f, 9.5f, col(0x4E5459));
    p.fillEllipse(cx - 1, cy - 1, 7.0f, 6.0f, col(0x6E757C));
    p.fillEllipse(cx - 2, cy - 2, 3.5f, 3.0f, col(0x858C93));
    // Gun shield (front arc toward the player).
    for (int x = -9; x <= 9; ++x)
        for (int y = 5; y <= 9; ++y)
            if (x * x + (y - 16) * (y - 16) <= 121 && x * x + (y - 16) * (y - 16) >= 72)
                p.set(static_cast<int>(cx) + x, y, y <= 6 ? col(0x858C93) : col(0x51575C));
    // Twin barrels with flash guards: bright gunmetal + lit rail so the
    // guns stay legible against the deck.
    for (int bx : {10, 16}) {
        p.fillRect(bx + 3, 3, 2, 13, col(0x000000, 70));   // deck shadow
        p.fillRect(bx, 1, 3, 15, col(0x41474D));
        p.fillRect(bx, 1, 1, 15, col(0x9AA2A8));
        p.fillRect(bx + 2, 1, 1, 15, col(0x23272B));
        p.fillRect(bx - 1, 1, 5, 2, col(0x6E757C));
    }
    // Seat box + ammo drum behind.
    p.fillRect(12, 17, 5, 4, col(0x3C4247));
    p.fillEllipse(20.0f, 18.0f, 2.5f, 2.5f, col(0x2B2F33));
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// Pagoda bridge 48x60: the flagship's identity piece. Three roofed tiers,
// glass window bands, rangefinder cupola, tripod mast with ensign, and a
// searchlight on the base deck. Amber ring marks it as a target.
PixelBuf makeBridge() {
    PixelBuf p(48, 60);
    const int cx = 24;
    // Base pedestal.
    p.fillEllipse(24.0f, 53.0f, 21.0f, 6.0f, col(0x23282C));
    p.fillEllipse(23.0f, 52.0f, 19.0f, 5.0f, col(0x4E5459));
    p.fillRect(6, 46, 36, 8, col(0x565D63));
    p.fillRect(6, 46, 36, 1, col(0x858C93));
    p.fillRect(40, 46, 2, 8, col(0x33383D));
    // Searchlight on the base deck.
    p.fillEllipse(11.0f, 49.0f, 3.5f, 3.5f, col(0x33383D));
    p.fillEllipse(10.5f, 48.5f, 2.2f, 2.2f, col(0xF2E8B8));
    // Lower tier.
    p.fillRect(9, 34, 30, 13, col(0x5C6368));
    p.fillRect(9, 34, 30, 1, col(0x8A9094));
    p.fillRect(37, 34, 2, 13, col(0x33383D));
    for (int x = 12; x <= 32; x += 4) {                    // lower windows
        p.fillRect(x, 37, 3, 4, col(0x3A5A6E));
        p.set(x, 37, col(0x7FB8D8));
    }
    // Pagoda roof 1 (overhanging eaves, lit top / dark underside).
    p.fillRect(5, 30, 38, 3, col(0x2A2E33));
    p.fillRect(6, 29, 36, 1, col(0x7C838A));
    p.fillRect(5, 33, 38, 1, col(0x1B1E21, 170));          // eave shadow
    // Middle tier with window band.
    p.fillRect(12, 20, 24, 10, col(0x666D73));
    p.fillRect(12, 20, 24, 1, col(0x8A9094));
    p.fillRect(34, 20, 2, 10, col(0x33383D));
    for (int x = 14; x <= 31; x += 3) {                    // glass band
        p.fillRect(x, 23, 2, 4, col(0x3A5A6E));
        p.set(x, 23, col(0x9FD0E8));
    }
    // Pagoda roof 2.
    p.fillRect(8, 17, 32, 2, col(0x2A2E33));
    p.fillRect(9, 16, 30, 1, col(0x7C838A));
    // Top control tier.
    p.fillRect(16, 9, 16, 8, col(0x5C6368));
    p.fillRect(16, 9, 16, 1, col(0x8A9094));
    p.fillRect(30, 9, 2, 8, col(0x33383D));
    p.fillRect(18, 11, 3, 3, col(0x3A5A6E));
    p.set(18, 11, col(0x9FD0E8));
    // Rangefinder cupola + arms.
    p.fillEllipse(24.0f, 7.0f, 6.5f, 3.0f, col(0x33383D));
    p.fillEllipse(23.0f, 6.0f, 3.5f, 1.5f, col(0x6E757C));
    p.hline(15, 33, 6, col(0x4E5459));
    p.set(15, 5, col(0x8A9094));
    p.set(33, 5, col(0x8A9094));
    // Tripod mast + ensign yard.
    p.vline(cx, 12, 54, col(0x2A2E33));
    p.vline(cx - 1, 12, 54, col(0x6E757C));
    p.hline(cx - 8, cx + 8, 14, col(0x4E5459));
    p.fillRect(cx + 1, 10, 6, 3, col(0xC23A2A));
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// ---------------------------------------------------------------------------
// Destroyer Hayabusa-class (boss 3): 96x196 canvas, hull centred, bow DOWN.
// Brief from the reference screenshot: a LONG NARROW hull — the beam is
// barely a quarter of the length (the battleship boss is ~1.3:1, this is
// ~4:1). Reads as a classic torpedo boat: superfiring gun pair forward,
// compact bridge with wings, raked funnel, twin triple torpedo-tube banks
// amidships, stern gun over depth-charge rails. Same steel family as the
// battleship but cooler/lighter grey so it reads as a leaner, faster hull.
// damage: 0 intact, 1 light, 2 heavy, 3 wreck.
// ---------------------------------------------------------------------------
namespace dship {
constexpr int W = 96, H = 196;
constexpr int yTop = 8, yBot = 188;
constexpr float L = static_cast<float>(yBot - yTop);

inline float half(float t) {
    // 0 = stern (top) .. 1 = bow tip (bottom).
    if (t < 0.06f) return 15.0f + 7.0f * std::sqrt(std::max(0.0f, t / 0.06f));
    if (t < 0.52f) return 22.0f;                                   // straight waist
    if (t < 0.72f) return 22.0f - 1.0f * ((t - 0.52f) / 0.20f);    // slight sheer
    if (t < 0.90f) return 21.0f - 8.0f * ((t - 0.72f) / 0.18f);    // forecastle taper
    return std::max(0.0f, 13.0f * (1.0f - (t - 0.90f) / 0.10f));   // fine raked bow
}
}  // namespace dship

// --- HEAVY CRUISER boss hull --------------------------------------------------
// A treaty heavy cruiser running full speed, bow toward the player: armour
// belt, superfiring main-gun barbettes fore and aft at dy +58/+30/-52/-78,
// secondary gun sponsons at (±26,+4), AA platforms at (±30,-30), the island
// bridge with fire-control tower at (0,-14) and a raked funnel at (0,-38).
// Barbettes and pads MUST mirror the component offsets in bosses.json.
// damage: 0 pristine, 2 scorched, 3 wreck.
namespace hcship {
constexpr int W = 120, H = 212;
constexpr int yTop = 8, yBot = 204;
constexpr float L = static_cast<float>(yBot - yTop);
inline float half(float t) {  // 0 = stern (top) .. 1 = bow tip (bottom)
    if (t < 0.06f) return 19.0f + 10.0f * std::sqrt(std::max(0.0f, t / 0.06f));
    if (t < 0.58f) return 29.0f;                                  // flush waist
    if (t < 0.80f) return 29.0f - 6.0f * ((t - 0.58f) / 0.22f);   // tumblehome
    if (t < 0.93f) return 23.0f - 10.0f * ((t - 0.80f) / 0.13f);  // forecastle
    return std::max(0.0f, 13.0f * (1.0f - (t - 0.93f) / 0.07f));  // raked bow
}
}  // namespace hcship

PixelBuf makeCruiserHull(int damage) {
    using namespace hcship;
    const int cx = W / 2;
    PixelBuf p(W, H);
    auto halfAt = [&](int y) { return half(static_cast<float>(y - yTop) / L); };

    // --- water shadow ----------------------------------------------------------
    for (int y = yTop; y <= yBot + 4; ++y) {
        const int hw = static_cast<int>(halfAt(std::min(y - 4, yBot))) + 4;
        for (int x = -hw; x <= hw; ++x)
            p.set(cx + x + 3, y + 4, col(0x050F1A, 100));
    }
    // --- bow wave & side foam ----------------------------------------------------
    for (int k = 0; k < 58; ++k) {
        const float a = static_cast<float>(k) / 57.0f;
        const float bowY = yBot - 4.0f - 38.0f * a;
        const float spread = 3.0f + 22.0f * a;
        const std::uint32_t foam = col(0xE8F2FA, static_cast<std::uint8_t>(200 - 160 * a));
        for (int sx : {-1, 1}) {
            const int fx = cx + static_cast<int>(sx * (halfAt(static_cast<int>(bowY)) + spread));
            p.set(fx, static_cast<int>(bowY), foam);
            p.set(fx + sx, static_cast<int>(bowY) + 1, col(0xC8DCF0, 110));
            if (k % 7 == 0) p.set(fx + sx * 2, static_cast<int>(bowY) - 2, col(0xFFFFFF, 150));
        }
    }
    // Sterna wake feathering behind the transom.
    for (int k = 0; k < 12; ++k) {
        const float a = static_cast<float>(k) / 11.0f;
        const int wy = yTop - 3 + k;
        const int off = 15 + static_cast<int>(10.0f * (1.0f - a));
        p.set(cx - off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(75 - 5 * k)));
        p.set(cx + off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(75 - 5 * k)));
    }
    for (int y = yTop + 6; y < yBot - 8; y += 3) {
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw - 2, y, col(0xC8DCF0, 70));
        p.set(cx + hw + 2, y + 1, col(0xC8DCF0, 70));
    }

    // --- sponson shoulders: secondaries & AA ride these (before hull body) -----
    for (int sx : {-1, 1}) {
        for (int y = H / 2 + 17; y >= H / 2 - 11; --y) {  // secondary sponsons
            const int hw = static_cast<int>(halfAt(y));
            for (int x = hw - 1; x <= hw + 5; ++x) {
                const int edge = hw + 5 - x;
                p.set(cx + sx * x, y, edge <= 1 ? col(0x23282D)
                                                : (y % 4 == 0 ? col(0x434A50) : col(0x4E555C)));
            }
        }
        for (int y = H / 2 - 20; y >= H / 2 - 42; --y) {  // AA sponsons
            const int hw = static_cast<int>(halfAt(y));
            for (int x = hw - 1; x <= hw + 7; ++x) {
                const int edge = hw + 7 - x;
                p.set(cx + sx * x, y, edge <= 1 ? col(0x23282D)
                                                : (y % 4 == 0 ? col(0x434A50) : col(0x4E555C)));
            }
        }
    }

    // --- hull body with armour belt ----------------------------------------------
    for (int y = yTop; y <= yBot; ++y) {
        const int hw = static_cast<int>(halfAt(y));
        for (int x = -hw; x <= hw; ++x) {
            const int dEdge = hw - std::abs(x);
            std::uint32_t c = col(0x545B62);
            if (dEdge <= 1) c = col(0x22272C);
            else if (dEdge <= 4) {  // thick belt plating
                const int plate = (y / 10) % 2;
                c = plate ? col(0x424950) : col(0x4D545B);
                if (y % 10 == 0) c = col(0x2B3035);
            }
            else if (x < 0) c = col(0x5F676E);
            p.set(cx + x, y, c);
        }
    }
    // --- deck with panel seams -----------------------------------------------------
    for (int y = yTop + 3; y <= yBot - 3; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 5;
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x686F76);
            if ((y % 3) == 0) c = col(0x596067);
            else if ((x + y) % 13 == 0) c = col(0x747B82);
            p.set(cx + x, y, c);
        }
    }
    for (int y = yTop + 6; y < yBot - 6; ++y)
        if (y % 3 != 2) p.set(cx, y, col(0x828990));  // centreline walkway

    // --- main gun barbette collars (match JSON offsets dy +58/+30/-52/-78) -------
    for (int dy : {58, 30, -52, -78}) {
        const float by = static_cast<float>(H / 2 + dy);
        p.fillEllipse(static_cast<float>(cx) + 2.0f, by + 3.0f, 16.5f, 16.5f,
                      col(0x0A0806, 90));
        p.fillEllipse(static_cast<float>(cx), by, 16.0f, 16.0f, col(0x2E3338));
        p.fillEllipse(static_cast<float>(cx), by, 13.5f, 13.5f, col(0x474E54));
        p.fillEllipse(static_cast<float>(cx), by - 1.0f, 9.5f, 9.5f, col(0x33383D));
        for (int r = 0; r < 12; ++r) {  // collar bolts
            const float a = r * 0.5236f;
            p.set(cx + static_cast<int>(15.0f * std::cos(a)),
                  static_cast<int>(by + 15.0f * std::sin(a)), col(0x6E757C));
        }
    }
    // Superfire step: 'B' mount rides a raised deck block behind 'A'.
    p.fillRect(cx - 16, H / 2 + 44, 32, 2, col(0x767D84));
    p.fillRect(cx - 16, H / 2 + 46, 32, 1, col(0x23282D));

    // --- secondary gun pads on the sponsons (match JSON offsets ±26,+4) ----------
    for (int sx : {-1, 1}) {
        const float px = static_cast<float>(cx + sx * 26);
        p.fillEllipse(px, static_cast<float>(H / 2 + 4), 11.5f, 11.0f, col(0x23282D));
        p.fillEllipse(px, static_cast<float>(H / 2 + 3), 10.0f, 9.5f, col(0x4E555C));
        p.fillEllipse(px, static_cast<float>(H / 2 + 2), 5.5f, 5.5f, col(0x3A4046));
    }
    // --- AA platforms (match JSON offsets ±30,-30) --------------------------------
    for (int sx : {-1, 1}) {
        const float ax = static_cast<float>(cx + sx * 30);
        p.fillEllipse(ax, static_cast<float>(H / 2 - 30), 10.5f, 10.0f, col(0x23282D));
        p.fillEllipse(ax, static_cast<float>(H / 2 - 31), 9.0f, 8.5f, col(0x4E555C));
    }

    // --- bridge blockout & fire-control tower (the bridge component rides it) ----
    p.fillRect(cx - 17, H / 2 - 24, 34, 20, col(0x3E454C));
    p.fillRect(cx - 17, H / 2 - 24, 34, 1, col(0x858C93));
    p.fillRect(cx - 20, H / 2 - 18, 4, 6, col(0x4E555C));   // bridge wings
    p.fillRect(cx + 16, H / 2 - 18, 4, 6, col(0x4E555C));
    p.fillRect(cx - 11, H / 2 - 31, 22, 8, col(0x4A5158));  // conning tower
    p.fillRect(cx - 11, H / 2 - 31, 22, 1, col(0x8A9094));
    p.fillRect(cx - 14, H / 2 - 33, 28, 2, col(0x2E3338));  // rangefinder bar
    p.fillEllipse(static_cast<float>(cx), static_cast<float>(H / 2 - 36), 3.0f, 3.0f,
                  col(0x33383D));  // director hood
    p.vline(cx - 6, H / 2 - 24, H / 2 - 5, col(0x2E3338));  // tripod legs
    p.vline(cx + 6, H / 2 - 24, H / 2 - 5, col(0x2E3338));

    // --- raked funnel amidships ------------------------------------------------------
    p.fillEllipse(static_cast<float>(cx) + 2.0f, static_cast<float>(H / 2 - 36), 8.0f, 9.0f,
                  col(0x0A0806, 90));
    p.fillEllipse(static_cast<float>(cx) + 0.5f, static_cast<float>(H / 2 - 38), 7.5f, 8.5f,
                  col(0x3A4046));
    p.fillEllipse(static_cast<float>(cx) + 1.0f, static_cast<float>(H / 2 - 39), 5.5f, 6.5f,
                  col(0x17191B));
    p.fillEllipse(static_cast<float>(cx - 1), static_cast<float>(H / 2 - 41), 1.8f, 1.8f,
                  col(0x565C61));
    p.hline(cx - 12, cx + 12, H / 2 - 15, col(0x8A9094, 140));  // bridge walkway

    // --- stern crane over the after deck ------------------------------------------------
    p.fillRect(cx - 1, H / 2 - 66, 2, 10, col(0x565C61));
    p.fillRect(cx - 4, H / 2 - 66, 8, 2, col(0x7C838A));

    // --- bow & stern fittings ---------------------------------------------------------
    for (int sx : {-1, 1}) {  // anchor recesses
        const int ax = cx + sx * 13;
        p.fillRect(ax - 2, yBot - 30, 4, 3, col(0x33383D));
        p.set(ax, yBot - 29, col(0x858C93));
    }
    for (int sx : {-1, 1})  // breakwater strokes
        for (int i = 0; i < 6; ++i)
            p.set(cx + sx * (8 - i), yBot - 18 - i * 2, col(0x7C838A, 180));
    // Portholes along the belt.
    for (int k = 0; k < 13; ++k) {
        const int py = yTop + 20 + k * 12;
        const int hw = static_cast<int>(halfAt(py)) - 2;
        if (hw > 4) { p.set(cx - hw, py, col(0x1E2226)); p.set(cx + hw, py, col(0x1E2226)); }
    }
    // Vents & hatches scatter.
    for (int k = 0; k < 12; ++k) {
        const int vx = cx - 15 + (k * 37) % 31;
        const int vy = yTop + 18 + (k * 83) % (static_cast<int>(L) - 36);
        if (k % 3 == 0) p.fillRect(vx, vy, 2, 3, col(0x858C93, 200));
        else { p.fillRect(vx, vy, 2, 2, col(0x4E555C)); p.set(vx, vy, col(0x7C838A)); }
    }
    // Rust streaks along the waterline.
    for (int k = 0; k < 12; ++k) {
        const int rx = cx - 20 + (k * 41) % 41;
        const int ry = yTop + 12 + (k * 71) % (static_cast<int>(L) - 24);
        p.set(rx, ry, col(0x6E4A2C, 85));
        p.set(rx, ry + 1, col(0x6E4A2C, 50));
    }

    // --- damage tiers ------------------------------------------------------------------
    auto scorch = [&](int dx, int dy, int r, bool ember) {
        const int yy = H / 2 + dy;
        const int lim = std::max(4, static_cast<int>(halfAt(yy)) - r - 2);
        dx = std::max(-lim, std::min(lim, dx));
        // Near-opaque alphas: low-alpha darks over the sea read blue, not burnt.
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      static_cast<float>(r), static_cast<float>(r), col(0x14100C, 215));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      r * 0.5f, r * 0.5f, col(0x0A0806, 255));
        if (ember)
            p.fillEllipse(static_cast<float>(cx + dx) + 1.0f, static_cast<float>(yy) + 1.0f,
                          r * 0.25f, r * 0.25f, col(0x8A3B12, 200));
    };
    if (damage >= 2) {
        scorch(-14, 46, 6, true);
        scorch(10, -8, 5, false);
        scorch(-8, -62, 6, true);
        scorch(16, 20, 4, false);
    }
    if (damage >= 3) {
        scorch(0, 74, 8, true);
        scorch(-16, -40, 7, true);
        scorch(12, -72, 6, false);
        scorch(6, 40, 7, true);
        // Soot wash down the whole deck.
        for (int y = yTop + 4; y < yBot - 4; ++y) {
            const int hw = static_cast<int>(halfAt(y)) - 4;
            for (int x = -hw; x <= hw; ++x)
                if ((x * 7 + y * 11) % 13 < 5)
                    p.set(cx + x, y, col(0x1E1A16, 120));
        }
    }
    return p;
}

// --- COASTAL FORTRESS boss hull ----------------------------------------------
// A heavily fortified island stronghold rendered with soft alpha-blended
// shading (reef shallows, breaker foam, gradient sand, rocky edge, a dense
// palm/jungle ring, concrete seawall + inner court, radial supply roads,
// gun pads, fuel farm, sandbag arcs and a west-side dock). The pad centres
// MUST mirror the component offsets in bosses.json (mains (0,58) (±62,4);
// secondaries (±46,-46); AA (±24,-70); keep (0,-6)).
// damage: 0 pristine, 2 cracked & burning, 3 wreck.
namespace ffort {
constexpr int W = 224, H = 214;
// Base island shoreline ellipse (the visible coast is this warped by the
// islR() harmonics inside makeFortressHull), seawall and inner court ellipses.
constexpr float RX = 95.0f, RY = 88.0f;
constexpr float WX = 78.0f,  WY = 73.0f;
constexpr float CX = 70.0f,  CY = 63.0f;
}  // namespace ffort

PixelBuf makeFortressHull(int damage) {
    using namespace ffort;
    const int cx = W / 2, cy = H / 2;
    PixelBuf p(W, H);

    // --- palettes -------------------------------------------------------------
    const std::uint32_t sandHi  = col(0xDCC698);
    const std::uint32_t sandMid = col(0xC6AF7F);
    const std::uint32_t sandDk  = col(0xA8916A);
    const std::uint32_t sandWet = col(0x8C7A55);
    const std::uint32_t rockLo  = col(0x6E6759);
    const std::uint32_t rockMid = col(0x867F70);
    const std::uint32_t jgDeep  = col(0x163314);
    const std::uint32_t jgDark  = col(0x24501F);
    const std::uint32_t jgMid   = col(0x3A7A31);
    const std::uint32_t jgHi    = col(0x5C9940);
    const std::uint32_t dirtHi  = col(0x97845F);
    const std::uint32_t dirtLo  = col(0x665741);
    const std::uint32_t concHi  = col(0x9D998B);
    const std::uint32_t concMid = col(0x8B877A);
    const std::uint32_t concLo  = col(0x767265);
    const std::uint32_t concRim = col(0x494539);
    const std::uint32_t concSeam= col(0x6A665A);

    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto hash = [](int x, int y, int s) {
        std::uint32_t h = static_cast<std::uint32_t>(x * 374761393 + y * 668265263 +
                                                     s * 1274126177);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (h ^ (h >> 16)) & 0xFFFFu;
    };
    auto frand = [&](int x, int y, int s) { return static_cast<float>(hash(x, y, s)) / 65535.0f; };
    // Soft-edged disc: near-solid core with a 2-pixel anti-aliased rim,
    // composited with per-pixel alpha. The key to the smoother look.
    auto softDisc = [&](float ex, float ey, float rx, float ry, std::uint32_t c) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(c & 0xFFu) * f);
                    p.blend(x, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else {
                    p.blend(x, y, c);
                }
            }
    };
    // Ellipse radius along a direction, for the man-made rings (wall, court).
    auto qAt = [&](float x, float y, float rx, float ry) {
        const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
        return std::sqrt(dx * dx + dy * dy);
    };

    // --- organic island outline -------------------------------------------------
    // Harmonic warp of the base ellipse: coves, headlands, asymmetric lobes.
    // The silhouette is still deterministic — it is baked sprite art.
    auto islR = [](float ang) {
        return 1.00f + 0.030f * std::sin(ang * 1.0f + 2.10f) +
                           0.090f * std::sin(ang * 3.0f + 1.30f) +
                           0.060f * std::sin(ang * 5.0f + 4.20f) +
                           0.040f * std::sin(ang * 8.0f + 2.50f) +
                           0.022f * std::sin(ang * 13.0f + 0.70f);
    };
    // Normalised distance to the organic shoreline (1.0 == waterline).
    auto shoreQ = [&](float x, float y) {
        const float dx = (x + 0.5f - cx) / RX, dy = (y + 0.5f - cy) / RY;
        const float base = std::sqrt(dx * dx + dy * dy);
        return base / islR(std::atan2(dy, dx));
    };
    auto angOf = [&](float x, float y) {
        return std::atan2((y + 0.5f - cy) / RY, (x + 0.5f - cx) / RX);
    };
    // Rocky shore sectors: exposed stone breaks the sand ring (0..1).
    auto rockAmt = [&](float ang) {
        const float v = std::sin(ang * 3.0f + 0.4f) * std::sin(ang * 2.0f - 1.2f);
        return v > 0.15f ? cl((v - 0.15f) * 2.2f) : 0.0f;
    };
    // Vegetation density mask: dense jungle masses alternating with open,
    // sandy stretches — no uniform green ring.
    auto vegDens = [&](float ang) {
        const float v = 0.5f + 0.5f * std::sin(ang * 2.3f + 0.8f) *
                                  std::sin(ang * 3.7f + 2.9f);
        const float fine = frand(static_cast<int>(ang * 40.0f), 5, 5) * 0.22f;
        return cl(0.24f + 0.76f * v + fine);
    };
    // Gun pads: {x, y, r} — mirrors bosses.json offsets.
    struct Pad { float x, y, r; };
    const Pad pads[] = {{0, 58, 31},   {62, 4, 31},   {-62, 4, 31},
                        {46, -46, 21}, {-46, -46, 21}, {24, -70, 16},
                        {-24, -70, 16}};
    auto nearPad = [&](float x, float y, float extra) {
        for (const Pad& pd : pads) {
            const float dx = x - (cx + pd.x), dy = y - (cy + pd.y);
            if (dx * dx + dy * dy < (pd.r + extra) * (pd.r + extra)) return true;
        }
        // Keep plinth zone.
        const float kx = (x - cx) / 38.0f, ky = (y - (cy - 4.0f)) / 31.0f;
        return kx * kx + ky * ky < 1.0f;
    };
    // Where vegetation may grow: between the seawall and the beach, clear of
    // the emplacements and the dock approach.
    auto inVegBand = [&](float x, float y) {
        if (qAt(x, y, WX + 4.0f, WY + 4.0f) < 1.0f) return false;
        if (shoreQ(x, y) > 0.985f) return false;
        if (nearPad(x, y, 4.0f)) return false;
        if (x < cx - 56.0f && y > cy + 12.0f && y < cy + 46.0f) return false;  // dock
        return true;
    };

    // --- layered drop shadow on the sea, following the organic outline ----------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            const float q = shoreQ(fx, fy);
            if (q <= 1.0f) continue;  // shadow only falls on water
            if (shoreQ(fx - 7.0f, fy - 9.0f) < 1.0f)
                p.blend(x, y, col(0x050F1A, static_cast<int>(72.0f * cl((1.15f - q) / 0.15f))));
            else if (shoreQ(fx - 13.0f, fy - 16.0f) < 1.0f)
                p.blend(x, y, col(0x050F1A, static_cast<int>(40.0f * cl((1.24f - q) / 0.09f))));
        }

    // --- reef shallows hugging the shore, deeper water just offshore ------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = shoreQ(static_cast<float>(x), static_cast<float>(y));
            if (q <= 1.0f || q > 1.34f) continue;
            const float ang = angOf(static_cast<float>(x), static_cast<float>(y));
            if (q < 1.17f) {
                // bright turquoise shallows, broken by darker reef patches
                const float t = (q - 1.0f) / 0.17f;
                const float patchf = 0.5f + 0.5f * std::sin(ang * 6.0f + 2.2f);
                const std::uint32_t c = patchf > 0.60f ? col(0x2E7E90) : col(0x4FA8B8);
                const int a = static_cast<int>(80.0f * (1.0f - t * t) * (0.65f + 0.35f * patchf));
                if (a > 6) p.blend(x, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
            } else {
                const float t = (q - 1.17f) / 0.17f;
                const int a = static_cast<int>(46.0f * (1.0f - t));
                if (a > 6) p.blend(x, y, col(0x0A2440, a));
            }
        }

    // --- sand body: organic outline, wet tideline, rock sectors, speckle --------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = shoreQ(static_cast<float>(x), static_cast<float>(y));
            if (q > 1.0f) continue;
            const float ang = angOf(static_cast<float>(x), static_cast<float>(y));
            std::uint32_t c = mix(sandHi, sandMid, q * 1.05f);
            if (q > 0.72f) c = mix(c, sandDk, (q - 0.72f) / 0.28f * 0.85f);
            if (hash(x, y, 3) % 9 == 0) c = mix(c, col(0x8F7C52), 0.30f);   // grit
            if (hash(x, y, 9) % 23 == 0) c = mix(c, col(0xE4D2A6), 0.45f);  // bright flecks
            // wet-sand band with an undulating tideline
            const float wet0 = 0.845f + 0.05f * std::sin(ang * 6.0f + 2.1f) +
                               0.03f * std::sin(ang * 11.0f + 4.4f);
            if (q > wet0) c = mix(c, sandWet, (q - wet0) / (1.0f - wet0) * 0.62f);
            // rocky shore sectors: exposed stone and pebbles replace the sand
            const float ra = rockAmt(ang);
            if (ra > 0.02f && q > 0.55f) {
                const float m = ra * cl((q - 0.55f) / 0.35f);
                c = mix(c, mix(rockLo, rockMid, frand(x >> 2, y >> 2, 13)), m * 0.85f);
                if (hash(x >> 1, y >> 1, 17) % 11 == 0) c = mix(c, col(0x544E44), 0.5f);
            }
            p.set(x, y, c);
        }
    // Sun-bright beach sheen (light from the top-left).
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (shoreQ(static_cast<float>(x), static_cast<float>(y)) < 0.98f &&
                dx + dy < -30.0f && hash(x, y, 5) % 3 == 0)
                p.blend(x, y, col(0xF0E2BC, 28));
        }

    // --- breaker foam: broken ring following the organic shoreline ---------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = shoreQ(static_cast<float>(x), static_cast<float>(y));
            if (q < 0.995f || q > 1.085f) continue;
            const float ang = angOf(static_cast<float>(x), static_cast<float>(y));
            const float wob = 0.55f + 0.45f * std::sin(ang * 7.0f + 1.7f);
            if (wob < 0.28f) continue;
            const float band = 1.0f - std::fabs((q - 1.0f) / 0.085f - 0.4f) / 0.6f;
            const int a = static_cast<int>(200.0f * band * wob);
            if (a > 12) p.blend(x, y, col(0xE8F2FA, a));
        }
    // thin wash tongues creeping up the beach
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = shoreQ(static_cast<float>(x), static_cast<float>(y));
            if (q < 0.90f || q > 1.0f) continue;
            const float ang = angOf(static_cast<float>(x), static_cast<float>(y));
            const float s = std::sin(ang * 17.0f + 0.6f) * std::sin(ang * 5.0f + 1.9f);
            if (s <= 0.62f) continue;
            const float w = (s - 0.62f) / 0.38f;
            const float edge = 0.995f - (1.0f - w) * 0.06f;
            if (q > edge) p.blend(x, y, col(0xE8F2FA, static_cast<int>(52.0f * w)));
        }
    for (int k = 0; k < 52; ++k) {  // foam sparkles just outside the breakers
        const float ang = static_cast<float>(k) * 0.1364f + 0.31f;
        const float q = (1.05f + frand(k, 17, 4) * 0.05f) * islR(ang);
        const float fx = cx + std::cos(ang) * q * RX;
        const float fy = cy + std::sin(ang) * q * RY;
        softDisc(fx, fy, 1.6f, 1.2f, col(0xFFFFFF, 120));
    }

    // --- rocks along the shoreline (bigger in the rocky sectors) ------------------
    for (int k = 0; k < 34; ++k) {
        const float ang = static_cast<float>(k) * 0.2417f + 0.62f;
        const float q = (0.985f + frand(k, 23, 6) * 0.05f) * islR(ang);
        const float rx = cx + std::cos(ang) * q * RX;
        const float ry = cy + std::sin(ang) * q * RY;
        const float boost = 1.0f + rockAmt(ang) * 1.1f;
        const float s = (2.2f + frand(k, 5, 8) * 2.4f) * boost;
        softDisc(rx + 1.0f, ry + 1.6f, s * 1.05f, s * 0.8f, col(0x0A141C, 90));
        softDisc(rx, ry, s, s * 0.78f, mix(rockLo, rockMid, frand(k, 9, 10)));
        softDisc(rx - s * 0.28f, ry - s * 0.3f, s * 0.5f, s * 0.4f, col(0x837E70));
        if (boost > 1.6f && k % 2 == 0) {  // companion rock in rocky sectors
            softDisc(rx + s * 1.3f, ry + s * 0.5f, s * 0.6f, s * 0.45f, mix(rockLo, rockMid, 0.4f));
            softDisc(rx + s * 1.15f, ry + s * 0.35f, s * 0.3f, s * 0.22f, col(0x837E70));
        }
    }

    // small rock outcrops scattered on the sand between wall and shore
    for (int k = 0; k < 7; ++k) {
        const float ang = k * 0.898f + 2.3f;
        const float qq = (0.88f + frand(k, 51, 1) * 0.08f) * islR(ang);
        const float bx = cx + std::cos(ang) * qq * RX, by = cy + std::sin(ang) * qq * RY;
        if (nearPad(bx, by, 2.0f)) continue;
        for (int j = 0; j < 3; ++j) {
            const float rx = bx + (frand(k, j, 52) - 0.5f) * 10.0f;
            const float ry = by + (frand(k, j, 53) - 0.5f) * 8.0f;
            const float s = 1.8f + frand(k, j, 54) * 2.2f;
            softDisc(rx + 1.0f, ry + 1.4f, s * 1.05f, s * 0.8f, col(0x0A141C, 70));
            softDisc(rx, ry, s, s * 0.75f, mix(rockLo, rockMid, frand(k, j, 55)));
            softDisc(rx - s * 0.25f, ry - s * 0.28f, s * 0.4f, s * 0.3f, col(0x837E70));
        }
    }

    // --- jungle floor: dark under-canopy, density-masked by sector ----------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!inVegBand(static_cast<float>(x), static_cast<float>(y))) continue;
            const float ang = angOf(static_cast<float>(x), static_cast<float>(y));
            const float d = vegDens(ang);
            if (d <= 0.40f) continue;
            const std::uint32_t c = mix(col(0x234A1F), col(0x1B3A18), frand(x, y, 11));
            const int a = static_cast<int>(190.0f * cl((d - 0.40f) / 0.5f));
            p.blend(x, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
        }

    // --- vegetation: four plant types, clumped + scattered --------------------------
    auto drawPalm = [&](float tx, float ty, float s, int k) {
        softDisc(tx + s * 0.55f, ty + s * 0.60f, s * 1.05f, s * 0.62f, col(0x08130A, 80));
        softDisc(tx + s * 0.18f, ty + s * 0.34f, s * 0.13f, s * 0.36f, col(0x6B4E2E, 210));
        for (int f = 0; f < 8; ++f) {  // tapered radiating fronds
            const float a = f * 0.7854f + frand(k, f, 14) * 0.42f;
            const std::uint32_t fc = (f % 2) ? mix(col(0x254D1F), jgMid, frand(f, k, 15))
                                             : mix(jgMid, jgHi, frand(f, k, 16) * 0.7f);
            for (int d = 0; d < 3; ++d) {
                const float rr = s * (0.42f + 0.30f * d);
                softDisc(tx + std::cos(a) * rr, ty + std::sin(a) * rr * 0.82f,
                         s * (0.48f - 0.11f * d), s * (0.35f - 0.08f * d), fc);
            }
        }
        softDisc(tx, ty, s * 0.42f, s * 0.34f, mix(jgDark, jgMid, 0.5f));
        softDisc(tx - s * 0.20f, ty - s * 0.20f, s * 0.20f, s * 0.15f, jgHi);
        softDisc(tx + s * 0.08f, ty + s * 0.10f, s * 0.10f, s * 0.09f, col(0x6B4E2E));
        softDisc(tx - s * 0.10f, ty + s * 0.12f, s * 0.08f, s * 0.08f, col(0x5A4128));
    };
    auto drawBroad = [&](float tx, float ty, float s, int k) {
        softDisc(tx + s * 0.5f, ty + s * 0.58f, s * 1.0f, s * 0.55f, col(0x08130A, 78));
        for (int b = 0; b < 5; ++b) {  // irregular dark canopy outline
            const float a = b * 1.2566f + frand(k, b, 21);
            softDisc(tx + std::cos(a) * s * 0.42f, ty + std::sin(a) * s * 0.36f,
                     s * (0.52f + frand(k, b, 22) * 0.16f),
                     s * (0.44f + frand(k, b, 23) * 0.12f), jgDeep);
        }
        softDisc(tx, ty, s * 0.80f, s * 0.64f, jgDark);
        for (int b = 0; b < 4; ++b) {  // mid-tone leaf lobes
            const float a = b * 1.5708f + 0.5f + frand(k, b, 24) * 0.5f;
            softDisc(tx + std::cos(a) * s * 0.30f - s * 0.06f,
                     ty + std::sin(a) * s * 0.24f - s * 0.06f, s * 0.42f, s * 0.34f,
                     mix(jgDark, jgMid, 0.35f + frand(b, k, 25) * 0.4f));
        }
        softDisc(tx - s * 0.26f, ty - s * 0.28f, s * 0.34f, s * 0.26f, mix(jgMid, jgHi, 0.55f));
        softDisc(tx - s * 0.34f, ty - s * 0.34f, s * 0.16f, s * 0.12f, jgHi);
    };
    auto drawClump = [&](float tx, float ty, float s, int k) {
        softDisc(tx + s * 0.45f, ty + s * 0.5f, s * 1.05f, s * 0.55f, col(0x08130A, 70));
        const int n = 6 + static_cast<int>(frand(k, 1, 26) * 4.0f);
        for (int b = 0; b < n; ++b) {  // merged canopy mass
            const float a = frand(k, b, 27) * 6.2832f;
            const float rr = frand(k, b, 28) * s * 0.62f;
            const std::uint32_t c = (b % 3 == 0) ? jgDeep
                                                 : mix(jgDark, jgMid, frand(k, b, 29));
            softDisc(tx + std::cos(a) * rr, ty + std::sin(a) * rr * 0.8f,
                     s * (0.40f + frand(k, b, 30) * 0.30f),
                     s * (0.32f + frand(k, b, 31) * 0.22f), c);
        }
        for (int b = 0; b < 4; ++b) {  // sunlit crown dots
            const float a = frand(k, b, 32) * 6.2832f;
            softDisc(tx + std::cos(a) * s * 0.4f - s * 0.1f,
                     ty + std::sin(a) * s * 0.3f - s * 0.12f, s * 0.16f, s * 0.12f,
                     mix(jgMid, jgHi, 0.6f));
        }
    };
    auto drawBush = [&](float tx, float ty, float s, int k) {
        softDisc(tx + s * 0.35f, ty + s * 0.45f, s * 0.85f, s * 0.5f, col(0x08130A, 60));
        softDisc(tx, ty, s * 0.72f, s * 0.56f, col(0x21451D));
        softDisc(tx - s * 0.18f, ty - s * 0.22f, s * 0.44f, s * 0.34f, jgDark);
        softDisc(tx - s * 0.30f, ty - s * 0.30f, s * 0.20f, s * 0.15f, mix(jgMid, jgHi, 0.4f));
        if (frand(k, 9, 33) < 0.30f)  // occasional flowering speck
            softDisc(tx + s * 0.10f, ty - s * 0.10f, s * 0.09f, s * 0.09f, col(0xC9A0C8, 160));
    };
    auto plantTree = [&](float tx, float ty, float s, int k) {
        const float r = frand(k, 13, 12);
        if (r < 0.38f) drawPalm(tx, ty, s, k);
        else if (r < 0.64f) drawBroad(tx, ty, s, k);
        else if (r < 0.82f) drawBush(tx, ty, s * 0.85f, k);
        else drawClump(tx, ty, s * 1.45f, k);
    };
    // irregular jungle masses
    for (int ci = 0; ci < 12; ++ci) {
        const float ca = ci * 0.5236f + frand(ci, 3, 1) * 0.55f;
        const float cq = (0.80f + frand(ci, 7, 2) * 0.15f) * islR(ca);
        const float ccx = cx + std::cos(ca) * cq * RX;
        const float ccy = cy + std::sin(ca) * cq * RY;
        const int n = 3 + static_cast<int>(frand(ci, 11, 2) * 5.0f);
        for (int m = 0; m < n; ++m) {
            const float tx = ccx + (frand(ci * 31 + m, 5, 3) - 0.5f) * 22.0f;
            const float ty = ccy + (frand(ci * 31 + m, 9, 4) - 0.5f) * 18.0f;
            if (!inVegBand(tx, ty)) continue;
            plantTree(tx, ty, 3.6f + frand(ci * 31 + m, 3, 2) * 3.2f, ci * 31 + m);
        }
    }
    // scattered singles, gated by the sector density mask
    for (int k = 0; k < 110; ++k) {
        const float ang = k * 2.39996f + 0.4f;
        const float qq = (0.76f + 0.22f * frand(k, 7, 1)) * islR(ang);
        const float tx = cx + std::cos(ang) * qq * RX;
        const float ty = cy + std::sin(ang) * qq * RY;
        if (!inVegBand(tx, ty)) continue;
        if (frand(k, 3, 6) > vegDens(ang)) continue;
        plantTree(tx, ty, 3.2f + frand(k, 3, 2) * 3.4f, 200 + k);
    }
    // coastal scrub right at the vegetation/beach boundary
    for (int k = 0; k < 28; ++k) {
        const float ang = k * 1.91f + 1.1f;
        const float qq = (0.925f + frand(k, 33, 1) * 0.05f) * islR(ang);
        const float tx = cx + std::cos(ang) * qq * RX;
        const float ty = cy + std::sin(ang) * qq * RY;
        if (!inVegBand(tx, ty)) continue;
        drawBush(tx, ty, 1.8f + frand(k, 34, 2) * 1.2f, 400 + k);
    }

    // --- concrete seawall: gradient body, dark rim, sun highlight ------------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = qAt(static_cast<float>(x), static_cast<float>(y), WX, WY);
            if (q > 1.0f) continue;
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            std::uint32_t c = mix(concMid, concLo, q);
            if (q > 0.88f) c = mix(c, concRim, (q - 0.88f) / 0.12f);
            else if ((x - y) % 19 == 0 || (x + y) % 23 == 0) c = mix(c, concSeam, 0.55f);
            if (dx + dy < -18.0f) c = mix(c, concHi, 0.35f);   // sun side
            if (dx + dy > 26.0f) c = mix(c, col(0x5E5A4E), 0.4f);  // shade side
            p.set(x, y, c);
        }
    // Wall outer shadow cast onto jungle/sand (bottom-right).
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q1 = qAt(static_cast<float>(x), static_cast<float>(y), WX + 3, WY + 3);
            const float q2 = qAt(static_cast<float>(x), static_cast<float>(y), WX + 7, WY + 6);
            if (q1 >= 1.0f || q2 <= 1.0f) continue;
            const float dx = x + 0.5f - cx;
            if (dx > -20.0f) p.blend(x, y, col(0x1A160F, 90));
        }
    // worn dirt and moss creeping across the rampart walk
    for (int k = 0; k < 8; ++k) {
        const float a = k * 0.7854f + 0.3f;
        const float rr = 0.80f + frand(k, 61, 1) * 0.14f;
        const float bx = cx + std::cos(a) * rr * WX, by = cy + std::sin(a) * rr * WY;
        softDisc(bx, by, 5.0f + frand(k, 62, 2) * 4.0f, 4.0f + frand(k, 62, 3) * 3.0f,
                 col(0x776B50, 130));
    }

    // --- inner court: packed dirt with grass intrusions -----------------------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q = qAt(static_cast<float>(x), static_cast<float>(y), CX, CY);
            if (q > 1.0f) continue;
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            std::uint32_t c = mix(dirtHi, dirtLo, q * 0.9f);
            if (hash(x, y, 23) % 9 == 0) c = mix(c, col(0x5C4D38), 0.35f);   // grit
            if (hash(x, y, 24) % 19 == 0) c = mix(c, col(0xA8946C), 0.40f);  // pale flecks
            if (frand(x >> 1, y >> 1, 37) > 0.955f) c = mix(c, col(0x5E6B3A), 0.55f);  // tufts
            if (dx + dy < -14.0f) c = mix(c, col(0xB09B72), 0.22f);  // sun side
            if (dx + dy > 22.0f) c = mix(c, col(0x54472F), 0.24f);   // shade side
            p.set(x, y, c);
        }
    // contact shadow where the court meets the wall base (upper-left sun)
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float q1 = qAt(static_cast<float>(x), static_cast<float>(y), CX, CY);
            const float q2 = qAt(static_cast<float>(x), static_cast<float>(y), CX - 3.0f, CY - 3.0f);
            if (q1 >= 1.0f || q2 <= 1.0f) continue;
            if (x + 0.5f - cx + (y + 0.5f - cy) < 30.0f)
                p.blend(x, y, col(0x2A2216, 80));
        }
    // grass patches intruding from the edges
    for (int k = 0; k < 12; ++k) {
        const float a = k * 0.5236f + frand(k, 71, 1) * 0.4f;
        const float rr = 0.82f + frand(k, 72, 2) * 0.2f;
        const float bx = cx + std::cos(a) * rr * CX, by = cy + std::sin(a) * rr * CY;
        softDisc(bx, by, 6.0f + frand(k, 73, 3) * 5.0f, 5.0f + frand(k, 73, 4) * 4.0f,
                 col(0x50683A, 150));
        softDisc(bx - 1.0f, by - 1.0f, 3.0f, 2.4f, col(0x5F7A44, 140));
    }
    // drainage gullies radiating from the keep to the wall base
    for (int d4 = 0; d4 < 4; ++d4) {
        const float a = 0.7854f + static_cast<float>(d4) * 1.5708f;
        for (float d = 40.0f; d < 60.0f; d += 1.0f) {
            const float gx = cx + std::cos(a) * d * 0.98f, gy = cy + std::sin(a) * d * 0.92f;
            p.blend(static_cast<int>(gx), static_cast<int>(gy), col(0x3F382C, 200));
            p.blend(static_cast<int>(gx) + 1, static_cast<int>(gy) + 1, col(0xA49B84, 90));
        }
    }

    // --- worn concrete service roads from the keep to every gun pad -----------------
    const float kx = static_cast<float>(cx), ky = static_cast<float>(cy - 6);
    auto road = [&](float x0, float y0, float x1, float y1) {
        const float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
        const int steps = static_cast<int>(len);
        const float nx = -(y1 - y0) / len, ny = (x1 - x0) / len;
        for (int i = 0; i < steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float rx = x0 + (x1 - x0) * t, ry = y0 + (y1 - y0) * t;
            const float rw = 6.8f - 2.2f * std::fabs(t - 0.5f);
            softDisc(rx + nx * rw, ry + ny * rw, 1.7f, 1.7f, col(0x55503F, 70));  // worn edge
            softDisc(rx - nx * rw, ry - ny * rw, 1.7f, 1.7f, col(0x55503F, 70));
            softDisc(rx, ry, rw, rw, col(0x94908A, 145));
            if (i % 6 == 0)
                softDisc(rx + (frand(i, static_cast<int>(x1), 41) - 0.5f) * rw,
                         ry + (frand(i, static_cast<int>(y1), 42) - 0.5f) * rw,
                         1.1f, 1.1f, col(0x6E6A5D, 110));  // spalling
        }
    };
    for (const Pad& pd : pads) road(kx, ky, cx + pd.x, cy + pd.y);
    road(kx, ky, static_cast<float>(cx) - 56.0f, static_cast<float>(cy) + 27.0f);  // dock spur

    // --- gun pads: rim, deck, bolt ring (mirrors the JSON offsets) -------------------
    for (const Pad& pd : pads) {
        const float px = cx + pd.x, py = cy + pd.y;
        softDisc(px + 2.0f, py + 3.0f, pd.r + 2.0f, pd.r + 2.0f, col(0x1A160F, 80));
        softDisc(px, py, pd.r, pd.r, col(0x7A7669));
        softDisc(px, py, pd.r - 4.0f, pd.r - 4.0f, col(0x8A867A));
        const int bolts = pd.r > 25 ? 14 : 10;
        for (int b = 0; b < bolts; ++b) {
            const float a = static_cast<float>(b) * 6.2832f / static_cast<float>(bolts);
            p.set(static_cast<int>(px + std::cos(a) * (pd.r - 2.0f)),
                  static_cast<int>(py + std::sin(a) * (pd.r - 2.0f)), col(0x59554A));
        }
    }

    // --- keep plinth: raised central platform with steps ------------------------------
    softDisc(kx + 2.0f, ky + 4.0f, 36.0f, 30.0f, col(0x1A160F, 90));
    softDisc(kx, ky, 35.0f, 29.0f, concRim);
    softDisc(kx, ky - 1.0f, 33.0f, 27.0f, concMid);
    softDisc(kx - 6.0f, ky - 8.0f, 20.0f, 14.0f, col(0xA39F92, 120));
    for (int s = 0; s < 3; ++s)  // front steps
        p.fillRect(static_cast<int>(kx - 10), static_cast<int>(ky + 24) + s, 20, 1,
                   s == 0 ? col(0xA8A496) : concSeam);

    // --- fuel farm (two camo'd tanks on each shoulder of the court) --------------------
    for (int sx : {-1, 1}) {
        const float fx = cx + sx * 68.0f, fy = cy - 30.0f;
        softDisc(fx + 2.0f, fy + 2.5f, 8.0f, 7.0f, col(0x1A160F, 80));
        softDisc(fx, fy, 7.5f, 6.5f, col(0x6F7A5E));
        softDisc(fx - 1.5f, fy - 1.5f, 5.0f, 4.0f, col(0x87927A));
        softDisc(fx + 2.0f, fy + 2.0f, 3.0f, 2.2f, col(0x4C5A3E, 150));  // camo patch
        p.vline(static_cast<int>(fx), static_cast<int>(fy - 6.0f), static_cast<int>(fy + 6.0f),
                col(0x55604A));
    }

    // --- sandbag arcs facing the player around the three main emplacements -------------
    for (int pi = 0; pi < 3; ++pi) {
        const Pad& pd = pads[pi];
        const float px = cx + pd.x, py = cy + pd.y;
        for (int b = 0; b < 11; ++b) {
            const float a = 0.32f + static_cast<float>(b) * 0.24f;  // lower arc
            const float bx = px + std::cos(a) * (pd.r + 4.0f);
            const float by = py + std::sin(a) * (pd.r + 4.0f);
            softDisc(bx, by, 2.4f, 1.9f, col(0x9A8A66));
            softDisc(bx - 0.6f, by - 0.6f, 1.2f, 0.9f, col(0xB5A57E));
        }
    }

    // --- supply depot beside the dock gate ---------------------------------------------
    auto hut = [&](int hx, int hy) {
        p.fillRect(hx + 1, hy + 2, 13, 8, col(0x0A141C, 80));  // cast shadow
        p.fillRect(hx, hy, 13, 8, col(0x5E6B4A));               // camo body
        p.fillRect(hx, hy, 13, 3, col(0x6E7B58));               // sunlit roof
        p.hline(hx, hx + 12, hy + 7, col(0x36402C));
        p.fillRect(hx + 2, hy + 3, 3, 2, col(0x4A5539));        // camo splotches
        p.fillRect(hx + 8, hy + 4, 3, 2, col(0x4A5539));
    };
    hut(cx - 52, cy + 36);
    hut(cx - 52, cy + 48);

    // --- west dock: timber pier, pilings, T-head, supply cache, barge -------------------
    {
        const int py0 = cy + 19, py1 = cy + 35;
        for (int x = cx - 111; x <= cx - 58; ++x) {  // deck shadow
            p.set(x, py1 + 1, col(0x0A141C, 90));
            p.set(x, py1 + 2, col(0x0A141C, 50));
        }
        for (int x = cx - 111; x <= cx - 58; ++x) {  // plank deck
            const int seam = ((x - (cx - 111)) % 4 == 0) ? 1 : 0;
            for (int y = py0; y <= py1; ++y) {
                std::uint32_t c = (y == py0 || y == py1) ? col(0x4A3B26)
                                             : (seam ? col(0x54432B) : col(0x6B5636));
                if (x > cx - 76 && (y + x) % 6 == 0) c = mix(c, sandMid, 0.5f);  // onto beach
                p.set(x, y, c);
            }
        }
        // T-head platform at the outer end
        for (int x = cx - 113; x <= cx - 103; ++x)
            for (int y = cy + 13; y <= cy + 41; ++y) {
                const std::uint32_t c = ((y - (cy + 13)) % 4 == 0) ? col(0x54432B) : col(0x665234);
                p.set(x, y, (y == cy + 13 || y == cy + 41) ? col(0x4A3B26) : c);
            }
        for (int y = cy + 13; y <= cy + 41; ++y) p.set(cx - 103, y, col(0x463722));
        for (int x = cx - 110; x <= cx - 60; x += 10) {  // pilings
            softDisc(static_cast<float>(x), static_cast<float>(py0 - 1), 1.6f, 1.4f, col(0x2E2415));
            softDisc(static_cast<float>(x), static_cast<float>(py1 + 1), 1.6f, 1.4f, col(0x2E2415));
        }
        for (int y = cy + 15; y <= cy + 39; y += 8)  // T-head pilings
            softDisc(static_cast<float>(cx - 112), static_cast<float>(y), 1.5f, 1.4f, col(0x2E2415));
        softDisc(cx - 110.0f, cy + 27.0f, 2.0f, 2.0f, col(0x33383D));  // bollard
        softDisc(cx - 108.0f, cy + 16.0f, 1.6f, 1.6f, col(0x33383D));
        softDisc(cx - 108.0f, cy + 38.0f, 1.6f, 1.6f, col(0x33383D));
        // supply cache on the beach beside the pier root
        auto crate = [&](float bx2, float by2, float s2) {
            const int x0 = static_cast<int>(bx2 - s2), y0 = static_cast<int>(by2 - s2);
            const int w2 = static_cast<int>(s2 * 2.0f), h2 = static_cast<int>(s2 * 2.0f);
            p.fillRect(x0 + 1, y0 + 1, w2, h2, col(0x0A141C, 70));
            p.fillRect(x0, y0, w2, h2, col(0x7A5F38));
            p.hline(x0, x0 + w2, y0, col(0x97764A));
            p.hline(x0, x0 + w2, y0 + h2, col(0x46361F));
            p.vline(x0 + w2 / 2, y0, y0 + h2, col(0x5C4830));
        };
        const float bx0 = static_cast<float>(cx) - 70.0f, by0 = static_cast<float>(cy) + 50.0f;
        crate(bx0, by0, 3.2f);
        crate(bx0 + 7.0f, by0 + 1.0f, 2.6f);
        crate(bx0 + 3.0f, by0 - 5.0f, 2.4f);
        for (int d = 0; d < 3; ++d) {  // red fuel drums
            const float dx2 = bx0 + 12.5f, dy2 = by0 - 7.0f + static_cast<float>(d) * 5.0f;
            softDisc(dx2 + 1.0f, dy2 + 1.2f, 2.6f, 2.2f, col(0x0A141C, 70));
            softDisc(dx2, dy2, 2.4f, 2.0f, col(0x8A4030));
            softDisc(dx2 - 0.6f, dy2 - 0.6f, 1.1f, 0.9f, col(0xB05A44));
        }
        // Moored barge just off the pier end.
        const float bx = cx - 95.0f, by = cy + 52.0f;
        softDisc(bx + 1.5f, by + 2.5f, 12.0f, 6.0f, col(0x0A141C, 80));
        p.fillRect(static_cast<int>(bx - 11), static_cast<int>(by - 5), 22, 10, col(0x5A4830));
        p.hline(static_cast<int>(bx - 11), static_cast<int>(bx + 10), static_cast<int>(by - 5),
                col(0x7C6442));
        p.hline(static_cast<int>(bx - 11), static_cast<int>(bx + 10), static_cast<int>(by + 4),
                col(0x3A2E1E));
        p.fillRect(static_cast<int>(bx - 3), static_cast<int>(by - 3), 6, 6, col(0x46523A));
        softDisc(bx - 13.0f, by, 1.6f, 3.2f, col(0xDDEAF4, 120));  // wake hint
    }

    // --- battle damage --------------------------------------------------------------
    auto scorchBlob = [&](float sx, float sy, float r, bool ember) {
        softDisc(sx, sy, r, r * 0.86f, col(0x241F18, 175));
        softDisc(sx, sy, r * 0.5f, r * 0.44f, col(0x0E0B08, 220));
        if (ember)
            softDisc(sx + r * 0.15f, sy + r * 0.15f, r * 0.22f, r * 0.2f, col(0x8A3B12, 200));
    };
    if (damage >= 2) {
        scorchBlob(cx + 24.0f, cy + 34.0f, 10.0f, true);
        scorchBlob(cx - 40.0f, cy - 6.0f, 8.0f, false);
        scorchBlob(cx + 8.0f, cy - 40.0f, 7.0f, true);
        // Burned-out jungle patch on the north-east shore.
        softDisc(cx + 56.0f, cy - 62.0f, 14.0f, 10.0f, col(0x3A3128, 170));
        softDisc(cx + 56.0f, cy - 62.0f, 8.0f, 5.5f, col(0x211C15, 200));
        // Cracks across the court.
        for (int k = 0; k < 26; ++k) {
            const float a = k * 0.24f + 0.5f;
            for (int d = 8; d < 8 + (k % 5) * 5; d += 2)
                p.blend(static_cast<int>(cx + 24.0f + d * std::cos(a)),
                        static_cast<int>(cy + 34.0f + d * 0.7f * std::sin(a)),
                        col(0x35322B, 200));
        }
    }
    if (damage >= 3) {
        scorchBlob(cx - 14.0f, cy + 12.0f, 11.0f, true);
        scorchBlob(cx + 44.0f, cy + 18.0f, 8.0f, true);
        scorchBlob(cx - 52.0f, cy - 44.0f, 9.0f, false);
        scorchBlob(cx + 14.0f, cy - 70.0f, 7.0f, true);
        // Soot wash over the whole fortification.
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (qAt(static_cast<float>(x), static_cast<float>(y), WX + 8, WY + 8) > 1.0f)
                    continue;
                if ((x * 7 + y * 11) % 13 < 4) p.blend(x, y, col(0x171614, 95));
            }
        // Breached seawall section (south-west): rubble and a gap to the sand.
        softDisc(cx - 46.0f, cy + 60.0f, 12.0f, 9.0f, col(0xB49E74, 210));
        for (int k = 0; k < 8; ++k) {
            const float rx = cx - 52.0f + frand(k, 31, 7) * 14.0f;
            const float ry = cy + 54.0f + frand(k, 41, 9) * 12.0f;
            softDisc(rx, ry, 2.0f + frand(k, 7, 3) * 1.8f, 1.6f, col(0x6E6A5E));
        }
        // Dead jungle: brown haze over wherever vegetation grew.
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!inVegBand(static_cast<float>(x), static_cast<float>(y))) continue;
                if ((x * 5 + y * 3) % 11 < 6) p.blend(x, y, col(0x3A362A, 130));
            }
    }
    return p;
}

// --- fortress-exclusive component sprites --------------------------------------
// Kept separate from the shared boss_big_turret / boss_aa / boss_bridge so the
// fortress guns can grow without touching the battleship/carrier bosses.

PixelBuf makeFortMain() {
    constexpr int S = 64;
    const int c = S / 2;
    PixelBuf p(S, S);
    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    auto soft = [&](float ex, float ey, float rx, float ry, std::uint32_t colr) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(colr & 0xFFu) * f);
                    p.blend(x, y, (colr & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else p.blend(x, y, colr);
            }
    };
    // Drop shadow + concrete barbette ring.
    soft(c + 3.0f, c + 4.0f, 27.0f, 26.0f, col(0x0A0806, 90));
    soft(static_cast<float>(c), static_cast<float>(c), 26.0f, 25.0f, col(0x565B52));
    soft(static_cast<float>(c), static_cast<float>(c), 23.0f, 22.0f, col(0x454A42));
    for (int b = 0; b < 16; ++b) {  // barbette bolts
        const float a = b * 0.3927f;
        p.set(c + static_cast<int>(24.5f * std::cos(a)), c + static_cast<int>(23.5f * std::sin(a)),
              col(0x6E756C));
    }
    // Twin barrels (drawn before the housing so the mantel covers their roots).
    for (int bx : {c - 5, c + 5}) {
        p.fillRect(bx - 2, c + 2, 4, 26, col(0x23272A));
        p.vline(bx - 1, c + 3, c + 26, col(0x4A4F53));
        p.fillRect(bx - 3, c + 22, 6, 6, col(0x2C3033));  // muzzle brake
        p.vline(bx - 2, c + 23, c + 27, col(0x565C61));
        p.set(bx, c + 27, col(0x0A0C0E));                 // bore
    }
    // Octagonal armoured housing: vertical gradient body + bevelled rim.
    for (int y = c - 20; y <= c + 10; ++y) {
        const float t = static_cast<float>(y - (c - 20)) / 30.0f;
        const bool taper = (t < 0.14f || t > 0.82f);
        const int cut = taper ? 7 : 1;
        for (int x = c - 19 + cut; x <= c + 18 - cut; ++x) {
            const bool edge = x <= c - 19 + cut || x >= c + 18 - cut ||
                              y <= c - 20 || y >= c + 10;
            std::uint32_t colr = mix(col(0x727970), col(0x464B44), t);
            if (edge) colr = mix(colr, col(0x2C302B), 0.75f);
            else if (x < c - 4 && y < c - 6) colr = mix(colr, col(0x8A918A), 0.45f);  // sun face
            p.set(x, y, colr);
        }
    }
    // Faceplate (front slope) + mantel band over the barrels.
    p.fillRect(c - 12, c + 4, 24, 7, col(0x565C54));
    p.hline(c - 12, c + 11, c + 4, col(0x7C837B));
    p.hline(c - 12, c + 11, c + 10, col(0x333833));
    p.fillRect(c - 7, c + 6, 14, 4, col(0x2C3033));
    // Roof details: hatch, vision slit, sight post.
    soft(static_cast<float>(c), static_cast<float>(c - 12), 5.0f, 4.0f, col(0x3A3F3A));
    soft(static_cast<float>(c), static_cast<float>(c - 13), 3.2f, 2.4f, col(0x565C61));
    p.fillRect(c - 6, c - 19, 12, 2, col(0x2E322E));
    p.set(c, c - 21, col(0x8A9094));
    return p;
}

PixelBuf makeFortSecondary() {
    constexpr int S = 36;
    const int c = S / 2;
    PixelBuf p(S, S);
    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    auto soft = [&](float ex, float ey, float rx, float ry, std::uint32_t colr) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(colr & 0xFFu) * f);
                    p.blend(x, y, (colr & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else p.blend(x, y, colr);
            }
    };
    soft(c + 2.0f, c + 3.0f, 15.0f, 14.0f, col(0x0A0806, 80));
    soft(static_cast<float>(c), static_cast<float>(c), 14.0f, 13.0f, col(0x51564E));
    soft(static_cast<float>(c), static_cast<float>(c), 11.5f, 10.5f, col(0x43483F));
    // Twin thin DP barrels.
    for (int bx : {c - 2, c + 3}) {
        p.fillRect(bx - 1, c, 2, 13, col(0x23272A));
        p.set(bx, c + 12, col(0x0A0C0E));
    }
    // Small octagonal housing with gradient.
    for (int y = c - 9; y <= c + 5; ++y) {
        const float t = static_cast<float>(y - (c - 9)) / 14.0f;
        const int cut = (y <= c - 8 || y >= c + 4) ? 4 : 2;
        for (int x = c - 8 + cut; x <= c + 7 - cut; ++x) {
            const bool edge = x <= c - 8 + cut || x >= c + 7 - cut || y <= c - 9 || y >= c + 5;
            std::uint32_t colr = mix(col(0x6B726A), col(0x454A43), t);
            if (edge) colr = mix(colr, col(0x2C302B), 0.7f);
            p.set(x, y, colr);
        }
    }
    p.fillRect(c - 4, c + 1, 9, 3, col(0x2C3033));  // mantel
    soft(static_cast<float>(c), static_cast<float>(c - 5), 2.6f, 2.0f, col(0x565C61));  // sight
    return p;
}

PixelBuf makeFortAa() {
    constexpr int S = 32;
    const int c = S / 2;
    PixelBuf p(S, S);
    auto soft = [&](float ex, float ey, float rx, float ry, std::uint32_t colr) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(colr & 0xFFu) * f);
                    p.blend(x, y, (colr & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else p.blend(x, y, colr);
            }
    };
    soft(c + 2.0f, c + 3.0f, 13.0f, 12.0f, col(0x0A0806, 75));
    soft(static_cast<float>(c), static_cast<float>(c), 12.0f, 11.0f, col(0x4E534B));
    soft(static_cast<float>(c), static_cast<float>(c), 9.5f, 8.5f, col(0x5A6058));
    // Triple light AA barrels.
    for (int bx : {c - 4, c, c + 4}) {
        p.fillRect(bx - 1, c - 2, 1, 13, col(0x8A9094));
        p.set(bx, c + 10, col(0x14181C));
    }
    p.fillRect(c - 6, c - 3, 12, 4, col(0x3A4038));  // mount block
    p.hline(c - 6, c + 5, c - 3, col(0x6E757C));
    // Gunner shield arc + seat.
    soft(static_cast<float>(c), static_cast<float>(c - 6), 6.5f, 3.5f, col(0x63685F));
    soft(static_cast<float>(c), static_cast<float>(c - 6), 4.5f, 2.2f, col(0x2E322E));
    soft(static_cast<float>(c), static_cast<float>(c + 4), 2.4f, 2.0f, col(0x3A3F3A));
    return p;
}

PixelBuf makeFortKeep() {
    constexpr int W = 56, H = 64;
    const int cx = W / 2;
    PixelBuf p(W, H);
    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    auto soft = [&](float ex, float ey, float rx, float ry, std::uint32_t colr) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(colr & 0xFFu) * f);
                    p.blend(x, y, (colr & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else p.blend(x, y, colr);
            }
    };
    // Big soft drop shadow — the keep is the tallest structure on the island.
    soft(cx + 4.0f, 38.0f, 29.0f, 30.0f, col(0x0A0806, 95));
    auto tier = [&](int x0, int y0, int tw, int th, std::uint32_t hi, std::uint32_t lo) {
        for (int y = y0; y < y0 + th; ++y) {
            const float t = static_cast<float>(y - y0) / static_cast<float>(th > 1 ? th : 1);
            for (int x = x0; x < x0 + tw; ++x) {
                std::uint32_t c = mix(hi, lo, t);
                if (x == x0 || x == x0 + tw - 1 || y == y0 || y == y0 + th - 1)
                    c = mix(c, col(0x2A2E28), 0.72f);  // dark bevel rim
                p.set(x, y, c);
            }
        }
        p.hline(x0 + 1, x0 + tw - 2, y0, col(0xB5B1A3, 200));  // sunlit top edge
    };
    // Tier 1: fortified base with windows and blast door.
    tier(cx - 24, 26, 48, 34, col(0x8F8B7E), col(0x66625A));
    for (int i = 0; i < 6; ++i) {  // windows
        const int wx = cx - 19 + i * 8;
        p.fillRect(wx, 34, 3, 5, col(0x1E2226));
        p.set(wx, 34, col(0x6E7A84));
        p.fillRect(wx, 44, 3, 5, col(0x1E2226));
        p.set(wx, 44, col(0x6E7A84));
    }
    p.fillRect(cx - 5, 52, 10, 8, col(0x333833));  // blast door
    p.hline(cx - 5, cx + 4, 52, col(0x7C837B));
    p.vline(cx, 53, 59, col(0x22261F));
    // Sandbag stacks flanking the door.
    for (int s = 0; s < 3; ++s) {
        soft(cx - 11.0f + s * 0.5f, 58.0f - s * 3.0f, 3.2f, 2.2f, col(0x9A8A66));
        soft(cx + 11.0f - s * 0.5f, 58.0f - s * 3.0f, 3.2f, 2.2f, col(0x9A8A66));
    }
    // Tier 2: command deck with camo netting on one corner.
    tier(cx - 19, 12, 38, 16, col(0x6E756A), col(0x4B5049));
    for (int i = 0; i < 4; ++i) {
        const int wx = cx - 13 + i * 8;
        p.fillRect(wx, 18, 4, 4, col(0x1E2226));
        p.set(wx, 18, col(0x6E7A84));
    }
    for (int k = 0; k < 10; ++k)  // camo net blobs
        soft(cx - 17.0f + (k % 3) * 3.4f, 13.0f + (k / 3) * 2.6f, 2.6f, 1.8f, col(0x3D5A30, 190));
    // Tier 3: fire direction with rotating hood.
    tier(cx - 12, 4, 24, 10, col(0x84897C), col(0x5C6158));
    p.fillRect(cx - 7, 7, 14, 3, col(0x22261F));  // slit windows
    soft(static_cast<float>(cx), 5.0f, 5.0f, 3.6f, col(0x3E4238));  // director dome
    soft(static_cast<float>(cx) - 1.0f, 4.0f, 2.6f, 1.8f, col(0x6E757C));
    // Mast + flag.
    p.vline(cx, 0, 4, col(0x565C61));
    p.set(cx - 3, 1, col(0x565C61));
    p.set(cx + 3, 1, col(0x565C61));
    p.fillRect(cx + 1, 0, 5, 3, col(0xC0392B));
    return p;
}

PixelBuf makeDestroyerHull(int damage) {
    using namespace dship;
    const int cx = W / 2;
    PixelBuf p(W, H);
    auto halfAt = [&](int y) { return half(static_cast<float>(y - yTop) / L); };

    // --- water shadow --------------------------------------------------------
    for (int y = yTop; y <= yBot + 4; ++y) {
        const int hw = static_cast<int>(halfAt(std::min(y - 4, yBot))) + 3;
        for (int x = -hw; x <= hw; ++x)
            p.set(cx + x + 3, y + 4, col(0x050F1A, 100));
    }
    // --- keener bow wave & side foam -----------------------------------------
    for (int k = 0; k < 56; ++k) {
        const float a = static_cast<float>(k) / 55.0f;
        const float bowY = yBot - 4.0f - 34.0f * a;
        const float spread = 3.0f + 20.0f * a;
        const std::uint32_t foam = col(0xE8F2FA, static_cast<std::uint8_t>(200 - 160 * a));
        for (int sx : {-1, 1}) {
            const int fx = cx + static_cast<int>(sx * (halfAt(static_cast<int>(bowY)) + spread));
            p.set(fx, static_cast<int>(bowY), foam);
            p.set(fx + sx, static_cast<int>(bowY) + 1, col(0xC8DCF0, 110));
            if (k % 7 == 0) p.set(fx + sx * 2, static_cast<int>(bowY) - 2, col(0xFFFFFF, 150));
        }
    }
    // Sterna wake feathering behind the transom.
    for (int k = 0; k < 10; ++k) {
        const float a = static_cast<float>(k) / 9.0f;
        const int wy = yTop - 3 + k;
        const int off = 13 + static_cast<int>(9.0f * (1.0f - a));
        p.set(cx - off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(70 - 5 * k)));
        p.set(cx + off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(70 - 5 * k)));
    }
    for (int y = yTop + 6; y < yBot - 8; y += 3) {
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw - 2, y, col(0xC8DCF0, 70));
        p.set(cx + hw + 2, y + 1, col(0xC8DCF0, 70));
    }

    // --- AA sponson shoulders beside the funnel (before hull body) -----------
    for (int sx : {-1, 1})
        for (int y = dship::H / 2 - 44; y <= dship::H / 2 - 26; ++y) {
            const int hw = static_cast<int>(halfAt(y));
            for (int x = hw - 1; x <= hw + 3; ++x) {
                const int edge = hw + 3 - x;
                p.set(cx + sx * x, y,
                      edge <= 1 ? col(0x23282D) : (y % 4 == 0 ? col(0x434A50) : col(0x4E555C)));
            }
        }

    // --- hull body with belt plating ------------------------------------------
    for (int y = yTop; y <= yBot; ++y) {
        const int hw = static_cast<int>(halfAt(y));
        for (int x = -hw; x <= hw; ++x) {
            const int dEdge = hw - std::abs(x);
            std::uint32_t c = col(0x565D64);
            if (dEdge <= 1) c = col(0x252A2F);
            else if (dEdge <= 3) {
                const int plate = (y / 9) % 2;
                c = plate ? col(0x454C52) : col(0x50575E);
                if (y % 9 == 0) c = col(0x2E3338);
            }
            else if (x < 0) c = col(0x60686F);
            p.set(cx + x, y, c);
        }
    }
    // --- deck with panel seams --------------------------------------------------
    for (int y = yTop + 3; y <= yBot - 3; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 4;
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x6A7178);
            if ((y % 3) == 0) c = col(0x5B6269);
            else if ((x + y) % 13 == 0) c = col(0x767D84);
            p.set(cx + x, y, c);
        }
    }
    for (int y = yTop + 6; y < yBot - 6; ++y)
        if (y % 3 != 2) p.set(cx, y, col(0x848B92));   // centreline walkway

    // --- gun barbette collars (match JSON component offsets dy +58/+26/-66) ---
    for (int dy : {58, 26, -66}) {
        const float by = static_cast<float>(H / 2 + dy);
        p.fillEllipse(static_cast<float>(cx) + 2.0f, by + 3.0f, 14.0f, 14.0f,
                      col(0x0A0806, 90));
        p.fillEllipse(static_cast<float>(cx), by, 13.5f, 13.5f, col(0x2E3338));
        p.fillEllipse(static_cast<float>(cx), by, 11.5f, 11.5f, col(0x474E54));
        p.fillEllipse(static_cast<float>(cx), by - 1.0f, 8.0f, 8.0f, col(0x33383D));
        for (int r = 0; r < 10; ++r) {                 // collar bolts
            const float a = r * 0.6283f;
            p.set(cx + static_cast<int>(12.5f * std::cos(a)),
                  static_cast<int>(by + 12.5f * std::sin(a)), col(0x6E757C));
        }
    }
    // Superfire step: 'B' mount rides a raised deck block behind 'A'.
    p.fillRect(cx - 15, H / 2 + 16, 30, 2, col(0x767D84));
    p.fillRect(cx - 15, H / 2 + 18, 30, 1, col(0x23282D));

    // --- bridge blockout (the bridge component sprite rides on this) ----------
    p.fillRect(cx - 13, H / 2 - 4, 26, 14, col(0x3E454C));
    p.fillRect(cx - 13, H / 2 - 4, 26, 1, col(0x858C93));
    p.fillRect(cx - 16, H / 2 + 1, 3, 4, col(0x4E555C));    // bridge wings
    p.fillRect(cx + 13, H / 2 + 1, 3, 4, col(0x4E555C));

    // --- raked funnel amidships --------------------------------------------------
    p.fillEllipse(static_cast<float>(cx) + 2.0f, static_cast<float>(H / 2 - 20), 7.5f, 8.5f,
                  col(0x0A0806, 90));
    p.fillEllipse(static_cast<float>(cx) + 0.5f, static_cast<float>(H / 2 - 22), 7.0f, 8.0f,
                  col(0x3A4046));
    p.fillEllipse(static_cast<float>(cx) + 1.0f, static_cast<float>(H / 2 - 23), 5.0f, 6.0f,
                  col(0x17191B));
    p.fillEllipse(static_cast<float>(cx - 1), static_cast<float>(H / 2 - 25), 1.8f, 1.8f,
                  col(0x565C61));
    p.hline(cx - 11, cx + 11, H / 2 - 14, col(0x8A9094, 140));   // bridge walkway

    // --- AA pedestals on the sponsons (match JSON offsets) ----------------------
    for (int sx : {-1, 1}) {
        const float ax = static_cast<float>(cx + sx * 13);
        p.fillEllipse(ax, static_cast<float>(H / 2 - 34), 12.0f, 11.0f, col(0x23282D));
        p.fillEllipse(ax, static_cast<float>(H / 2 - 35), 10.5f, 9.5f, col(0x4E555C));
    }

    // --- twin triple torpedo-tube banks: the destroyer's signature ---------------
    for (int sx : {-1, 1}) {
        const float bx = static_cast<float>(cx + sx * 9);
        const float by = static_cast<float>(H / 2 - 52);
        p.fillEllipse(bx, by, 8.0f, 10.0f, col(0x2E3338));        // tube deck
        p.fillEllipse(bx, by, 6.5f, 8.5f, col(0x4A5158));
        for (int i = -1; i <= 1; ++i) {                           // three tubes
            const float oy = by + i * 4.5f;
            for (int t = 0; t < 6; ++t) {
                const int tx = static_cast<int>(bx) + sx * t;
                const std::uint32_t c =
                    t == 5 ? col(0x0A0C0E)                          // bore
                           : ((t + i) % 2 == 0 ? col(0x1F2429) : col(0x2A3036));
                p.set(tx, static_cast<int>(oy - 1), c);
                p.set(tx, static_cast<int>(oy), c);
                if (t > 0) p.set(tx, static_cast<int>(oy + 1), col(0x14181C));
            }
            p.set(static_cast<int>(bx) + sx * 5, static_cast<int>(oy) - 1, col(0x7C838A));
        }
    }
    // Reload crane between the banks.
    p.fillRect(cx - 1, H / 2 - 58, 2, 12, col(0x565C61));
    p.fillRect(cx - 3, H / 2 - 58, 6, 2, col(0x7C838A));

    // --- stern depth-charge rails -------------------------------------------------
    for (int sx : {-1, 1}) {
        p.fillRect(cx + sx * 6 - 1, H / 2 - 88, 3, 16, col(0x3A4046));
        for (int i = 0; i < 3; ++i) {
            const float dcy = static_cast<float>(H / 2 - 86 + i * 5);
            p.fillEllipse(static_cast<float>(cx + sx * 6), dcy, 2.2f, 2.6f, col(0x2B2F33));
            p.set(cx + sx * 6 - 1, static_cast<int>(dcy) - 1, col(0x6E757C));
        }
    }

    // --- bow & stern fittings -------------------------------------------------------
    for (int sx : {-1, 1}) {                                  // anchor recesses
        const int ax = cx + sx * 11;
        p.fillRect(ax - 2, yBot - 26, 4, 3, col(0x33383D));
        p.set(ax, yBot - 25, col(0x858C93));
    }
    for (int sx : {-1, 1})                                    // breakwater strokes
        for (int i = 0; i < 5; ++i)
            p.set(cx + sx * (7 - i), yBot - 16 - i * 2, col(0x7C838A, 180));
    // Portholes along the belt.
    for (int k = 0; k < 12; ++k) {
        const int py = yTop + 20 + k * 11;
        const int hw = static_cast<int>(halfAt(py)) - 2;
        if (hw > 4) { p.set(cx - hw, py, col(0x1E2226)); p.set(cx + hw, py, col(0x1E2226)); }
    }
    // Vents & hatches scatter.
    for (int k = 0; k < 10; ++k) {
        const int vx = cx - 14 + (k * 37) % 29;
        const int vy = yTop + 18 + (k * 83) % (static_cast<int>(L) - 36);
        if (k % 3 == 0) p.fillRect(vx, vy, 2, 3, col(0x858C93, 200));
        else { p.fillRect(vx, vy, 2, 2, col(0x4E555C)); p.set(vx, vy, col(0x7C838A)); }
    }
    // Rust streaks.
    for (int k = 0; k < 10; ++k) {
        const int rx = cx - 18 + (k * 41) % 37;
        const int ry = yTop + 12 + (k * 71) % (static_cast<int>(L) - 24);
        p.set(rx, ry, col(0x6E4A2C, 85));
        p.set(rx, ry + 1, col(0x6E4A2C, 50));
    }

    // --- damage tiers (kept on the narrow deck) --------------------------------------
    auto scorch = [&](int dx, int dy, int r, bool ember) {
        const int yy = H / 2 + dy;
        const int lim = std::max(4, static_cast<int>(halfAt(yy)) - r - 2);
        dx = std::max(-lim, std::min(lim, dx));
        // Near-opaque alphas: low-alpha darks over the sea read blue, not burnt.
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      static_cast<float>(r), static_cast<float>(r), col(0x14100C, 215));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      r * 0.5f, r * 0.5f, col(0x0A0806, 255));
        if (ember) {
            p.set(cx + dx, yy, col(0xFF8A30));
            p.set(cx + dx + 1, yy, col(0xC25A18));
        }
    };
    if (damage >= 1) { scorch(12, 20, 5, false); scorch(-14, -16, 5, false); }
    if (damage >= 2) {
        scorch(15, -40, 6, true); scorch(-8, 44, 6, true); scorch(10, 0, 5, true);
        for (int k = 0; k < 8; ++k)
            p.set(cx - 14 + (k * 23) % 29, H / 2 - 30 + (k * 17) % 60, col(0x1E1A16, 170));
    }
    if (damage >= 3) {
        scorch(-10, -64, 7, true); scorch(8, 70, 6, true); scorch(0, -14, 6, true);
        // Hull breach starboard waist.
        for (int i = 0; i < 11; ++i) {
            const int by = H / 2 - 6 + i;
            const int hw = static_cast<int>(halfAt(by));
            for (int x = hw - 5 + (i % 3); x <= hw; ++x) {
                p.set(cx + x, by, col(0x141210));
                if ((i + x) % 3 == 0) p.set(cx + x - 1, by, col(0x8A2A10));
            }
        }
    }
    // Smooth flank contours against the sea: rim coverage from the
    // analytic hull profile, not from pixels.
    profileRim(p, static_cast<float>(cx), yTop + 1, yBot - 1,
               [&](int yy) { return halfAt(yy); });

    return p;
}

// Destroyer main gun 32x32: octagonal hood on a steel barbette, TWIN barrels
// with muzzle brakes pointing down at the player — a scaled-down echo of the
// battleship's triple mount so the two ships share a family.
PixelBuf makeDestroyerGun() {
    PixelBuf p(32, 32);
    const float cx = 16.0f, cy = 16.0f;
    p.fillEllipse(cx, cy + 1, 15.0f, 15.0f, col(0x23282C));
    p.fillEllipse(cx, cy, 13.5f, 13.5f, col(0x4A5056));
    p.fillEllipse(cx, cy, 12.0f, 12.0f, col(0x33383D));
    // Octagonal turret body.
    const int oct[13] = {4, 7, 9, 10, 10, 10, 10, 10, 10, 9, 7, 4, 4};
    for (int r = 0; r < 13; ++r) {
        const int y = 10 + r;
        const int hw = oct[static_cast<std::size_t>(r)];
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x6B7278);
            const int dEdge = hw - std::abs(x);
            if (dEdge <= 1) c = col(0x1E2226);
            else if (x <= 0 && r <= 6) c = col(0x949CA3);
            else if (x >= 0 && r > 6) c = col(0x42484D);
            p.set(static_cast<int>(cx) + x, y, c);
        }
    }
    p.fillEllipse(cx, cy - 3, 3.5f, 3.0f, col(0x51575C));     // roof hatch
    p.fillEllipse(cx - 1, cy - 4, 1.5f, 1.5f, col(0x8A9094));
    p.fillRect(24, 11, 2, 2, col(0x1B1E21));                  // sight hood
    // Mantlet bar across the barrel bases.
    p.fillRect(7, 21, 18, 4, col(0x3C4247));
    p.fillRect(7, 21, 18, 1, col(0x8A9094));
    p.fillRect(7, 24, 18, 1, col(0x1B1E21));
    // Twin barrels with muzzle brakes.
    for (int bx : {9, 19}) {
        p.fillRect(bx + 3, 25, 2, 7, col(0x000000, 80));   // deck shadow
        p.fillRect(bx, 24, 4, 8, col(0x41474D));            // barrel body
        p.fillRect(bx, 24, 1, 8, col(0x9AA2A8));            // lit rail
        p.fillRect(bx + 3, 24, 1, 8, col(0x23272B));        // dark edge
        p.fillRect(bx - 1, 29, 6, 2, col(0x7C838A));        // muzzle brake
        p.fillRect(bx - 1, 31, 6, 1, col(0x23272B));
    }
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// Compact destroyer bridge 40x44: boxy glass-enclosed pilot house with wide
// bridge wings, director top with rangefinder, mast and ensign.
PixelBuf makeDestroyerBridge() {
    PixelBuf p(40, 44);
    const int cx = 20;
    // Deck pedestal.
    p.fillEllipse(20.0f, 38.0f, 17.0f, 5.0f, col(0x23282C));
    p.fillEllipse(19.0f, 37.0f, 15.5f, 4.0f, col(0x4E5459));
    // Pilot house.
    p.fillRect(8, 12, 24, 18, col(0x5C6368));
    p.fillRect(8, 12, 24, 1, col(0x8A9094));
    p.fillRect(30, 12, 2, 18, col(0x33383D));
    for (int x = 11; x <= 26; x += 4) {                    // window band
        p.fillRect(x, 15, 3, 5, col(0x3A5A6E));
        p.set(x, 15, col(0x9FD0E8));
    }
    // Bridge wings with searchlight dots.
    p.fillRect(3, 18, 34, 4, col(0x4E555C));
    p.fillRect(3, 18, 34, 1, col(0x858C93));
    p.fillRect(3, 21, 34, 1, col(0x1B1E21, 170));
    p.set(4, 19, col(0xF2E8B8));
    p.set(35, 19, col(0xF2E8B8));
    // Director top + rangefinder arms.
    p.fillRect(14, 5, 12, 7, col(0x666D73));
    p.fillRect(14, 5, 12, 1, col(0x8A9094));
    p.fillEllipse(20.0f, 4.0f, 5.5f, 2.5f, col(0x33383D));
    p.hline(13, 27, 3, col(0x4E5459));
    p.set(13, 2, col(0x8A9094));
    p.set(27, 2, col(0x8A9094));
    // Mast + ensign.
    p.vline(cx, 6, 36, col(0x2A2E33));
    p.vline(cx - 1, 6, 36, col(0x6E757C));
    p.fillRect(cx + 1, 8, 5, 3, col(0xC23A2A));
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// ---------------------------------------------------------------------------
// Carrier KAGAMI-CLASS: flight-deck silhouette (flat-top), drawn bow-DOWN
// like every other ship. The deck is the star: wood planking, landing
// markings, deck-edge elevators, parked strike group and the island block
// the component tower rides on. damage: 0 intact, 1 light, 2 heavy, 3 wreck.
// ---------------------------------------------------------------------------
namespace cship {
constexpr int W = 176, H = 204;
constexpr int yTop = 10, yBot = 194;
constexpr float L = static_cast<float>(yBot - yTop);

inline float half(float t) {
    // 0 = stern (top, blunt transom) .. 1 = bow tip (bottom).
    if (t < 0.05f) return 44.0f + 21.0f * std::sqrt(std::max(0.0f, t / 0.05f));
    if (t < 0.80f) return 65.0f;                                     // straight lift side
    if (t < 0.93f) return 65.0f - 36.0f * ((t - 0.80f) / 0.13f);     // clipped bow
    return std::max(0.0f, 29.0f * (1.0f - (t - 0.93f) / 0.07f));
}
}  // namespace cship

PixelBuf makeCarrierHull(int damage) {
    using namespace cship;
    const int cx = W / 2;
    PixelBuf p(W, H);
    auto halfAt = [&](int y) { return half(static_cast<float>(y - yTop) / L); };

    // --- water shadow ---------------------------------------------------------
    for (int y = yTop; y <= yBot + 4; ++y) {
        const int hw = static_cast<int>(halfAt(std::min(y - 4, yBot))) + 3;
        for (int x = -hw; x <= hw; ++x)
            p.set(cx + x + 3, y + 4, col(0x050F1A, 100));
    }
    // --- bow wave & side foam ---------------------------------------------------
    for (int k = 0; k < 46; ++k) {
        const float a = static_cast<float>(k) / 45.0f;
        const float bowY = yBot - 2.0f - 40.0f * a;
        const float spread = 4.0f + 26.0f * a;
        const std::uint32_t foam = col(0xE8F2FA, static_cast<std::uint8_t>(190 - 150 * a));
        for (int sx : {-1, 1}) {
            const int fx = cx + static_cast<int>(sx * (halfAt(static_cast<int>(bowY)) + spread));
            p.set(fx, static_cast<int>(bowY), foam);
            p.set(fx + sx, static_cast<int>(bowY) + 1, col(0xC8DCF0, 100));
            if (k % 6 == 0) p.set(fx + sx * 2, static_cast<int>(bowY) - 2, col(0xFFFFFF, 150));
        }
    }
    for (int k = 0; k < 12; ++k) {                     // stern wake feathering
        const float a = static_cast<float>(k) / 11.0f;
        const int wy = yTop - 4 + k;
        const int off = 26 + static_cast<int>(16.0f * (1.0f - a));
        p.set(cx - off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(70 - 5 * k)));
        p.set(cx + off, wy, col(0xC8DCF0, static_cast<std::uint8_t>(70 - 5 * k)));
    }
    for (int y = yTop + 8; y < yBot - 8; y += 3) {     // lift-side foam ticks
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw - 2, y, col(0xC8DCF0, 70));
        p.set(cx + hw + 2, y + 1, col(0xC8DCF0, 70));
    }

    // --- hull / lift side (grey plating under the overhanging deck) -----------
    for (int y = yTop; y <= yBot; ++y) {
        const int hw = static_cast<int>(halfAt(y));
        for (int x = -hw; x <= hw; ++x) {
            const int dEdge = hw - std::abs(x);
            std::uint32_t c = col(0x4A5158);
            if (dEdge <= 1) c = col(0x22272B);
            else if (x < 0) c = col(0x565D64);
            p.set(cx + x, y, c);
        }
    }
    // Hangar-side vent dashes along both lift sides.
    for (int y = yTop + 14; y < yBot - 18; y += 11) {
        const int hw = static_cast<int>(halfAt(y));
        if (hw > 20)
            for (int sx : {-1, 1})
                p.fillRect(cx + sx * (hw - 3) - 1, y, 2, 4, col(0x2C3136));
    }

    // --- flight deck overhang ring ------------------------------------------------
    for (int y = yTop + 2; y <= yBot - 2; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 3;
        for (int x = -hw - 1; x <= hw + 1; ++x)
            p.set(cx + x, y, (hw + 1 - std::abs(x)) <= 0 ? col(0x1E2225) : col(0x2A2E32));
    }
    // --- wood flight deck ---------------------------------------------------------
    for (int y = yTop + 3; y <= yBot - 3; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 5;
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x574D41);
            if ((y % 3) == 0) c = col(0x493F34);                 // plank lines
            else if ((x + y * 2) % 23 == 0) c = col(0x61564A);   // sun-bleached seam
            else if ((x * 7 + y) % 41 == 0) c = col(0x4E4438);
            p.set(cx + x, y, c);
        }
    }
    // Deck-edge catwalk highlight.
    for (int y = yTop + 4; y < yBot - 4; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 5;
        p.set(cx - hw - 1, y, col(0x7A6F60, 160));
        p.set(cx + hw + 1, y + 1, col(0x3A332B, 160));
    }

    // --- landing markings ---------------------------------------------------------
    for (int y = yTop + 16; y < yBot - 26; ++y)                  // dashed centreline
        if ((y % 14) < 7) { p.set(cx, y, col(0xD8D2C4)); p.set(cx + 1, y, col(0xB8B2A4)); }
    for (int bx : {-14, 14})                                      // touchdown bars
        p.fillRect(cx + bx - 1, yBot - 74, 2, 22, col(0xD8D2C4));
    p.hline(cx - 30, cx + 30, yBot - 84, col(0xD8D2C4, 200));     // beam stop line
    for (int k = 0; k < 7; ++k)                                   // red hazard tabs on line
        p.fillRect(cx - 28 + k * 9, yBot - 85, 4, 2, col(0xB03A28));

    // --- island deckhouse blockout (component tower rides on this) --------------
    p.fillRect(cx + 28, H / 2 - 40, 34, 52, col(0x3E454C));
    p.fillRect(cx + 28, H / 2 - 40, 34, 1, col(0x858C93));
    p.fillRect(cx + 60, H / 2 - 40, 2, 52, col(0x252A2F));
    p.fillEllipse(static_cast<float>(cx + 40), static_cast<float>(H / 2 - 24), 6.0f, 7.0f,
                  col(0x17191B));                                 // uptake
    p.fillEllipse(static_cast<float>(cx + 40) + 1.5f, static_cast<float>(H / 2 - 22),
                  4.5f, 5.0f, col(0x0A0806, 120));
    p.fillRect(cx + 30, H / 2 + 4, 10, 6, col(0x4E555C));         // chart room wing

    // --- AA sponson pedestals (match JSON component offsets) -----------------------
    for (int sx : {-1, 1})
        for (int sy : {-1, 1}) {
            const float ax = static_cast<float>(cx + sx * 54);
            const float ay = static_cast<float>(H / 2 + sy * (sy < 0 ? 52 : 54));
            p.fillEllipse(ax + 1.5f, ay + 2.5f, 12.5f, 12.0f, col(0x0A0806, 90));
            p.fillEllipse(ax, ay, 12.5f, 12.0f, col(0x23282D));
            p.fillEllipse(ax, ay - 1.0f, 10.5f, 10.0f, col(0x4E555C));
        }
    // Bow gun collar.
    p.fillEllipse(static_cast<float>(cx) + 2.0f, static_cast<float>(H / 2 + 77), 13.0f, 12.0f,
                  col(0x0A0806, 90));
    p.fillEllipse(static_cast<float>(cx), static_cast<float>(H / 2 + 74), 13.0f, 12.0f,
                  col(0x2E3338));
    p.fillEllipse(static_cast<float>(cx), static_cast<float>(H / 2 + 73), 10.0f, 9.0f,
                  col(0x474E54));

    // --- deck-edge elevators (starboard fwd, port aft) ------------------------------
    {
        auto elevator = [&](int ex, int ey, int w, int h) {
            for (int y = ey; y < ey + h; ++y)
                for (int x = ex; x < ex + w; ++x) {
                    std::uint32_t c = col(0x645948);
                    if ((y % 3) == 0) c = col(0x554B3D);
                    p.set(x, y, c);
                }
            p.hline(ex, ex + w - 1, ey, col(0x8A7E6C));
            p.hline(ex, ex + w - 1, ey + h - 1, col(0x2E2820));
            p.vline(ex, ey, ey + h - 1, col(0x2E2820));
            p.vline(ex + w - 1, ey, ey + h - 1, col(0x1E1A15));
        };
        elevator(cx + 58, H / 2 + 26, 13, 26);      // starboard, overhanging edge
        elevator(cx - 71, H / 2 - 40, 13, 26);      // port
    }
    // Cranes near the stern.
    p.fillRect(cx - 24, H / 2 - 66, 2, 10, col(0x565C61));
    p.fillRect(cx - 28, H / 2 - 66, 8, 2, col(0x7C838A));
    p.fillRect(cx + 16, H / 2 - 66, 2, 10, col(0x565C61));
    p.fillRect(cx + 12, H / 2 - 66, 8, 2, col(0x7C838A));

    // --- parked strike group near the bow (decorative) ------------------------------
    auto parked = [&](int px, int py, int dir) {
        p.vline(px, py - 5, py + 5, col(0x2E343A));                    // fuselage
        p.hline(px - 5, px + 5, py + dir * 2, col(0x33393F));          // wings
        p.hline(px - 3, px + 3, py + 5, col(0x2E343A));                // tailplane
        p.set(px, py - 5, col(0x4A5056));                              // nose glint
        p.set(px + 1, py, col(0xB03A28, 200));                         // fuselage dot
    };
    parked(cx - 32, H / 2 + 14, 1);
    parked(cx, H / 2 + 18, 1);
    parked(cx + 32, H / 2 + 14, 1);
    parked(cx - 16, H / 2 + 38, 1);
    parked(cx + 16, H / 2 + 38, 1);

    // Portholes & rust.
    for (int k = 0; k < 14; ++k) {
        const int py = yTop + 24 + k * 11;
        const int hw = static_cast<int>(halfAt(py)) - 2;
        if (hw > 10) { p.set(cx - hw, py, col(0x1E2226)); p.set(cx + hw, py, col(0x1E2226)); }
    }
    for (int k = 0; k < 12; ++k) {
        const int rx = cx - 50 + (k * 41) % 101;
        const int ry = yTop + 12 + (k * 71) % (static_cast<int>(L) - 24);
        p.set(rx, ry, col(0x6E4A2C, 85));
        p.set(rx, ry + 1, col(0x6E4A2C, 50));
    }

    // --- damage tiers ----------------------------------------------------------------
    auto scorch = [&](int dx, int dy, int r, bool ember) {
        const int yy = H / 2 + dy;
        const int lim = std::max(4, static_cast<int>(halfAt(yy)) - r - 6);
        dx = std::max(-lim, std::min(lim, dx));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      static_cast<float>(r), static_cast<float>(r), col(0x14100C, 215));
        p.fillEllipse(static_cast<float>(cx + dx), static_cast<float>(yy),
                      r * 0.5f, r * 0.5f, col(0x0A0806, 255));
        if (ember) {
            p.set(cx + dx, yy, col(0xFF8A30));
            p.set(cx + dx + 1, yy, col(0xC25A18));
        }
    };
    if (damage >= 1) { scorch(-24, 30, 7, false); scorch(20, -34, 6, false); }
    if (damage >= 2) {
        scorch(-40, -10, 8, true); scorch(30, 46, 7, true); scorch(0, -58, 6, true);
        for (int k = 0; k < 14; ++k)
            p.set(cx - 48 + (k * 23) % 97, H / 2 - 40 + (k * 17) % 80, col(0x1E1A16, 170));
    }
    if (damage >= 3) {
        scorch(-30, -64, 9, true); scorch(24, 70, 8, true); scorch(6, -6, 8, true);
        // Starboard deck-edge breach: flight deck torn open aft.
        for (int i = 0; i < 15; ++i) {
            const int by = H / 2 - 24 + i;
            const int hw = static_cast<int>(halfAt(by));
            for (int x = hw - 13 + (i % 3); x <= hw; ++x) {
                p.set(cx + x, by, col(0x141210));
                if ((i + x) % 3 == 0) p.set(cx + x - 1, by, col(0x8A2A10));
            }
        }
    }
    // Smooth flank contours against the sea: rim coverage from the
    // analytic hull profile, not from pixels.
    profileRim(p, static_cast<float>(cx), yTop + 1, yBot - 1,
               [&](int yy) { return halfAt(yy); });

    return p;
}

// Carrier island tower 40x54: bridge windows, stubby funnel, lattice mast
// with yardarm and a recognition disc — the command component.
PixelBuf makeCarrierIsland() {
    PixelBuf p(40, 54);
    const int cx = 18;
    // Deck pedestal shadow + base.
    p.fillEllipse(20.0f, 49.0f, 18.0f, 5.0f, col(0x23282C));
    p.fillEllipse(19.0f, 48.0f, 16.5f, 4.0f, col(0x4E5459));
    // Tower block.
    p.fillRect(8, 16, 22, 26, col(0x5C6368));
    p.fillRect(8, 16, 22, 1, col(0x8A9094));
    p.fillRect(28, 16, 2, 26, col(0x33383D));
    for (int x = 10; x <= 26; x += 4) {                       // bridge windows
        p.fillRect(x, 19, 3, 5, col(0x3A5A6E));
        p.set(x, 19, col(0x9FD0E8));
    }
    // Recognition disc on the tower face.
    p.fillRect(12, 28, 12, 9, col(0xE6E0D4));
    p.fillEllipse(18.0f, 32.5f, 3.2f, 3.2f, col(0xC23A2A));
    // Stub funnel aft of the tower with a smoke lip.
    p.fillRect(26, 24, 9, 16, col(0x454C52));
    p.fillRect(26, 24, 9, 1, col(0x767D84));
    p.fillEllipse(30.5f, 24.0f, 4.2f, 2.4f, col(0x17191B));
    // Lattice mast + yardarm + radar bar.
    p.vline(cx, 2, 16, col(0x2A2E33));
    p.vline(cx + 1, 2, 16, col(0x6E757C));
    p.hline(cx - 8, cx + 8, 8, col(0x4E555C));
    p.hline(cx - 5, cx + 5, 3, col(0x8A9094));
    p.set(cx, 1, col(0xF2E8B8));                              // masthead light
    // Bridge wing platforms.
    p.fillRect(4, 26, 4, 3, col(0x4E555C));
    p.set(4, 27, col(0xF2E8B8, 220));
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}


// Sky Fortress GX-7 144x96: four-engine heavy bomber, drawn nose-DOWN
// (it faces the player), full-span wings, gun blisters, prop blur discs.
// Sky Fortress GX-7 (pass 2) 176x120: six-engine airborne fortress drawn
// nose-DOWN. Primary silhouette: huge straight wing + fat armoured fuselage
// + triple tail. Secondary: six prop nacelles, armoured core band, command
// greenhouse, dorsal barbette. damage: 0 intact, 2 wreck (burned/breached).
PixelBuf makeGiantBomberHull(int damage) {
    const int W = 176, H = 120;
    const int cx = W / 2;
    PixelBuf p(W, H);

    // --- wings: full span, reinforced roots, rounded tips --------------------
    const int wingTop = 44, wingBot = 76;
    for (int x = 4; x < W - 4; ++x) {
        const int tipIn = std::min(x - 4, W - 5 - x);
        const int tInset = tipIn < 10 ? (10 - tipIn) / 2 : 0;
        const int root = std::abs(x - cx) < 22 ? 4 : 0;      // heavy wing roots
        for (int y = wingTop + tInset - root; y <= wingBot - tInset + root; ++y) {
            std::uint32_t c = col(0x5A5F63);                 // wing mid
            if (y == wingTop - root) c = col(0x7E838A);      // leading edge lit
            else if (y >= wingBot + root - 1) c = col(0x3C4046);
            else if (root && y < wingTop) c = col(0x6E7378); // lit root plate
            else if ((x * 7 + y) % 23 == 0) c = col(0x51565B);
            p.set(x, y, c);
        }
        if (x % 8 == 0)                                      // panel lines
            for (int y = wingTop - root + 1; y < wingBot + root - 1; ++y)
                if (p.get(x, y) && y != wingTop - root) p.set(x, y, col(0x484D52));
    }
    // Enemy roundels outboard.
    for (int rx : {31, 145}) {
        p.fillEllipse(static_cast<float>(rx), 60.0f, 7.0f, 7.0f, col(0xC23A2A));
        p.set(rx - 7, 60, col(0xF2F2F2));
        p.set(rx + 7, 60, col(0xF2F2F2));
        p.set(rx, 53, col(0xF2F2F2));
        p.set(rx, 67, col(0xF2F2F2));
    }

    // --- fuselage: fat armoured hull, nose toward the player ------------------
    for (int y = 8; y <= 112; ++y) {
        const float t = static_cast<float>(y - 8) / 104.0f;  // 0 tail .. 1 nose
        float hw = 15.0f;
        if (t > 0.82f) hw = 15.0f * (1.0f - (t - 0.82f) / 0.18f * 0.6f);
        if (t < 0.10f) hw = 6.0f + 9.0f * (t / 0.10f);
        for (int x = -static_cast<int>(hw); x <= static_cast<int>(hw); ++x) {
            std::uint32_t c = col(0x666B70);
            const int dEdge = static_cast<int>(hw) - std::abs(x);
            if (dEdge <= 1) c = col(0x33383D);
            else if (x < 0) c = col(0x767B81);               // lit flank
            p.set(cx + x, y, c);
        }
    }
    // ARMORED CORE BAND across fuselage + wing centre (primary mass cue).
    for (int y = 50; y <= 70; ++y)
        for (int x = cx - 26; x <= cx + 26; ++x) {
            std::uint32_t c = col(0x4A5056);
            if (y == 50) c = col(0x6E7378);                  // top-lit edge
            if (y == 70) c = col(0x2A2E33);                  // under-shadow
            if ((x + y) % 9 == 0 && y > 50 && y < 70) c = col(0x3C4046);  // seams
            if (y > 50 && y < 70 && (x % 6) == 0) c = shade(c, 1.15f);    // rivets
            p.set(x, y, c);
        }
    // Amber hazard trim on the core edges.
    for (int x = cx - 26; x <= cx + 26; x += 4) {
        p.fillRect(x, 49, 2, 1, col(0xC8A83C));
        p.fillRect(x, 71, 2, 1, col(0xC8A83C));
    }
    // Command greenhouse toward the nose: three window rows + deflector.
    p.fillRect(cx - 8, 82, 17, 14, col(0x3A5A6E));
    for (int wy : {82, 87, 92}) p.hline(cx - 8, cx + 8, wy, col(0x2A4454));
    for (int wx = cx - 6; wx <= cx + 6; wx += 4) p.vline(wx, 82, 95, col(0x2A4454));
    p.set(cx - 7, 83, col(0x9FD0E8));
    p.set(cx - 6, 83, col(0x7FB8D8));
    p.set(cx + 1, 88, col(0x7FB8D8));
    p.fillRect(cx - 9, 96, 19, 2, col(0x4E5459));            // deflector plate
    // Dorsal barbette housing + radio blister (mid/aft).
    p.fillEllipse(static_cast<float>(cx), 38.0f, 5.0f, 6.0f, col(0x4E5459));
    p.fillEllipse(static_cast<float>(cx) - 1, 37.0f, 2.5f, 3.0f, col(0x8A9094));
    p.fillEllipse(static_cast<float>(cx), 24.0f, 3.0f, 4.0f, col(0x4E5459));
    // Flank gun ports.
    for (int y : {40, 46, 76, 80}) {
        p.set(cx - 15, y, col(0x2B2F33));
        p.set(cx + 15, y, col(0x2B2F33));
    }

    // --- six engine nacelles with prop blur ----------------------------------
    for (int ex : {20, 42, 64, 112, 134, 156}) {
        const int nx = ex;
        const int inner = std::abs(nx - cx) < 50 ? 1 : 0;    // inboard pair armed
        for (int q = 0; q < 12; ++q)                         // exhaust staining
            p.set(nx + 2 + q / 4, 42 - q, col(0x1A1D20, 85 - q * 6));
        p.fillRect(nx - 5, 44, 11, 30, col(0x4E5459));       // nacelle body
        p.fillRect(nx - 5, 44, 2, 30, col(0x767B81));
        p.fillRect(nx + 4, 44, 2, 30, col(0x2E3338));
        p.fillRect(nx - 4, 72, 9, 4, col(0x33383D));         // cowl
        p.fillRect(nx - 3, 74, 7, 1, col(0x565C61));
        if (inner) {                                         // wing-gun muzzle
            p.fillRect(nx - 1, 76, 3, 5, col(0x41474D));
            p.fillRect(nx - 1, 76, 1, 5, col(0x9AA2A8));
            p.fillRect(nx - 2, 60, 5, 3, col(0x2B2F33));     // gun pod
        }
        if (!(damage >= 2 && nx == 20)) {                    // outer-left prop lost
            p.fillEllipse(static_cast<float>(nx + 0.5f), 80.0f, 11.0f, 4.0f,
                          col(0xB8C4CC, 60));
            p.fillEllipse(static_cast<float>(nx + 0.5f), 80.0f, 7.0f, 2.2f,
                          col(0xD8E4EC, 50));
            p.hline(nx - 10, nx + 11, 80, col(0xE8F2FA, 40));
        } else {                                             // bent stub blades
            p.fillRect(nx - 1, 74, 2, 7, col(0x2B2F33));
            p.fillRect(nx - 6, 78, 5, 2, col(0x2B2F33));
        }
        p.fillRect(nx, 76, 2, 3, col(0x2B2F33));             // hub
    }

    // --- triple tail: H-stabiliser + twin fins + centre fin -------------------
    for (int x = cx - 38; x <= cx + 38; ++x) {
        const int tipIn = std::min(x - (cx - 38), (cx + 38) - x);
        const int ins = tipIn < 5 ? (5 - tipIn) : 0;
        for (int y = 18 + ins; y <= 28 - ins; ++y) {
            std::uint32_t c = col(0x51565B);
            if (y == 18 + ins) c = col(0x767B81);
            if (y >= 27 - ins) c = col(0x3C4046);
            p.set(x, y, c);
        }
    }
    for (int fx : {cx - 24, cx + 24, cx}) {                  // three fins
        const int fw = fx == cx ? 3 : 2;
        const int fh = fx == cx ? 16 : 12;
        p.fillRect(fx - fw, 2, fw * 2 + 1, fh, col(0x3C4046));
        p.fillRect(fx - fw, 2, fw, fh, col(0x5C6368));
        p.fillRect(fx - fw, 2, 1, fh, col(0x8A9094));
        p.fillRect(fx - 1, 2, 3, 4, col(0xC23A2A));          // fin flashes
    }

    // --- wreck state: torn wing panel + burns --------------------------------
    if (damage >= 2) {
        // Blown panel out of the port outer wing (jagged transparent gap).
        for (int x = 8; x <= 28; ++x) {
            const int gapTop = 46 + (x % 5);
            const int gapBot = 66 - ((x + 3) % 6);
            for (int y = gapTop; y <= gapBot; ++y) p.set(x, y, 0);
            p.set(x, gapTop - 1, col(0xAEB4BA));             // torn bright edge
            p.set(x, gapBot + 1, col(0x24282C));
            if ((x + 1) % 5 == 0) p.set(x, gapTop - 2, col(0x8A9094));
        }
        return addScorchTo(std::move(p), 2);
    }
    return addScorchTo(std::move(p), damage);
}

// Wake foam trail (drawn behind the battleship's stern).
PixelBuf makeWake() {
    const int W = 80, H = 128;
    PixelBuf p(W, H);
    // Foam widens toward the bottom (stern) and dissipates upward.
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / H;          // 0 far .. 1 stern
        const float w = 3.0f + 36.0f * t * t;
        const int a = static_cast<int>(150.0f * t * (1.0f - (1.0f - t) * 0.25f));
        for (int sx : {-1, 1}) {
            const int fx = W / 2 + static_cast<int>(sx * w);
            p.set(fx, y, col(0xE8F2FA, static_cast<std::uint8_t>(a)));
            if (y % 2 == 0)
                p.set(fx + sx, y, col(0xC8DCF0, static_cast<std::uint8_t>(a / 2)));
            if (y % 5 == 0)   // foam clumps for texture
                p.set(fx - sx * 3, y, col(0xFFFFFF, static_cast<std::uint8_t>(a * 2 / 3)));
        }
        // Churned water core.
        if (y % 3 == 0 && t > 0.25f)
            p.set(W / 2 + ((y * 7) % 13) - 6, y,
                  col(0x9CC8E8, static_cast<std::uint8_t>(70 * t)));
    }
    return p;
}

// --- Level 7 boss: AIRFIELD COMMAND ------------------------------------------
// A fortified enemy air base. The hull sprite carries the runway section that
// continues seamlessly into the stage's airfield terrain columns (the boss
// holds station at x=192, exactly centred over the two runway lanes), with
// apron, taxiways, parked aircraft and grassy margins around it. Hangars,
// flak nests and the control tower are boss components drawn on top.

PixelBuf makeAirfieldHull(int damage) {
    constexpr int W = 224, H = 224;
    const int cx = W / 2, cy = H / 2;
    PixelBuf p(W, H);

    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    auto hash = [](int x, int y, int s) {
        std::uint32_t h = static_cast<std::uint32_t>(x * 374761393 + y * 668265263 +
                                                     s * 1274126177);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (h ^ (h >> 16)) & 0xFFFFu;
    };
    auto frand = [&](int x, int y, int s) { return static_cast<float>(hash(x, y, s)) / 65535.0f; };
    auto softDisc = [&](float ex, float ey, float rx, float ry, std::uint32_t c) {
        for (int y = static_cast<int>(ey - ry) - 1; y <= static_cast<int>(ey + ry) + 1; ++y)
            for (int x = static_cast<int>(ex - rx) - 1; x <= static_cast<int>(ex + rx) + 1; ++x) {
                const float dx = (x + 0.5f - ex) / rx, dy = (y + 0.5f - ey) / ry;
                const float q2 = dx * dx + dy * dy;
                if (q2 > 1.0f) continue;
                if (q2 > 0.68f) {
                    const float f = 1.0f - (q2 - 0.68f) / 0.32f;
                    const int a = static_cast<int>(static_cast<float>(c & 0xFFu) * f);
                    p.blend(x, y, (c & 0xFFFFFF00u) | static_cast<std::uint32_t>(a));
                } else {
                    p.blend(x, y, c);
                }
            }
    };

    const std::uint32_t jgDeep = col(0x163314);
    const std::uint32_t jgDark = col(0x24501F);
    const std::uint32_t jgMid  = col(0x3A7A31);
    const std::uint32_t jgHi   = col(0x5C9940);

    // --- poured apron concrete --------------------------------------------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            std::uint32_t c = mix(col(0x82827C), col(0x8E8E88), frand(x, y, 1));
            if ((y % 16) == 0 || (x % 16) == 0) c = mix(c, col(0x6E6E68), 0.8f);  // slab seams
            else if ((y % 16) == 1 || (x % 16) == 1) c = mix(c, col(0x94948E), 0.35f);
            if (frand(x >> 3, y >> 3, 7) > 0.90f) c = mix(c, col(0x6F6F69), 0.5f);  // oil stains
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx + dy < -60.0f) c = mix(c, col(0x9C9C96), 0.16f);   // sun side
            if (dx + dy > 60.0f) c = mix(c, col(0x72726C), 0.18f);    // shade side
            p.set(x, y, c);
        }

    // --- grassy margins with irregular edges ------------------------------------
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float leftB  = 20.0f + 8.0f * std::sin(y * 0.11f + 1.3f) +
                                 5.0f * std::sin(y * 0.31f + 4.0f);
            const float rightB = W - 20.0f + 8.0f * std::sin(y * 0.09f + 3.1f) +
                                 5.0f * std::sin(y * 0.27f + 1.0f);
            const bool inL = x < leftB, inR = x > rightB;
            if (!inL && !inR) continue;
            const float over = inL ? (leftB - x) : (x - rightB);   // depth into grass
            if (over < 2.0f && frand(x, y, 2) > over * 0.5f) continue;  // ragged edge
            std::uint32_t c = mix(jgDark, jgMid, frand(x, y, 3));
            if (frand(x >> 1, y >> 1, 4) > 0.80f) c = mix(c, jgDeep, 0.6f);
            if (frand(x, y, 5) > 0.90f) c = mix(c, jgHi, 0.5f);
            p.set(x, y, c);
        }

    // --- runway: asphalt through the centre, matching the terrain lanes ----------
    const int rxA0 = 76, rxA1 = 148;
    for (int y = 0; y < H; ++y)
        for (int x = rxA0; x < rxA1; ++x) {
            std::uint32_t c = mix(col(0x4A4C50), col(0x54565A), frand(x, y, 6));
            if ((y % 16) == 0) c = mix(c, col(0x404246), 0.85f);   // transverse seams
            if (frand(x >> 2, y >> 2, 9) > 0.88f) c = mix(c, col(0x5E6064), 0.5f);  // wear
            if (frand(x >> 3, y >> 3, 10) > 0.94f) c = mix(c, col(0x3A3C40), 0.6f); // patches
            // weathered white edge lines
            if ((x == 78 || x == 79 || x == 144 || x == 145) && (y + x) % 5 != 0)
                c = mix(c, col(0xE8E8E2), 0.85f);
            p.set(x, y, c);
        }
    // threshold ladder at the near (bottom) end of the base's runway section
    for (int s = 0; s < 5; ++s) {
        const int tx = 84 + s * 12;
        for (int y = 206; y < 216; ++y)
            for (int x = tx; x < tx + 5; ++x)
                if ((x + y) % 6 != 0) p.set(x, y, mix(col(0xE8E8E2), col(0x54565A), 0.15f));
    }

    // --- taxiway stubs with yellow guide lines -----------------------------------
    for (int side : {-1, 1}) {
        const int y0 = 96, y1 = 104;
        const int xs = side < 0 ? 24 : rxA1, xe = side < 0 ? rxA0 : W - 24;
        for (int x = xs; x < xe; ++x)
            for (int y = y0; y <= y1; ++y)
                p.set(x, y, mix(col(0x7E7E78), col(0x8C8C86), frand(x, y, 11)));
        for (int x = xs + 2; x < xe - 2; ++x)  // centre guide line
            p.set(x, 100, mix(col(0xC9A23A), col(0x8C8C86), (x % 9 == 0) ? 0.5f : 0.0f));
    }
    // spur lines out to the hangar pads
    for (int side : {-1, 1})
        for (int yy : {56, 164})
            for (int d = 0; d < 40; ++d) {
                const int x = cx + side * (36 + d);
                if ((x + yy) % 8 != 0) p.blend(x, yy, col(0xC9A23A, 170));
            }

    // --- hangar apron pads (lighter concrete, tie-downs) --------------------------
    for (int sx : {-1, 1})
        for (int sy : {-1, 1}) {
            const int hx = cx + sx * 78, hy = cy + sy * 60;
            for (int y = hy - 28; y <= hy + 28; ++y)
                for (int x = hx - 40; x <= hx + 40; ++x) {
                    if (x < 2 || x >= W - 2 || y < 1 || y >= H - 1) continue;
                    if (x >= rxA0 - 2 && x <= rxA1 + 2) continue;  // keep runway clear
                    const float ex = std::fabs(x - hx) / 40.0f, ey = std::fabs(y - hy) / 28.0f;
                    if (ex * ex + ey * ey > 1.0f) continue;
                    std::uint32_t c = mix(col(0x94948C), col(0x9E9E96), frand(x, y, 12));
                    if ((y % 14) == 0) c = mix(c, col(0x7C7C76), 0.7f);
                    p.set(x, y, c);
                }
            // tie-down marks
            for (int t = -2; t <= 2; ++t)
                p.fillRect(hx + t * 14 - 1, hy + 18, 2, 2, col(0xC9A23A, 190));
        }

    // --- parked aircraft on the central aprons ------------------------------------
    auto parkPlane = [&](float px, float py, int k) {
        softDisc(px + 2.0f, py + 3.0f, 12.0f, 9.0f, col(0x1A160F, 70));
        // wings
        softDisc(px, py - 1.0f, 13.0f, 4.2f, col(0x6E7460));
        softDisc(px, py - 2.0f, 12.0f, 3.0f, col(0x7E8470));
        // fuselage
        softDisc(px, py, 3.2f, 11.0f, col(0x767C68));
        softDisc(px - 1.0f, py - 2.0f, 1.6f, 8.0f, col(0x888E7A));
        // tailplane
        softDisc(px, py + 9.0f, 6.5f, 2.2f, col(0x6E7460));
        // cowling + canopy
        softDisc(px, py - 10.0f, 2.6f, 2.6f, col(0x3E4238));
        softDisc(px, py - 3.0f, 1.8f, 3.0f, col(0x2E3A44));
        // hinomaru on the wings
        softDisc(px - 7.5f, py - 1.0f, 2.4f, 2.0f, col(0xC22A1E));
        softDisc(px + 7.5f, py - 1.0f, 2.4f, 2.0f, col(0xC22A1E));
        if (frand(k, 3, 13) < 0.5f)  // camo mottle
            softDisc(px + 3.0f, py + 2.0f, 3.0f, 2.0f, col(0x565C48, 150));
    };
    parkPlane(static_cast<float>(cx) - 44.0f, static_cast<float>(cy) - 8.0f, 1);
    parkPlane(static_cast<float>(cx) + 44.0f, static_cast<float>(cy) - 8.0f, 2);
    parkPlane(static_cast<float>(cx) - 44.0f, static_cast<float>(cy) + 40.0f, 3);
    parkPlane(static_cast<float>(cx) + 44.0f, static_cast<float>(cy) + 40.0f, 4);

    // --- tree clusters along the grass margins ------------------------------------
    for (int k = 0; k < 22; ++k) {
        const float ty = 12.0f + static_cast<float>(k) * 9.4f + frand(k, 1, 14) * 6.0f;
        if (ty > H - 10.0f) continue;
        const float tx = (k % 2 == 0) ? 6.0f + frand(k, 2, 15) * 14.0f
                                      : W - 20.0f + frand(k, 2, 15) * 14.0f;
        const float s = 3.6f + frand(k, 3, 16) * 2.6f;
        softDisc(tx + s * 0.4f, ty + s * 0.5f, s * 0.95f, s * 0.6f, col(0x08130A, 70));
        softDisc(tx, ty, s * 0.8f, s * 0.66f, jgDeep);
        softDisc(tx - s * 0.12f, ty - s * 0.14f, s * 0.55f, s * 0.44f,
                 mix(jgDark, jgMid, 0.3f + frand(k, 4, 17) * 0.5f));
        softDisc(tx - s * 0.26f, ty - s * 0.28f, s * 0.24f, s * 0.18f, mix(jgMid, jgHi, 0.5f));
    }
    // sand/weathering bleed from the apron edges (keeps the slab from floating)
    for (int k = 0; k < 14; ++k) {
        const float bx = 30.0f + frand(k, 5, 18) * (W - 60.0f);
        const float by = 24.0f + frand(k, 6, 19) * (H - 48.0f);
        const float qx = std::fabs(bx - cx);
        if (qx > 34.0f && qx < 38.0f) continue;  // keep runway borders clean-ish
        softDisc(bx, by, 3.0f + frand(k, 7, 20) * 4.0f, 2.2f + frand(k, 7, 21) * 3.0f,
                 col(0xA8916A, 90));
    }

    // --- battle damage --------------------------------------------------------------
    auto crater = [&](float sx, float sy, float r) {
        softDisc(sx, sy, r * 1.3f, r * 1.15f, col(0x35332E, 170));   // scorch
        softDisc(sx, sy, r, r * 0.9f, col(0x14120F, 230));           // hole
        softDisc(sx - r * 0.25f, sy - r * 0.3f, r * 0.5f, r * 0.4f, col(0x55524B));
        for (int k = 0; k < 6; ++k) {  // rim rubble
            const float a = frand(k, 30 + static_cast<int>(sx), 7) * 6.2832f;
            softDisc(sx + std::cos(a) * r * 1.15f, sy + std::sin(a) * r * 1.0f,
                     1.6f + frand(k, 31, 8) * 1.4f, 1.2f, col(0x6E6A5E));
        }
    };
    if (damage >= 2) {
        crater(cx - 10.0f, cy - 34.0f, 7.0f);
        crater(cx + 12.0f, cy + 40.0f, 8.0f);
        // burned grass margin (west)
        softDisc(14.0f, cy - 50.0f, 12.0f, 16.0f, col(0x35302A, 160));
        softDisc(14.0f, cy - 50.0f, 7.0f, 10.0f, col(0x1E1B16, 190));
        // shattered parked plane (east apron)
        softDisc(cx + 44.0f, cy + 40.0f, 10.0f, 7.0f, col(0x2A261F, 180));
        for (int k = 0; k < 5; ++k)
            softDisc(cx + 40.0f + frand(k, 40, 9) * 12.0f, cy + 36.0f + frand(k, 41, 10) * 10.0f,
                     1.6f, 1.2f, col(0x4E5346));
    }
    if (damage >= 3) {
        crater(cx + 4.0f, cy - 66.0f, 9.0f);
        crater(cx - 6.0f, cy + 92.0f, 8.0f);
        crater(cx - 70.0f, cy + 10.0f, 6.0f);
        // soot wash over the whole base
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if ((x * 7 + y * 11) % 13 < 4) p.blend(x, y, col(0x171614, 85));
        // runway split: cracked through the asphalt
        for (int y = 0; y < H; ++y) {
            const float jx = cx + 6.0f * std::sin(y * 0.09f + 2.0f);
            for (int d = -1; d <= 1; ++d)
                p.blend(static_cast<int>(jx) + d, y,
                        col(d == 0 ? 0x14120F : 0x35332E, d == 0 ? 230 : 140));
        }
        // dead tree margins
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool edge = x < 26 || x > W - 26;
                if (!edge) continue;
                if ((x * 5 + y * 3) % 11 < 6) p.blend(x, y, col(0x3A362A, 150));
            }
    }
    return p;
}

// Sawn-off arch (Quonset) hangar, seen from above: capsule roof with the
// bright ridge along the spine and corrugation ribs running down the slopes.
PixelBuf makeAirfieldHangar() {
    constexpr int W = 80, H = 56;
    const float cx = W / 2.0f, cy = H / 2.0f;
    PixelBuf p(W, H);
    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    // capsule distance field: segment x in [cx-14, cx+14] at y=cy, radius 20
    auto capsuleD = [&](float x, float y) {
        const float ex = std::fmax(std::fabs(x - cx) - 14.0f, 0.0f);
        const float ey = std::fabs(y - cy);
        return std::sqrt(ex * ex + ey * ey);
    };
    // cast shadow
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float d = capsuleD(x - 3.0f, y - 4.0f);
            if (d < 21.0f) p.blend(x, y, col(0x0A0806, 90));
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float d = capsuleD(static_cast<float>(x), static_cast<float>(y));
            if (d > 20.0f) continue;
            const float slope = d / 20.0f;  // 0 at the ridge, 1 at the eaves
            std::uint32_t c = mix(col(0xA6ABA0), col(0x565B52), slope * slope * 0.75f +
                                  slope * 0.25f);
            // upper-left of the ridge catches the sun
            if (x < cx && y < cy) c = mix(c, col(0xC0C5BA), 0.22f * (1.0f - slope));
            if (x % 3 == 0) c = mix(c, col(0x3E433C), 0.16f);       // corrugation ribs
            const std::uint32_t hh = (static_cast<std::uint32_t>(x * 73856093u) ^
                                      static_cast<std::uint32_t>(y * 19349663u));
            if ((hh >> 8) % 23 == 0) c = mix(c, col(0x7A5A3E), 0.5f);  // rust
            if ((hh >> 12) % 9 == 0) c = mix(c, col(0x5A6848), 0.4f);  // camo spatter
            if (d > 18.6f) c = mix(c, col(0x2E332C), 0.75f);           // eave rim
            p.set(x, y, c);
        }
    // ridge highlight + door seams at the ends
    for (int x = static_cast<int>(cx) - 13; x <= static_cast<int>(cx) + 13; ++x) {
        p.set(x, static_cast<int>(cy), col(0xC6CBBF, 200));
        p.set(x, static_cast<int>(cy) + 1, col(0xB0B5A9, 140));
    }
    for (int yy = static_cast<int>(cy) - 12; yy <= static_cast<int>(cy) + 12; ++yy) {
        if (capsuleD(cx - 30.0f, static_cast<float>(yy)) < 20.0f)
            p.blend(static_cast<int>(cx) - 30, yy, col(0x3A3F38, 190));
        if (capsuleD(cx + 30.0f, static_cast<float>(yy)) < 20.0f)
            p.blend(static_cast<int>(cx) + 30, yy, col(0x3A3F38, 190));
    }
    outlineSilhouette(p, col(0x0E1216, 150));
    return p;
}

// Control tower: concrete bunker base, tapered shaft, glazed octagon cab.
PixelBuf makeAirfieldTower() {
    constexpr int W = 44, H = 48;
    PixelBuf p(W, H);
    auto mix = [](std::uint32_t a, std::uint32_t b, float t) -> std::uint32_t {
        auto ch = [](std::uint32_t v, int s) { return static_cast<float>((v >> s) & 0xFFu); };
        const float r  = ch(a, 24) + (ch(b, 24) - ch(a, 24)) * t;
        const float g  = ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t;
        const float bl = ch(a, 8)  + (ch(b, 8)  - ch(a, 8))  * t;
        const float al = ch(a, 0)  + (ch(b, 0)  - ch(a, 0))  * t;
        return (static_cast<std::uint32_t>(r + 0.5f) << 24) |
               (static_cast<std::uint32_t>(g + 0.5f) << 16) |
               (static_cast<std::uint32_t>(bl + 0.5f) << 8) |
               static_cast<std::uint32_t>(al + 0.5f);
    };
    // shadow
    for (int y = 30; y < 46; ++y)
        for (int x = 10; x < 40; ++x)
            p.blend(x, y, col(0x0A0806, 80));
    // bunker base
    for (int y = 30; y <= 43; ++y) {
        const float t = (y - 30) / 13.0f;
        for (int x = 7; x <= 36; ++x) {
            std::uint32_t c = mix(col(0x9A968A), col(0x6E6A5E), t);
            if (x == 7 || x == 36 || y == 30 || y == 43) c = mix(c, col(0x4A463C), 0.7f);
            if ((x + y) % 9 == 0) c = mix(c, col(0x7E7A6E), 0.5f);
            p.set(x, y, c);
        }
    }
    p.fillRect(12, 34, 6, 4, col(0x3E3A32));   // door
    p.fillRect(27, 34, 6, 4, col(0x3E3A32));
    // shaft
    for (int y = 16; y <= 30; ++y)
        for (int x = 16; x <= 27; ++x) {
            std::uint32_t c = mix(col(0x848A80), col(0x5E645A), (y - 16) / 14.0f);
            if (x == 16 || x == 27) c = mix(c, col(0x3A3F38), 0.6f);
            else if (x <= 19) c = mix(c, col(0x9AA096), 0.35f);   // sun side
            p.set(x, y, c);
        }
    // glazed cab
    for (int y = 6; y <= 19; ++y) {
        const float t = (y - 6) / 13.0f;
        const int cut = (y <= 7 || y >= 18) ? 3 : (y <= 8 || y >= 17 ? 1 : 0);
        for (int x = 6 + cut; x <= 37 - cut; ++x) {
            const bool edge = x <= 6 + cut || x >= 37 - cut || y <= 6 || y >= 19;
            std::uint32_t c = mix(col(0x7E847A), col(0x565C52), t);
            if (edge) c = mix(c, col(0x2E332C), 0.75f);
            p.set(x, y, c);
        }
    }
    p.fillRect(9, 8, 26, 6, col(0x2E4A5E));            // window band
    for (int x = 12; x < 35; x += 6) p.vline(x, 8, 13, col(0x1E3444));  // mullions
    p.fillRect(10, 9, 8, 2, col(0x6E9AB8, 200));       // glass glare
    p.fillRect(22, 9, 4, 1, col(0x6E9AB8, 150));
    p.fillRect(14, 1, 16, 3, col(0x4E544A));           // roof deck
    p.vline(22, 0, 1, col(0x3A3F44));                  // antenna
    p.set(22, 0, col(0xE8E8E2));
    outlineSilhouette(p, col(0x0E1216, 150));
    return p;
}

// Small defensive turret 28x28 for the Sky Fortress (nose/tail/engine
// guns): glass-domed barrel base.
PixelBuf makeTurretSmall() {
    PixelBuf p(28, 28);
    const float cx = 14.0f, cy = 15.0f;
    p.fillEllipse(cx, cy + 1, 12.0f, 11.0f, col(0x24282C));
    p.fillEllipse(cx, cy, 10.5f, 9.5f, col(0x4E5459));
    p.fillEllipse(cx - 1, cy - 1, 7.0f, 6.5f, col(0x666B70));
    p.fillEllipse(cx - 2, cy - 2, 4.0f, 3.5f, col(0x3A5A6E));   // glass dome
    p.set(static_cast<int>(cx) - 3, static_cast<int>(cy) - 3, col(0x9FD0E8));
    // Twin stub barrels (down = forward when on the nose).
    for (int bx : {10, 16}) {
        p.fillRect(bx, 17, 3, 10, col(0x41474D));
        p.fillRect(bx, 17, 1, 10, col(0x9AA2A8));
        p.fillRect(bx + 2, 17, 1, 10, col(0x23272B));
    }
    outlineSilhouette(p, col(0x0E1216, 150));  // shared crisp contour language

    return p;
}

// Fire and smoke puffs for damaged bosses.
PixelBuf makeFirePuff() {
    const int S = 16;
    PixelBuf p(S, S);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const float dx = x + 0.5f - S / 2, dy = y + 0.5f - S / 2;
            const float d = std::sqrt(dx * dx + dy * dy) / (S / 2);
            if (d > 1.0f) continue;
            const std::uint32_t c = d < 0.35f ? col(0xFFF4C0, 255)
                                  : d < 0.65f ? col(0xFFA82A, 235)
                                              : col(0xE8501A, 150);
            p.set(x, y, c);
        }
    return p;
}
PixelBuf makeSmokePuff() {
    const int S = 20;
    PixelBuf p(S, S);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const float dx = x + 0.5f - S / 2, dy = y + 0.5f - S / 2;
            const float d = std::sqrt(dx * dx + dy * dy) / (S / 2);
            if (d > 1.0f) continue;
            const int a = static_cast<int>(110.0f * (1.0f - d) * (1.0f - d));
            p.set(x, y, col(0x3A3A3E, static_cast<std::uint8_t>(a)));
        }
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Generated-art disk cache.
//
// Baking every sprite from vector code is deterministic but costs real time
// (and far more in unoptimised debug builds), so the finished textures are
// dumped once to a compact binary blob and simply uploaded on later
// launches. Bump kArtCacheVersion whenever generated art changes; any
// parse hiccup falls back to a full regeneration, and callers disable the
// cache entirely by passing an empty path.
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kArtCacheMagic = 0x50534152u;  // "PSAR"
// Bump whenever any generated art changes (v10: A6M Zero redesign of the
// primary enemy fighter and interceptor).
constexpr std::uint32_t kArtCacheVersion = 24u;

void wr32(std::ostream& o, std::uint32_t v) {
    o.write(reinterpret_cast<const char*>(&v), 4);
}
bool rd32(std::istream& i, std::uint32_t& v) {
    char b[4];
    if (!i.read(b, 4)) return false;
    v = static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[0])) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[3])) << 24);
    return true;
}

bool loadArtCache(Renderer& r, const std::string& path,
                  const std::function<void(float)>& progress) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::uint32_t magic = 0, version = 0, endian = 0, count = 0;
    if (!rd32(in, magic) || !rd32(in, version) || !rd32(in, endian) ||
        !rd32(in, count))
        return false;
    if (magic != kArtCacheMagic || version != kArtCacheVersion ||
        endian != 0x01020304u || count == 0 || count > 4096)
        return false;
    std::vector<std::uint8_t> px;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t nLen = 0, w = 0, h = 0;
        if (!rd32(in, nLen) || nLen == 0 || nLen > 128) return false;
        std::string name(nLen, '\0');
        if (!in.read(&name[0], nLen)) return false;
        if (!rd32(in, w) || !rd32(in, h) || w == 0 || h == 0 ||
            static_cast<std::uint64_t>(w) * h > 16u * 1024u * 1024u)
            return false;
        px.resize(static_cast<std::size_t>(w) * h * 4);
        if (!in.read(reinterpret_cast<char*>(px.data()),
                     static_cast<std::streamsize>(px.size())))
            return false;
        r.createTextureFromPixels(name, px.data(), static_cast<int>(w),
                                  static_cast<int>(h));
        if (progress && (i % 24) == 0)
            progress(static_cast<float>(i) / static_cast<float>(count));
    }
    return true;
}

void saveArtCache(
    const std::string& path,
    const std::vector<std::pair<std::string, PixelBuf>>& items) {
    if (items.empty()) return;
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    wr32(out, kArtCacheMagic);
    wr32(out, kArtCacheVersion);
    wr32(out, 0x01020304u);  // endian probe, validated on load
    wr32(out, static_cast<std::uint32_t>(items.size()));
    for (const auto& [name, buf] : items) {
        wr32(out, static_cast<std::uint32_t>(name.size()));
        out.write(name.data(), name.size());
        wr32(out, static_cast<std::uint32_t>(buf.w));
        wr32(out, static_cast<std::uint32_t>(buf.h));
        out.write(reinterpret_cast<const char*>(buf.bytes()),
                  static_cast<std::streamsize>(buf.px.size() * 4));
    }
    if (!out.good()) {
        out.close();
        std::filesystem::remove(tmp, ec);
        return;
    }
    out.close();
    std::filesystem::rename(tmp, p, ec);  // atomic: torn files never parse
}

}  // namespace

// ---------------------------------------------------------------------------
bool generateAllArt(Renderer& r, const std::string& cachePath,
                    const std::function<void(float)>& progress) {
    std::vector<std::pair<std::string, PixelBuf>> captured;
    const bool wantCache = !cachePath.empty();
    if (wantCache && loadArtCache(r, cachePath, progress)) {
        // Cached pixels never pass through the sprite builder, but the
        // renderer still needs the enemy prop anchors. Re-deriving them is
        // a handful of tiny 52x52 rasters — milliseconds, not the seconds
        // the full art dump takes.
        buildEnemyPlanes([](const std::string&, const PixelBuf&) {});
        if (progress) progress(1.0f);
        return true;
    }
    auto add = [&](const std::string& name, const PixelBuf& p) {
        r.createTextureFromPixels(name, p.bytes(), p.w, p.h);
        dumpSpriteDbg(name, p);
        if (wantCache) captured.emplace_back(name, p);
    };
    auto step = [&progress](float p) { if (progress) progress(p); };

    // Player: P-38 Lightning; level + banked frames + the spinning prop
    // overlay (rotated by the renderer at each engine anchor).
    {
        const PixelBuf lvl = makeP38(0);
        add("player_fighter", lvl);
        const PixelBuf banked = makeP38(-2);
        add("player_fighter_l", banked);
        PixelBuf right = banked;
        right.mirrorX();
        add("player_fighter_r", right);
        const PixelBuf prop = makePropStreak();
        add("prop_blur_p", prop);
#ifndef NDEBUG
        // Debug hook: WWII_DUMP_SPRITES=<dir> writes the player sprites as
        // PNGs for offline pixel inspection (development tool only).
        if (SDL_getenv("WWII_DUMP_SPRITES")) {
            dumpSpriteDbg("player_fighter", lvl);
            dumpSpriteDbg("player_fighter_l", banked);
            dumpSpriteDbg("player_fighter_r", right);
            dumpSpriteDbg("prop_blur_p", prop);
        }
#endif
    }
    step(0.06f);

    // Enemies (drawn nose-down: generate nose-up then flip). Sprites and
    // their spinning-prop anchors come from one builder so the prop_blur_e
    // overlay anchors always match the finished silhouettes.
    add("prop_blur_e", makePropStreakEnemy());
    buildEnemyPlanes(
        [&](const std::string& name, const PixelBuf& p) { add(name, p); });
#ifndef NDEBUG
    dumpSpriteDbg("prop_blur_e", makePropStreakEnemy());
#endif
    step(0.22f);

    // Ground & sea.
    add("turret_aa", makeAATurret());
    add("tank", makeTank());
    add("bunker", makeBunker());
    add("boat_patrol", makeShip(22, 44, false));
    add("ship_destroyer", makeShip(34, 64, true));
    step(0.32f);

    // Projectiles.
    add("bullet_player", makeBulletPlayer());
    add("bullet_cannon", makeBulletCannon());
    add("bullet_rocket", makeBulletRocket());
    add("bullet_enemy", makeBulletEnemy());
    add("shell_enemy", makeShellEnemy());
    add("flak_enemy", makeFlak());
    step(0.42f);

    // Power-ups.
    add("powerup_p", makePowerupUpgrade());
    add("powerup_spread", makePowerupSpread());
    add("powerup_cannon", makePowerupCannon());
    add("powerup_rocket", makePowerupRocket());
    add("powerup_bomb", makePowerupBomb());
    add("powerup_repair", makePowerupRepair());
    add("powerup_life", makePowerupLife());
    step(0.50f);

    // Effects.
    add("explosion_small", makeBoomSheet(6, 24, 0xE5A1u, 0));
    add("explosion_medium", makeBoomSheet(11, 40, 0xE5B2u, 1));
    add("explosion_large", makeBoomSheet(15, 72, 0xE5C3u, 2));
    add("splash_small", makeBoomSheet(10, 32, 0x5D1Au, 1, true));
    add("splash_large", makeBoomSheet(14, 48, 0x5D2Bu, 2, true));
    add("muzzle_flash", makeMuzzleSheet());
    add("shadow_ell", makeShadow());
    step(0.60f);

    // Tiles.
    add("tile_ocean_a", makeOceanTile(0));
    add("tile_ocean_b", makeOceanTile(1));
    add("tile_ocean_c", makeOceanTile(2));
    add("tile_ocean_glint", makeOceanGlintTile());
    add("tile_ocean_glint_far", makeOceanGlintFarTile());
    add("tile_sand", makeSandTile(0));
    add("tile_jungle_a", makeJungleTile(1));
    add("tile_jungle_b", makeJungleTile(2));
    add("tile_shore", makeShoreTile(true));
    add("tile_shore_in", makeShoreTile(false));
    add("tile_road_v", makeRoadTile(true));
    add("tile_road_h", makeRoadTile(false));
    add("tile_airfield", makeAirfieldTile());
    add("tile_runway", makeRunwayTile());
    add("tile_fortress", makeFortressTile());
    add("tile_island_edge", makeGrassEdgeTile());
    step(0.72f);

    // Boss parts.
    add("boss_battleship_hull", makeBattleshipHull(0));
    add("boss_battleship_hull_light", makeBattleshipHull(1));
    add("boss_battleship_hull_dmg", makeBattleshipHull(2));
    add("boss_battleship_hull_wreck", makeBattleshipHull(3));
    add("boss_battleship_hull_flash", toFlashSilhouette(makeBattleshipHull(0)));
    add("boss_big_turret", makeBigTurret());
    add("boss_aa", makeBossAA());
    add("boss_bridge", makeBridge());
    step(0.82f);
    add("boss_bomber_hull", makeGiantBomberHull(0));
    add("boss_bomber_hull_dmg", makeGiantBomberHull(2));
    add("boss_bomber_hull_flash", toFlashSilhouette(makeGiantBomberHull(0)));
    add("boss_turret_small", makeTurretSmall());
    add("boss_destroyer_hull", makeDestroyerHull(0));
    add("boss_destroyer_hull_light", makeDestroyerHull(1));
    add("boss_destroyer_hull_dmg", makeDestroyerHull(2));
    add("boss_destroyer_hull_wreck", makeDestroyerHull(3));
    add("boss_destroyer_hull_flash", toFlashSilhouette(makeDestroyerHull(0)));
    add("boss_destroyer_gun", makeDestroyerGun());
    add("boss_destroyer_bridge", makeDestroyerBridge());
    step(0.90f);
    add("boss_carrier_hull", makeCarrierHull(0));
    add("boss_carrier_hull_light", makeCarrierHull(1));
    add("boss_carrier_hull_dmg", makeCarrierHull(2));
    add("boss_carrier_hull_wreck", makeCarrierHull(3));
    add("boss_carrier_hull_flash", toFlashSilhouette(makeCarrierHull(0)));
    add("boss_fortress_hull", makeFortressHull(0));
    add("boss_fortress_hull_dmg", makeFortressHull(2));
    add("boss_fortress_hull_wreck", makeFortressHull(3));
    add("boss_fortress_hull_flash", toFlashSilhouette(makeFortressHull(0)));
    add("boss_airfield_hull", makeAirfieldHull(0));
    add("boss_airfield_hull_dmg", makeAirfieldHull(2));
    add("boss_airfield_hull_wreck", makeAirfieldHull(3));
    add("boss_airfield_hull_flash", toFlashSilhouette(makeAirfieldHull(0)));
    add("boss_airfield_hangar", makeAirfieldHangar());
    add("boss_airfield_tower", makeAirfieldTower());
    add("boss_cruiser_hull", makeCruiserHull(0));
    add("boss_cruiser_hull_dmg", makeCruiserHull(2));
    add("boss_cruiser_hull_wreck", makeCruiserHull(3));
    add("boss_cruiser_hull_flash", toFlashSilhouette(makeCruiserHull(0)));
    add("boss_fort_main", makeFortMain());
    add("boss_fort_secondary", makeFortSecondary());
    add("boss_fort_aa", makeFortAa());
    add("boss_fort_keep", makeFortKeep());
    add("boss_carrier_island", makeCarrierIsland());
    add("wake_foam", makeWake());
    add("fire_puff", makeFirePuff());
    add("smoke_puff", makeSmokePuff());
    add("explosion_xlarge", makeBoomSheet(17, 96, 0xE5D4u, 3));
    add("boss_explosion", makeBossExplosionSheet());
    step(1.0f);

    if (wantCache) saveArtCache(cachePath, captured);
    return true;
}

void loadPngOverrides(Renderer& renderer, const std::string& assetDir) {
    namespace fs = std::filesystem;
    static const char* names[] = {
        "player_fighter", "player_fighter_l", "player_fighter_r",
        "enemy_fighter_light", "enemy_interceptor", "enemy_interceptor_m",
        "enemy_heavy_fighter", "enemy_bomber", "turret_aa", "tank", "bunker",
        "enemy_fighter_carrier",
        "boat_patrol", "ship_destroyer", "bullet_player", "bullet_cannon",
        "bullet_rocket", "bullet_enemy", "shell_enemy", "flak_enemy",
        "powerup_p", "powerup_spread", "powerup_cannon", "powerup_rocket",
        "powerup_bomb", "powerup_repair", "powerup_life",
        "explosion_small", "explosion_medium", "explosion_large",
        "explosion_xlarge", "boss_explosion",
        "splash_small", "splash_large", "muzzle_flash",
        "boss_battleship_hull", "boss_battleship_hull_light",
        "boss_battleship_hull_dmg",
        "boss_battleship_hull_wreck", "boss_battleship_hull_flash",
        "boss_big_turret", "boss_aa",
        "boss_bridge", "boss_bomber_hull", "boss_bomber_hull_dmg",
        "boss_bomber_hull_flash",
        "boss_destroyer_hull", "boss_destroyer_hull_light",
        "boss_destroyer_hull_dmg", "boss_destroyer_hull_wreck",
        "boss_destroyer_hull_flash", "boss_destroyer_gun",
        "boss_destroyer_bridge",
        "boss_carrier_hull", "boss_carrier_hull_light", "boss_carrier_hull_dmg",
        "boss_carrier_hull_wreck", "boss_carrier_hull_flash", "boss_carrier_island",
        "boss_fortress_hull", "boss_fortress_hull_dmg",
        "boss_fortress_hull_wreck", "boss_fortress_hull_flash",
        "boss_cruiser_hull", "boss_cruiser_hull_dmg",
        "boss_cruiser_hull_wreck", "boss_cruiser_hull_flash",
        "boss_fort_main", "boss_fort_secondary", "boss_fort_aa",
        "boss_fort_keep",
        "wake_foam", "fire_puff", "smoke_puff",
    };
    for (const char* n : names) {
        const fs::path p = fs::path(assetDir) / "sprites" / (std::string(n) + ".png");
        if (fs::exists(p)) renderer.createTextureFromPng(n, p.string());
    }
    // Tiles directory.
    const fs::path tiles = fs::path(assetDir) / "tiles";
    if (fs::is_directory(tiles)) {
        for (const auto& e : fs::directory_iterator(tiles)) {
            if (e.path().extension() == ".png")
                renderer.createTextureFromPng(
                    e.path().stem().string(), e.path().string());
        }
    }
}

const std::vector<EnemyPropAnchor>* enemyPropAnchors(const std::string& name) {
    const auto it = gEnemyProps.find(name);
    return it == gEnemyProps.end() ? nullptr : &it->second;
}

}  // namespace ww
