#include "graphics/ProceduralArt.h"

#include <cmath>
#include <filesystem>

#include "core/Rng.h"

namespace ww {
namespace {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
const std::uint32_t kDrab = col(0x5A6E3A);
const std::uint32_t kDrabHi = col(0x748C4E);
const std::uint32_t kDrabDk = col(0x3C4A26);
const std::uint32_t kInsigniaBlue = col(0x24407A);
const std::uint32_t kWhite = col(0xF2F2F2);

const std::uint32_t kEnTan = col(0x8A7A4E);
const std::uint32_t kEnTanDk = col(0x66583A);
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
    // Sandbag ring: solid base ring with individual bags on the rim.
    hi.fillEllipse(cx, 21, 15.0f, 12.0f, col(0x54452C));
    hi.fillEllipse(cx, 20, 14.0f, 11.0f, col(0x746240));
    for (int k = 0; k < 10; ++k) {
        const float a = k * 0.6283f + 0.3f;
        const float bx = cx + 13.2f * std::cos(a);
        const float by = 20.0f + 10.3f * std::sin(a);
        hi.fillEllipse(bx, by, 2.9f, 2.3f, (k % 2) ? col(0x8A7A52) : col(0x685838));
        hi.fillEllipse(bx - 0.6f, by - 0.7f, 1.5f, 1.0f,
                       (k % 2) ? col(0x9C8C62) : col(0x7A6A48));   // bag sheen
    }
    // Amber hazard dashes (armored-target language shared with bosses).
    for (int k = 0; k < 16; ++k) {
        if (k % 2) continue;
        const float a = k * 0.3927f + 0.15f;
        hi.fillEllipse(cx + 14.4f * std::cos(a), 20.4f + 11.4f * std::sin(a),
                       0.9f, 0.7f, col(0xC8A83C));
    }
    // Concrete gun platform.
    hi.fillEllipse(cx, 19, 10.2f, 8.2f, col(0x585850));
    hi.fillEllipse(cx, 18, 9.0f, 7.0f, col(0x8C8C80));
    hi.fillEllipse(cx - 2, 15.6f, 5.2f, 3.4f, col(0x9E9E92));      // lit top
    hi.fillEllipse(cx, 20, 4.2f, 3.2f, col(0x666660));             // mount base
    // Armored gun shield with bevel + sight slit.
    hi.fillRoundRect(cx - 7.5f, 8.5f, 15.0f, 6.5f, 1.6f, col(0x6A7076));
    hi.fillRoundRect(cx - 6.6f, 9.2f, 13.2f, 2.4f, 1.0f, col(0x8A9094));
    hi.stroke({{cx - 4.0f, 12.6f}, {cx + 4.0f, 12.6f}}, 1.1f, col(0x181C20));
    // Twin AA barrels with flash hiders.
    for (const float bx : {12.8f, 17.6f}) {
        hi.stroke({{bx, 2.0f}, {bx, 13.5f}}, 1.9f, col(0x14181C));
        hi.stroke({{bx - 0.5f, 2.6f}, {bx - 0.5f, 12.6f}}, 0.55f, col(0x565C62));
        hi.fillRoundRect(bx - 1.3f, 1.2f, 2.6f, 2.4f, 0.7f, col(0x2B2F33));
    }
    // Ready-rack box and ladder.
    hi.fillRoundRect(21.5f, 19.5f, 5.5f, 4.5f, 0.8f, col(0x4A4A40));
    hi.stroke({{22.2f, 20.4f}, {26.2f, 20.4f}}, 0.7f, col(0x7A7A6C));
    hi.stroke({{7.6f, 26.5f}, {10.4f, 26.5f}}, 0.8f, col(0x3A3A32));
    hi.stroke({{7.6f, 29.0f}, {10.4f, 29.0f}}, 0.8f, col(0x3A3A32));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x120E08, 150));
    return p;
}

PixelBuf makeTank() {
    Hi hi(40, 28);
    const float cx = 20.0f;
    // Tracks: capsules with wheel hubs.
    for (const float tx : {6.4f, 33.6f}) {
        hi.fillRoundRect(tx - 3.4f, 3.0f, 6.8f, 22.0f, 3.2f, col(0x1A1D20));
        hi.fillRoundRect(tx - 2.6f, 3.8f, 5.2f, 20.4f, 2.6f, col(0x2C3036));
        for (int k = 0; k < 5; ++k)
            hi.fillEllipse(tx, 6.5f + k * 3.6f, 1.5f, 1.5f, col(0x4A5056));
        hi.stroke({{tx - 2.9f, 5.0f}, {tx - 2.9f, 23.0f}}, 0.6f, col(0x3E444A));
    }
    // Hull: rounded slab with front glacis bevel.
    hi.fillRoundRect(9.5f, 4.0f, 21.0f, 21.0f, 3.4f, col(0x36402A));
    hi.fillRoundRect(10.6f, 5.2f, 18.8f, 17.6f, 2.8f, col(0x4E5A38));
    hi.fillRoundRect(11.8f, 6.2f, 8.0f, 7.0f, 2.0f, col(0x5E6C44));   // lit deck
    hi.stroke({{11.0f, 21.5f}, {29.0f, 21.5f}}, 0.8f, col(0x38422A)); // hull seam
    // Turret + mantlet + barrel pointing down at the player.
    hi.fillEllipse(cx, 12.0f, 7.6f, 6.8f, col(0x38422A));
    hi.fillEllipse(cx - 1.2f, 10.8f, 5.2f, 4.4f, col(0x5C6A42));      // crown light
    hi.fillRoundRect(cx - 2.4f, 15.0f, 4.8f, 3.6f, 1.2f, col(0x46523A));
    hi.stroke({{cx, 16.5f}, {cx, 26.5f}}, 2.1f, col(0x14181C));
    hi.stroke({{cx - 0.55f, 17.5f}, {cx - 0.55f, 25.5f}}, 0.55f, col(0x4E545A));
    hi.fillRoundRect(cx - 1.6f, 25.4f, 3.2f, 2.2f, 0.6f, col(0x38422A));  // muzzle brake
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x101408, 150));
    return p;
}

PixelBuf makeBunker() {
    Hi hi(32, 32);
    // Earth berm + concrete dome.
    hi.fillEllipse(16, 20, 14.5f, 11.5f, col(0x4A4436));
    hi.fillEllipse(16, 18, 12.6f, 10.6f, col(0x6A6A60));
    hi.fillEllipse(16, 16.4f, 11.4f, 9.4f, col(0x8C8C80));
    hi.fillEllipse(12.4f, 12.8f, 5.4f, 3.4f, col(0x9E9E90));          // lit crown
    // Embrasure: dark slot with inner shadow + stub gun.
    hi.fillRoundRect(7.5f, 15.5f, 17.0f, 4.2f, 1.6f, col(0x0E1012));
    hi.fillRoundRect(8.4f, 16.2f, 15.2f, 1.4f, 0.6f, col(0x2A2E32));  // lintel shade
    hi.stroke({{16.0f, 16.6f}, {16.0f, 19.4f}}, 1.5f, col(0x3A4046));
    hi.stroke({{15.6f, 17.0f}, {15.6f, 19.0f}}, 0.5f, col(0x7A8086));
    // Cracked concrete seams + base slab.
    hi.stroke({{6.0f, 24.0f}, {9.5f, 26.5f}}, 0.5f, col(0x6E6E64, 200));
    hi.stroke({{25.0f, 23.0f}, {22.0f, 26.0f}}, 0.5f, col(0x6E6E64, 200));
    hi.fillRoundRect(5.0f, 26.0f, 22.0f, 3.2f, 1.2f, col(0x565650));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x121008, 150));
    return p;
}

PixelBuf makeShip(int w, int h, bool destroyer) {
    Hi hi(w, h);
    const float cx = w * 0.5f;
    // AA hull profile: straight lift sides, fine clipped bow at the bottom.
    auto halfAt = [&](float t) {
        return (w * 0.5f - 2.0f) *
               (t > 0.72f ? std::max(0.05f, 1.0f - (t - 0.72f) * 3.4f) : 1.0f);
    };
    std::vector<std::pair<float, float>> hull;
    for (int i = 0; i <= 14; ++i) hull.push_back({cx - halfAt(i / 14.0f) + 0.4f, 1.0f + (h - 2) * i / 14.0f});
    for (int i = 14; i >= 0; --i) hull.push_back({cx + halfAt(i / 14.0f), 1.0f + (h - 2) * i / 14.0f});
    hi.fillPoly(hull, col(0x565D64));
    // Belt shading + deck inset.
    std::vector<std::pair<float, float>> deck;
    for (int i = 0; i <= 12; ++i) {
        const float t = 0.08f + 0.86f * i / 12.0f;
        deck.push_back({cx - halfAt(t) + 3.0f, 1.0f + (h - 2) * t});
    }
    for (int i = 12; i >= 0; --i) {
        const float t = 0.08f + 0.86f * i / 12.0f;
        deck.push_back({cx + halfAt(t) - 3.4f, 1.0f + (h - 2) * t});
    }
    hi.fillPoly(deck, col(0x6A7178));
    hi.stroke({{cx - halfAt(0.5f) + 1.2f, 3.0f}, {cx - halfAt(0.5f) + 1.2f, h - 6.0f}},
              1.2f, col(0x7C848B));                                  // lit flank
    hi.stroke({{cx + halfAt(0.5f) - 1.2f, 3.0f}, {cx + halfAt(0.5f) - 1.2f, h - 6.0f}},
              1.2f, col(0x3A4046));                                  // shaded flank
    // Deck structures: bridge block with windows.
    hi.fillRoundRect(cx - 6.5f, h * 0.5f - 8.0f, 13.0f, 16.0f, 1.8f, col(0x7E858C));
    hi.fillRoundRect(cx - 4.5f, h * 0.5f - 6.0f, 9.0f, 10.0f, 1.2f, col(0x41474D));
    hi.fillRoundRect(cx - 3.6f, h * 0.5f - 5.2f, 7.2f, 2.0f, 0.6f, col(0x3A5A6E));
    // Gun turrets (barrels face the player).
    auto turret = [&](float tx, float ty) {
        hi.fillEllipse(tx, ty, 3.8f, 3.8f, col(0x33383D));
        hi.fillEllipse(tx - 0.5f, ty - 0.5f, 2.4f, 2.4f, col(0x5E656C));
        hi.stroke({{tx, ty + 1.5f}, {tx, ty + 6.5f}}, 1.5f, col(0x14181C));
    };
    turret(cx, h - 9.0f);
    if (destroyer) { turret(cx, 9.0f); turret(cx - 10.0f, h * 0.5f); turret(cx + 10.0f, h * 0.5f); }
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x10141A, 150));
    return p;
}

// ---------------------------------------------------------------------------
// Power-ups / bullets / misc
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Power-up icons (pass 2): each pickup is a distinct object silhouette —
// bold fills, dark edges, top-left shine — instead of a lettered badge.
// Canvas stays 22x22; core mass stays <= ~14px so the HUD bomb counter
// (14px pitch) does not overlap.
// ---------------------------------------------------------------------------

// Weapon upgrade: brass bullet riding up through a green chevron arrow.
PixelBuf makePowerupUpgrade() {
    PixelBuf p(22, 22);
    p.fillEllipse(12.5f, 14.0f, 8.5f, 8.0f, col(0x000000, 60));   // soft shadow
    // Green up-chevron, 2px thick, lit on the outer tips.
    const std::uint32_t gr = col(0x38A038), grHi = col(0x8CD868), grDk = col(0x1E6E22);
    for (int r = 0; r <= 4; ++r)
        for (int sx : {-1, 1}) {
            p.set(11 + sx * r, 3 + r, r == 0 ? grHi : gr);
            p.set(11 + sx * r, 4 + r, grDk);
        }
    // Bullet along the shaft: copper tip, brass body, lit left rail.
    p.fillEllipse(11.0f, 10.5f, 2.4f, 2.6f, col(0xE8A040));
    p.set(10, 9, col(0xF8E0A8));
    p.fillRect(9, 12, 5, 6, col(0xC8902C));
    p.vline(9, 12, 17, col(0xE8C060));
    p.vline(13, 12, 17, col(0x8A6420));
    p.fillRect(9, 17, 5, 1, col(0x6E4E18));                        // base rim
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Spread gun: three bullets fanning out of a steel hub.
PixelBuf makePowerupSpread() {
    PixelBuf p(22, 22);
    const double pi = 3.14159265358979;
    auto bullet = [&](double angDeg) {
        const double a = angDeg * pi / 180.0;
        const double dx = std::sin(a), dy = -std::cos(a);
        const double nx = -dy, ny = dx;                             // perpendicular
        for (double t = 1.0; t <= 12.0; t += 0.5) {
            const double x = 11.0 + dx * t, y = 17.0 + dy * t;
            for (double w = -1.5; w <= 1.5; w += 0.5) {
                std::uint32_t c;
                if (t > 9.8) c = w <= 0 ? col(0xF0E0B0) : col(0xC07828);   // copper tip
                else if (w < -0.5) c = col(0x1A1A1A);                      // outline
                else if (w < 0.5) c = w < 0 ? col(0xE8C060) : col(0xC8902C);
                else c = col(0x1A1A1A);                                    // outline
                p.set(static_cast<int>(std::lround(x + nx * w)),
                      static_cast<int>(std::lround(y + ny * w)), c);
            }
        }
    };
    bullet(-34.0); bullet(0.0); bullet(34.0);
    // Hub plate with the classic magenta supply mark.
    p.fillEllipse(11.0f, 17.0f, 4.5f, 4.0f, col(0x1A1A1A));
    p.fillEllipse(11.0f, 16.5f, 3.5f, 3.0f, col(0x7C838A));
    p.fillEllipse(11.0f, 16.0f, 2.0f, 1.8f, col(0xC868B8));
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Heavy cannon: fat armour-piercing shell, copper driving band, iron nose.
PixelBuf makePowerupCannon() {
    PixelBuf p(22, 22);
    p.fillEllipse(12.0f, 12.0f, 7.0f, 8.5f, col(0x000000, 60));    // soft shadow
    p.fillEllipse(11.0f, 10.0f, 6.0f, 8.0f, col(0x1A1D20));        // silhouette
    p.fillEllipse(11.0f, 9.5f, 5.0f, 7.0f, col(0x6E757C));         // steel body
    p.fillEllipse(11.0f, 6.0f, 3.5f, 3.5f, col(0x9AA2A8));         // hardened nose
    p.set(9, 5, col(0xE8EEF2));                                    // nose shine
    p.fillRect(6, 10, 10, 3, col(0xB87830));                       // copper band
    p.vline(6, 10, 12, col(0xE0A858));
    p.fillRect(6, 13, 10, 1, col(0x7A4E1C));                       // band seam
    p.fillRect(8, 14, 6, 3, col(0x565C61));                        // tapered base
    p.vline(15, 8, 13, col(0x2A2E33));                             // dark edge
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Rocket: red nose, grey body, swept fins, live exhaust flame.
PixelBuf makePowerupRocket() {
    PixelBuf p(22, 22);
    // Nose cone.
    for (int y = 2; y <= 4; ++y) {
        const int hw = y - 2;
        for (int x = 11 - hw; x <= 11 + hw; ++x)
            p.set(x, y, x < 11 ? col(0xE85A48) : (x > 11 ? col(0x8A2418) : col(0xC23A2A)));
    }
    p.fillRect(9, 5, 5, 2, col(0xC23A2A));
    p.vline(9, 5, 6, col(0xE85A48));
    p.vline(13, 5, 6, col(0x8A2418));
    // Body.
    p.fillRect(9, 7, 5, 9, col(0xD8DCE0));
    p.vline(9, 7, 15, col(0xF4F6F8));
    p.vline(13, 7, 15, col(0x9AA0A6));
    p.fillRect(9, 8, 5, 1, col(0x3A5A6E));                         // cockpit band
    // Swept fins.
    for (int i = 0; i < 4; ++i) {
        p.set(8 - i, 12 + i, col(0x8A2A2A)); p.set(7 - i, 12 + i, col(0x1A1A1A));
        p.set(14 + i, 12 + i, col(0x8A2A2A)); p.set(15 + i, 12 + i, col(0x1A1A1A));
    }
    p.fillRect(10, 15, 2, 1, col(0x565C61));                       // nozzle
    // Exhaust: white core, gold, then orange.
    p.fillRect(10, 16, 2, 2, col(0xFFF4C8));
    p.fillRect(9, 18, 4, 2, col(0xFFB028));
    p.set(10, 20, col(0xF06820, 220)); p.set(11, 20, col(0xF06820, 220));
    p.set(11, 21, col(0xF06820, 120));
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Loop bomb: falling aerial bomb, steel-blue body, tail fins, yellow cap.
PixelBuf makePowerupBomb() {
    PixelBuf p(22, 22);
    // Tail assembly (top).
    p.fillRect(9, 1, 4, 1, col(0xC8A83C));                         // service cap
    p.fillRect(8, 2, 6, 3, col(0x2E3338));
    p.vline(11, 2, 4, col(0x6E757C));
    p.set(7, 3, col(0x2E3338)); p.set(14, 3, col(0x2E3338));       // fin tips
    // Body with dark outline ring so it reads over open water.
    p.fillEllipse(11.0f, 10.0f, 5.0f, 6.5f, col(0x14181E));
    p.fillEllipse(11.0f, 10.0f, 4.0f, 5.5f, col(0x5A7EC0));
    p.vline(8, 7, 13, col(0x9AC0E8));                              // lit edge
    p.vline(13, 6, 14, col(0x2E4468));                             // shaded edge
    p.set(9, 7, col(0xF0F6FF));                                    // shine
    // Nose cone (pointing down = falling).
    for (int i = 0; i < 4; ++i) {
        const int hw = std::max(0, 3 - i);
        for (int x = -hw; x <= hw; ++x)
            p.set(11 + x, 15 + i, x <= 0 ? col(0x4A6AA8) : col(0x2E4468));
    }
    p.set(11, 19, col(0x14181E));                                  // fuse tip
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Repair: white first-aid crate with red cross, handle and latches.
PixelBuf makePowerupRepair() {
    PixelBuf p(22, 22);
    p.fillRect(6, 18, 12, 2, col(0x000000, 60));                   // ground shadow
    // Carry handle.
    p.fillRect(9, 3, 5, 3, col(0x3A4046));
    p.fillRect(10, 4, 3, 2, col(0x10141A));
    // Case.
    p.fillRect(4, 5, 14, 12, col(0x1A1A1A));                       // outline
    p.fillRect(5, 6, 12, 10, col(0xE8E8E0));
    p.fillRect(5, 6, 12, 1, col(0xFFFFF8));                        // lid shine
    p.fillRect(5, 9, 12, 1, col(0xB0B0A8));                        // lid seam
    p.fillRect(5, 15, 12, 1, col(0x909088));                       // base shade
    p.fillRect(6, 10, 1, 5, col(0xFFFFF8));                        // lit corner
    // Red cross.
    p.fillRect(10, 10, 3, 5, col(0xC23A2A));
    p.fillRect(8, 11, 7, 3, col(0xC23A2A));
    p.fillRect(10, 14, 3, 1, col(0x8A2418));                       // cross shade
    p.fillRect(13, 11, 2, 1, col(0x8A2418));
    // Latches on the seam.
    p.fillRect(7, 9, 1, 2, col(0x60605A));
    p.fillRect(15, 9, 1, 2, col(0x60605A));
    outlineSilhouette(p, col(0x141018, 160));  // shared crisp contour language

    return p;
}

// Extra life: miniature golden fighter, nose-up like the player.
PixelBuf makePowerupLife() {
    PixelBuf p(22, 22);
    // Prop.
    p.fillRect(7, 1, 8, 1, col(0x3A4046));
    p.set(11, 1, col(0x101010));
    // Wings with dark leading edge and white star dots.
    p.fillRect(2, 9, 18, 3, col(0xD8B040));
    p.fillRect(2, 9, 18, 1, col(0x8A6420));
    p.fillRect(2, 11, 18, 1, col(0xF4DC88));
    p.set(4, 10, kWhite); p.set(18, 10, kWhite);
    // Fuselage.
    p.fillRect(9, 3, 4, 14, col(0xF0D878));
    p.vline(9, 3, 16, col(0xFFF4B0));
    p.vline(12, 3, 16, col(0xB88E28));
    p.fillRect(10, 3, 2, 2, col(0x8A6420));                        // spinner cowl
    p.fillRect(10, 7, 2, 3, col(0x3A5A6E));                        // canopy
    // Tailplane + rudder.
    p.fillRect(6, 15, 10, 2, col(0xD8B040));
    p.fillRect(6, 15, 10, 1, col(0x8A6420));
    p.fillRect(10, 17, 2, 1, col(0xB88E28));
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

PixelBuf makeOceanTile(int variant) {
    // Two-scale wave texture over a smooth low-frequency depth field, so the
    // tiled sea never reads as flat repeating wallpaper. All features wrap
    // modulo 32 to keep the tile seamless.
    PixelBuf p(32, 32);
    Rng rng(0x0CE4Au + variant * 77);
    auto wset = [&](int x, int y, std::uint32_t c) {
        p.set(((x % 32) + 32) % 32, ((y % 32) + 32) % 32, c);
    };
    // Base + seamless depth shading: two sine sweeps + lattice noise.
    const float ph = variant * 1.7f;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float w1 = std::sin((x * 0.3927f) + (y * 0.1963f) + ph) * 0.5f + 0.5f;
            const float n = tileNoise(x + variant * 13, y, 0x5EA0u + variant);
            const float v = (w1 * 0.35f + n * 0.65f - 0.5f) * 0.22f;
            p.set(x, y, shadeBy(col(0x173C6C), v));
        }
    }
    // Long swell lines with soft crests and troughs.
    const int swells = 2 + (variant % 2);
    for (int i = 0; i < swells; ++i) {
        const int x = rng.rangeInt(0, 24), y0 = rng.rangeInt(0, 30);
        const int len = rng.rangeInt(12, 20);
        for (int q = 0; q < len; ++q) {
            const int yy = y0 + (q / 4) % 2;
            wset(x + q, yy, col(0x2A568F));
            wset(x + q, yy - 1, col(0x41729F));
            wset(x + q, yy + 2, col(0x113260));
        }
    }
    // Short ripple dashes.
    for (int i = 0; i < 6; ++i) {
        const int x = rng.rangeInt(0, 28), y = rng.rangeInt(0, 31);
        wset(x, y, col(0x2E5A92));
        wset(x + 1, y, col(0x2E5A92));
        wset(x + 2, y - 1, col(0x2E5A92));
    }
    // Fine speckle kept subtle (noise field already carries the mid-tones).
    for (int i = 0; i < 14; ++i)
        wset(rng.rangeInt(0, 31), rng.rangeInt(0, 31),
             rng.chance(0.5f) ? col(0x1F487E) : col(0x123462));
    // Specular sparkles.
    const int sparkles = 2 + variant * 2;
    for (int i = 0; i < sparkles; ++i)
        wset(rng.rangeInt(0, 31), rng.rangeInt(0, 31),
             col(0x6FA0CC, static_cast<std::uint8_t>(rng.rangeInt(150, 220))));
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
bool generateAllArt(Renderer& r) {
    auto add = [&](const std::string& name, const PixelBuf& p) {
        r.createTextureFromPixels(name, p.bytes(), p.w, p.h);
    };

    // Player: level + banked frames.
    {
        const PlaneStyle ps{44, 38, kDrab, kDrabHi, kDrabDk, false, false};
        PixelBuf lvl = cropToContent(makePlane(ps, 0));
        // Add white star insignia to the player's wings.
        auto star = [&](int mx, int my) {
            lvl.fillEllipse(static_cast<float>(mx), static_cast<float>(my), 4.0f, 4.0f, kInsigniaBlue);
            lvl.hline(mx - 3, mx + 3, my, kWhite);
            lvl.vline(mx, my - 3, my + 3, kWhite);
        };
        star(9, lvl.h / 2 + 1);
        star(lvl.w - 10, lvl.h / 2 + 1);
        add("player_fighter", lvl);
        add("player_fighter_l", cropToContent(makePlane(ps, -2)));
        auto right = cropToContent(makePlane(ps, -2));
        right.mirrorX();
        add("player_fighter_r", right);
    }

    // Enemies (drawn nose-down: generate nose-up then flip).
    auto addDown = [&](const std::string& name, PlaneStyle st, int bank = 0) {
        st.redDot = true;
        PixelBuf p = cropToContent(makePlane(st, bank));
        flipY(p);
        add(name, p);
    };
    addDown("enemy_fighter_light", {26, 22, kEnTan, shade(kEnTan, 1.18f), kEnTanDk});
    {
        auto p = cropToContent(makePlane({22, 22, kEnYellow, shade(kEnYellow, 1.2f), shade(kEnYellow, 0.7f)}));
        flipY(p);
        auto mirror = p; mirror.mirrorX();
        add("enemy_interceptor", p);
        add("enemy_interceptor_m", mirror);
    }
    {
        PlaneStyle st{40, 30, kEnGreen, shade(kEnGreen, 1.2f), kEnGreenDk, true};
        st.amberMark = true;   // elite heavy fighter
        addDown("enemy_heavy_fighter", st);
    }
    {
        // Carrier escort fighter: steel-blue twin with crimson nose band and
        // bolt-on root armour — visibly tougher than anything it escorts.
        PlaneStyle st{34, 26, col(0x55606D), col(0x707C8A), col(0x39424D), true};
        st.redDot = true;
        PixelBuf pf = makePlane(st, 0);
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
        PixelBuf p = cropToContent(pf);
        flipY(p);
        add("enemy_fighter_carrier", p);
    }
    addDown("enemy_bomber", {56, 40, kEnGrey, shade(kEnGrey, 1.2f), kEnGreyDk, true});

    // Ground & sea.
    add("turret_aa", makeAATurret());
    add("tank", makeTank());
    add("bunker", makeBunker());
    add("boat_patrol", []() { auto p = makeShip(44, 20, false); flipY(p); return p; }());
    add("ship_destroyer", makeShip(64, 30, true));

    // Projectiles.
    add("bullet_player", makeBulletPlayer());
    add("bullet_cannon", makeBulletCannon());
    add("bullet_rocket", makeBulletRocket());
    add("bullet_enemy", makeBulletEnemy());
    add("shell_enemy", makeShellEnemy());
    add("flak_enemy", makeFlak());

    // Power-ups.
    add("powerup_p", makePowerupUpgrade());
    add("powerup_spread", makePowerupSpread());
    add("powerup_cannon", makePowerupCannon());
    add("powerup_rocket", makePowerupRocket());
    add("powerup_bomb", makePowerupBomb());
    add("powerup_repair", makePowerupRepair());
    add("powerup_life", makePowerupLife());

    // Effects.
    add("explosion_small", makeBoomSheet(6, 24, 0xE5A1u, 0));
    add("explosion_medium", makeBoomSheet(11, 40, 0xE5B2u, 1));
    add("explosion_large", makeBoomSheet(15, 72, 0xE5C3u, 2));
    add("splash_small", makeBoomSheet(10, 32, 0x5D1Au, 1, true));
    add("splash_large", makeBoomSheet(14, 48, 0x5D2Bu, 2, true));
    add("muzzle_flash", makeMuzzleSheet());
    add("shadow_ell", makeShadow());

    // Tiles.
    add("tile_ocean_a", makeOceanTile(0));
    add("tile_ocean_b", makeOceanTile(1));
    add("tile_ocean_c", makeOceanTile(2));
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

    // Boss parts.
    add("boss_battleship_hull", makeBattleshipHull(0));
    add("boss_battleship_hull_light", makeBattleshipHull(1));
    add("boss_battleship_hull_dmg", makeBattleshipHull(2));
    add("boss_battleship_hull_wreck", makeBattleshipHull(3));
    add("boss_battleship_hull_flash", toFlashSilhouette(makeBattleshipHull(0)));
    add("boss_big_turret", makeBigTurret());
    add("boss_aa", makeBossAA());
    add("boss_bridge", makeBridge());
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
    add("boss_carrier_hull", makeCarrierHull(0));
    add("boss_carrier_hull_light", makeCarrierHull(1));
    add("boss_carrier_hull_dmg", makeCarrierHull(2));
    add("boss_carrier_hull_wreck", makeCarrierHull(3));
    add("boss_carrier_hull_flash", toFlashSilhouette(makeCarrierHull(0)));
    add("boss_carrier_island", makeCarrierIsland());
    add("wake_foam", makeWake());
    add("fire_puff", makeFirePuff());
    add("smoke_puff", makeSmokePuff());
    add("explosion_xlarge", makeBoomSheet(17, 96, 0xE5D4u, 3));
    add("boss_explosion", makeBossExplosionSheet());

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

}  // namespace ww
