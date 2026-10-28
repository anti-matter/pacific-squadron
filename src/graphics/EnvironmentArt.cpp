#include "graphics/EnvironmentArt.h"

#include <cmath>
#include <vector>

#include "graphics/SpriteKit.h"

// ---------------------------------------------------------------------------
// Environment art: the landscape composition primitives.
//
// Everything here is drawn with the shared Hi supersampled rasterizer
// (SpriteKit.h), so trees, huts and tanks get smooth organic contours and
// anti-aliased silhouettes rather than pixel-grid staircases. The palette is
// deliberately low in saturation and contrast: scenery must read clearly as
// *ground*, never as a target (bright metal + flash pads are reserved for
// actual gameplay objects). Light comes from the top-left; every asset bakes
// its own soft cast shadow toward the bottom-right.
// ---------------------------------------------------------------------------
namespace ww {
namespace {

using kit::Hi;
using kit::mix24;      // raw 0xRRGGBB operands
using kit::mixPacked;  // packed 0xRRGGBBAA operands
using kit::outlineSilhouette;
using kit::shade;
using kit::softBlendEllipse;

// Deterministic float in [0,1) for (a,b,seed) — splitmix-style hash.
float fr(int a, int b, int seed) {
    std::uint64_t v = 0x9E3779B97F4A7C15ull ^
                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) * 0x9E3779B1ull) ^
                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)) * 0x85EBCA77ull) ^
                      (static_cast<std::uint64_t>(seed) * 0xC2B2AE3Dull);
    v ^= v >> 30; v *= 0xBF58476D1CE4E5B9ull;
    v ^= v >> 27; v *= 0x94D049BB133111EBull;
    v ^= v >> 31;
    return static_cast<float>(v >> 40) / 16777216.0f;
}

// Muted scenery tint: desaturate a hex color toward its own luminance.
std::uint32_t mute(std::uint32_t rgb, float amount) {
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);
    const int lum = (r * 30 + g * 59 + b * 11) / 100;
    // Both operands are packed 0xRRGGBBAA values here (raw mix24 would read
    // the channels one byte too high).
    return kit::mixPacked(col(rgb),
                          col(static_cast<std::uint32_t>((lum << 16) | (lum << 8) | lum)),
                          amount);
}

// Scenery palette (all pre-muted; keep interactive-entity contrast above).
namespace pal {
// Vegetation greens (cool to warm spread, never neon).
const std::uint32_t leafDk = mute(0x2C4520, 0.22f);
const std::uint32_t leafMid = mute(0x3E5C2A, 0.22f);
const std::uint32_t leafLo = mute(0x587A3C, 0.22f);
const std::uint32_t leafHi = mute(0x7A9A52, 0.25f);
const std::uint32_t jungleDk = mute(0x1F3618, 0.20f);
const std::uint32_t jungleMid = mute(0x31491F, 0.20f);
const std::uint32_t jungleHi = mute(0x4A6631, 0.22f);
const std::uint32_t dryLeaf = mute(0x6B6F35, 0.30f);
// Trunk / wood / thatch browns.
const std::uint32_t bark = mute(0x5A4430, 0.25f);
const std::uint32_t barkDk = mute(0x3E2E20, 0.25f);
const std::uint32_t wood = mute(0x8A6C44, 0.28f);
// Roof families (kept dull: no saturated reds).
const std::uint32_t roofWarm = mute(0x8A5238, 0.38f);
const std::uint32_t roofWarmDk = mute(0x5E3624, 0.38f);
const std::uint32_t roofSlate = mute(0x5C6470, 0.30f);
const std::uint32_t roofSlateDk = mute(0x3E444E, 0.30f);
const std::uint32_t roofThatch = mute(0x9A8454, 0.34f);
const std::uint32_t roofThatchDk = mute(0x6E5C38, 0.34f);
const std::uint32_t roofMetal = mute(0x7E8479, 0.26f);
const std::uint32_t roofMetalDk = mute(0x565B52, 0.26f);
const std::uint32_t roofOlive = mute(0x5E6448, 0.30f);
const std::uint32_t roofOliveDk = mute(0x40462F, 0.30f);
// Walls / small details.
const std::uint32_t wallPale = mute(0xB8AE98, 0.22f);
const std::uint32_t ink = mute(0x20241C, 0.0f);
const std::uint32_t glass = mute(0x39434F, 0.10f);
const std::uint32_t stone = mute(0x8B8578, 0.25f);
const std::uint32_t stoneDk = mute(0x5F5A4E, 0.25f);
const std::uint32_t hay = mute(0xA8874A, 0.28f);
const std::uint32_t hayHi = mute(0xC8A964, 0.28f);
const std::uint32_t water = mute(0x2E5570, 0.25f);
const std::uint32_t waterHi = mute(0x5B87A2, 0.25f);
const std::uint32_t mud = mute(0x4E4232, 0.30f);
}  // namespace pal

// Cast-shadow ellipse under an asset (blended after bake so it stays soft).
void castShadow(PixelBuf& p, float cx, float cy, float rx, float ry) {
    softBlendEllipse(p, cx, cy, rx, ry, 0x0A1410, 88);
}

// ---------------------------------------------------------------------------
// Trees. A canopy is a ring of lobes (organic, never a plain disc) with a
// darker underside crescent to the bottom-right, a lit crown to the
// top-left, and a few brighter speckle flecks for leaf texture.
// ---------------------------------------------------------------------------
PixelBuf makeBroadleaf(int size, int variant) {
    const int w = size + 6, h = size + 8;
    Hi hi(w, h);
    const float cx = w * 0.5f - 1.0f, cy = h * 0.5f - 1.0f;
    const float R = size * 0.5f;
    const std::uint32_t base =
        variant == 0 ? pal::leafMid : (variant == 1 ? pal::jungleMid : pal::dryLeaf);
    const std::uint32_t dark = shade(base, 0.62f);
    const std::uint32_t lit = shade(base, 1.28f);
    const std::uint32_t lit2 = shade(base, 1.55f);
    // Lobed canopy: 6-8 overlapping discs on a lightly elliptical ring.
    const int lobes = 6 + variant;
    for (int i = 0; i < lobes; ++i) {
        const float a = 6.2831853f * i / lobes + fr(i, variant, 3) * 0.5f;
        const float rr = R * (0.55f + fr(i, variant, 9) * 0.20f);
        const float d = R * (0.42f + fr(i, variant, 17) * 0.16f);
        hi.fillEllipse(cx + std::cos(a) * d, cy + std::sin(a) * d * 0.92f, rr, rr * 0.92f,
                       mixPacked(base, dark, fr(i, variant, 23) * 0.45f));
    }
    hi.fillEllipse(cx, cy, R * 0.78f, R * 0.72f, base);
    // Underside shade crescent (bottom-right).
    hi.fillEllipse(cx + R * 0.22f, cy + R * 0.26f, R * 0.70f, R * 0.62f,
                   mixPacked(base, dark, 0.55f));
    // Lit crown (top-left) in two tones for depth.
    hi.fillEllipse(cx - R * 0.26f, cy - R * 0.30f, R * 0.52f, R * 0.46f, lit);
    hi.fillEllipse(cx - R * 0.34f, cy - R * 0.38f, R * 0.28f, R * 0.25f, lit2);
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x131F10, 150));
    castShadow(p, cx + 2.5f + R * 0.10f, cy + 3.5f + R * 0.14f, R * 0.94f, R * 0.70f);
    // Leaf flecks: a few brighter/darker dots on the lit side.
    for (int i = 0; i < size / 3; ++i) {
        const float a = fr(i, size, 31) * 6.2831853f;
        const float d = fr(i, size, 37) * R * 0.75f;
        const int fx = static_cast<int>(cx + std::cos(a) * d - R * 0.15f);
        const int fy = static_cast<int>(cy + std::sin(a) * d * 0.9f - R * 0.15f);
        p.set(fx, fy, fr(i, size, 41) > 0.5f ? lit2 : dark);
    }
    return p;
}

PixelBuf makeConifer(int size, int variant) {
    // Dark conifer: three stacked lobed tiers, tighter silhouette.
    const int w = size + 5, h = size + 7;
    Hi hi(w, h);
    const float cx = w * 0.5f - 0.5f, cy = h * 0.5f;
    const float R = size * 0.5f;
    const std::uint32_t base = variant == 0 ? pal::jungleDk : pal::leafDk;
    const std::uint32_t lit = shade(base, 1.35f);
    // Tier discs from bottom (widest) to top.
    for (int t = 0; t < 3; ++t) {
        const float ty = cy + R * (0.55f - t * 0.55f);
        const float tr = R * (1.0f - t * 0.28f);
        const int lobes = 5;
        for (int i = 0; i < lobes; ++i) {
            const float a = 6.2831853f * i / lobes + t * 0.7f;
            hi.fillEllipse(cx + std::cos(a) * tr * 0.55f, ty + std::sin(a) * tr * 0.30f,
                           tr * 0.55f, tr * 0.45f,
                           mixPacked(base, lit, t * 0.25f + fr(i, t, variant) * 0.2f));
        }
    }
    hi.fillEllipse(cx + R * 0.18f, cy + R * 0.25f, R * 0.60f, R * 0.45f,
                   mixPacked(base, pal::ink, 0.30f));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x101A0E, 150));
    castShadow(p, cx + 2.5f, cy + 3.5f, R * 0.9f, R * 0.55f);
    return p;
}

PixelBuf makePalm(int variant) {
    const int w = 26, h = 26;
    Hi hi(w, h);
    const float cx = 12.5f, cy = 13.0f;
    // Fronds: 7 long leaf shapes radiating from the crown, each a pointed
    // triangle with a midrib; older fronds lie flat, the centre plume is up.
    const int n = 7;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * i / n + (variant ? 0.45f : 0.12f) +
                        fr(i, variant, 5) * 0.22f;
        const float len = 9.0f + fr(i, variant, 7) * 2.5f;
        const float tx = cx + std::cos(a) * len, ty = cy + std::sin(a) * len * 0.94f;
        const float px = -std::sin(a), py = std::cos(a) * 0.94f;
        const float wdt = 2.2f + fr(i, variant, 11) * 0.9f;
        const std::uint32_t cA = i % 2 ? pal::jungleMid : pal::leafMid;
        hi.fillPoly({{cx + px * wdt, cy + py * wdt},
                     {cx - px * wdt, cy - py * wdt},
                     {tx, ty}},
                    cA);
        hi.strokeLine(cx, cy, tx, ty, 0.55f, shade(cA, 0.72f));
        // Lit upper edge on fronds pointing to the sun side.
        if (std::cos(a) + std::sin(a) < -0.2f)
            hi.strokeLine(cx + px * wdt * 0.6f, cy + py * wdt * 0.6f, tx, ty, 0.4f,
                          pal::jungleHi);
    }
    // Crown: small trunk disc + bright plume centre + coconuts.
    hi.fillEllipse(cx, cy, 2.4f, 2.4f, pal::bark);
    hi.fillEllipse(cx - 0.5f, cy - 0.5f, 1.3f, 1.3f, pal::leafHi);
    hi.fillEllipse(cx + 1.6f, cy + 1.2f, 0.8f, 0.8f, pal::barkDk);
    hi.fillEllipse(cx - 1.8f, cy + 1.4f, 0.8f, 0.8f, pal::barkDk);
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x111C0E, 130));
    castShadow(p, cx + 3.0f, cy + 3.5f, 9.0f, 6.5f);
    return p;
}

PixelBuf makeBush(int variant) {
    const int w = variant ? 13 : 10, h = w + 1;
    Hi hi(w, h);
    const float cx = w * 0.5f, cy = h * 0.5f;
    const std::uint32_t base = variant ? pal::leafLo : pal::leafMid;
    const int lobes = 4;
    for (int i = 0; i < lobes; ++i) {
        const float a = 6.2831853f * i / lobes + fr(i, variant, 13) * 0.6f;
        hi.fillEllipse(cx + std::cos(a) * 2.0f, cy + std::sin(a) * 1.6f, 3.0f, 2.6f,
                       mixPacked(base, shade(base, 0.7f), fr(i, variant, 5) * 0.5f));
    }
    hi.fillEllipse(cx - 1.2f, cy - 1.2f, 2.2f, 1.8f, shade(base, 1.3f));
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x141F0F, 130));
    castShadow(p, cx + 1.8f, cy + 2.2f, 4.4f, 2.8f);
    return p;
}

// One 32px hedgerow segment: an uneven low berm of leaf blobs so hedgerows
// read as continuous soft lines when the baker strokes them along a path.
PixelBuf makeHedgerow(int variant) {
    const int w = 32, h = 9;
    Hi hi(w, h);
    const std::uint32_t base = variant ? pal::leafDk : pal::jungleDk;
    const std::uint32_t lit = shade(base, 1.45f);
    for (int i = 0; i < 9; ++i) {
        const float x = i * 3.6f + fr(i, variant, 3) * 2.0f;
        const float y = 4.5f + fr(i, variant, 7) * 2.0f - 1.0f;
        const float r = 2.6f + fr(i, variant, 11) * 1.6f;
        hi.fillEllipse(x, y, r, r * 0.82f, mixPacked(base, lit, fr(i, variant, 17) * 0.4f));
    }
    hi.fillEllipse(8.0f, 3.2f, 6.0f, 2.2f, lit);   // sunlit top strip
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x0F1A0B, 120));
    castShadow(p, 16.0f + 1.6f, 6.6f, 15.5f, 2.6f);
    return p;
}

// ---------------------------------------------------------------------------
// Buildings. Top-down / slight 3/4: roof planes dominate, with a sliver of
// wall visible on the bottom/right faces, a baked drop shadow, and a crisp
// contour. Roofs carry seams/ridge detail so repeated buildings never read
// as flat colored rectangles.
// ---------------------------------------------------------------------------

// Gabled roof: two planes split by a ridge line running `along` the long
// axis; the plane facing the sun (upper-left) is lighter.
void gableRoof(Hi& hi, float x, float y, float bw, float bh, bool ridgeVertical,
               std::uint32_t light, std::uint32_t dark) {
    if (ridgeVertical) {
        hi.fillPoly({{x, y}, {x + bw * 0.5f, y}, {x + bw * 0.5f, y + bh}, {x, y + bh}}, light);
        hi.fillPoly({{x + bw * 0.5f, y}, {x + bw, y}, {x + bw, y + bh},
                     {x + bw * 0.5f, y + bh}}, dark);
        hi.strokeLine(x + bw * 0.5f, y, x + bw * 0.5f, y + bh, 0.5f, shade(light, 1.25f));
    } else {
        hi.fillPoly({{x, y}, {x + bw, y}, {x + bw, y + bh * 0.5f}, {x, y + bh * 0.5f}}, light);
        hi.fillPoly({{x, y + bh * 0.5f}, {x + bw, y + bh * 0.5f}, {x + bw, y + bh},
                     {x, y + bh}}, dark);
        hi.strokeLine(x, y + bh * 0.5f, x + bw, y + bh * 0.5f, 0.5f, shade(light, 1.25f));
    }
}

// Wall slivers on the bottom + right edges (the visible faces).
void wallFaces(Hi& hi, float x, float y, float bw, float bh, std::uint32_t wall) {
    hi.fillPoly({{x, y + bh}, {x + bw, y + bh}, {x + bw - 0.8f, y + bh + 1.6f},
                 {x - 0.4f, y + bh + 1.6f}}, shade(wall, 0.72f));
    hi.fillPoly({{x + bw, y}, {x + bw, y + bh}, {x + bw + 1.4f, y + bh - 0.8f},
                 {x + bw + 1.4f, y - 0.4f}}, shade(wall, 0.88f));
}

struct HouseStyle {
    std::uint32_t roofLight, roofDark, wall;
    bool ridgeVertical;
};

PixelBuf makeHouse(float bw, float bh, const HouseStyle& st, int seed, bool chimney) {
    const int w = static_cast<int>(bw + 7), h = static_cast<int>(bh + 7);
    Hi hi(w, h);
    const float x = 2.0f, y = 2.0f;
    wallFaces(hi, x, y, bw, bh, st.wall);
    gableRoof(hi, x, y, bw, bh, st.ridgeVertical, st.roofLight, st.roofDark);
    // Roof seam lines along the planes (subtle shingle courses).
    const int seams = st.ridgeVertical ? static_cast<int>(bh / 3.0f)
                                       : static_cast<int>(bw / 3.0f);
    for (int i = 1; i < seams; ++i) {
        const float t = static_cast<float>(i) / seams;
        if (st.ridgeVertical) {
            hi.strokeLine(x, y + bh * t, x + bw * 0.5f, y + bh * t, 0.28f,
                          shade(st.roofDark, 1.12f));
            hi.strokeLine(x + bw * 0.5f, y + bh * t, x + bw, y + bh * t, 0.28f,
                          shade(st.roofDark, 0.92f));
        } else {
            hi.strokeLine(x + bw * t, y, x + bw * t, y + bh, 0.28f,
                          shade(st.roofDark, 1.12f));
            hi.strokeLine(x + bw * t, y + bh * 0.5f, x + bw * t, y + bh, 0.28f,
                          shade(st.roofDark, 0.92f));
        }
    }
    // Eave outline (crisp): stroke the roof perimeter dark.
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.5f,
              col(0x171510, 190), true);
    if (chimney) {
        const float chx = x + bw * (fr(seed, 1, 3) > 0.5f ? 0.22f : 0.78f);
        const float chy = y + bh * 0.62f;
        hi.fillRoundRect(chx, chy, 2.4f, 2.4f, 0.4f, pal::stone);
        hi.fillRoundRect(chx + 0.4f, chy + 0.4f, 1.6f, 1.6f, 0.3f, pal::stoneDk);
    }
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 2.6f, y + bh * 0.5f + 3.0f, bw * 0.58f, bh * 0.62f);
    // Scattered tone flecks so flat roofs are not dead flat.
    for (int i = 0; i < static_cast<int>(bw * bh / 14.0f); ++i) {
        const int fx = static_cast<int>(x + fr(seed, i, 51) * bw);
        const int fy = static_cast<int>(y + fr(seed, i, 53) * bh);
        const std::uint32_t c = p.get(fx, fy);
        if ((c & 0xFF) && fr(seed, i, 57) > 0.5f) p.set(fx, fy, shade(c, 1.08f));
    }
    return p;
}

PixelBuf makeFarmhouse(int variant) {
    // Larger farmhouse: main gabled block + small lean-to wing.
    HouseStyle st;
    if (variant == 0) {
        st = {pal::roofWarm, pal::roofWarmDk, pal::wallPale, false};
    } else {
        st = {pal::roofSlate, pal::roofSlateDk, mix24(0xCFC6B0, 0x808080, 0.15f), true};
    }
    const float bw = variant == 0 ? 16.0f : 14.0f, bh = variant == 0 ? 12.0f : 14.0f;
    const int w = static_cast<int>(bw + 12), h = static_cast<int>(bh + 9);
    Hi hi(w, h);
    const float x = 2.0f, y = 2.0f;
    wallFaces(hi, x, y, bw, bh, st.wall);
    gableRoof(hi, x, y, bw, bh, st.ridgeVertical, st.roofLight, st.roofDark);
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.5f,
              col(0x171510, 190), true);
    // Lean-to wing along the lower-left with its own mono-pitch roof.
    const float wx = x - 1.0f, wy = y + bh - 2.0f;
    hi.fillPoly({{wx + 4.0f, wy}, {wx + 9.0f, wy}, {wx + 9.0f, wy + 5.0f},
                 {wx + 4.0f, wy + 5.0f}},
                shade(st.roofDark, 1.05f));
    hi.stroke({{wx + 4.0f, wy}, {wx + 9.0f, wy}, {wx + 9.0f, wy + 5.0f},
               {wx + 4.0f, wy + 5.0f}}, 0.4f, col(0x171510, 170), true);
    // Chimney + yard path stub.
    hi.fillRoundRect(x + bw * 0.24f, y + bh * 0.55f, 2.6f, 2.6f, 0.4f, pal::stone);
    hi.fillRoundRect(x + bw * 0.24f + 0.5f, y + bh * 0.55f + 0.5f, 1.6f, 1.6f, 0.3f,
                     pal::stoneDk);
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 3.0f, y + bh * 0.5f + 3.4f, bw * 0.62f, bh * 0.66f);
    return p;
}

PixelBuf makeBarn(int variant) {
    // Barn: big gambrel-ish roof (three bands), dull red-brown, wide eaves;
    // variant 1 carries a fenced yard apron baked alongside.
    const float bw = 18.0f, bh = 13.0f;
    const int w = variant ? 26 : 23, h = 20;
    Hi hi(w, h);
    const float x = 2.5f, y = 2.5f;
    const std::uint32_t roofA = variant ? mute(0x7A4A32, 0.42f) : pal::roofWarmDk;
    const std::uint32_t roofB = shade(roofA, 1.28f);
    const std::uint32_t roofC = shade(roofA, 0.78f);
    wallFaces(hi, x, y, bw, bh, mix24(0x9A4436, 0x808080, 0.45f));
    // Gambrel cross-section: slope | flat | slope along the short axis.
    hi.fillPoly({{x, y}, {x + bw, y}, {x + bw, y + bh * 0.30f}, {x, y + bh * 0.30f}},
                roofB);
    hi.fillPoly({{x, y + bh * 0.30f}, {x + bw, y + bh * 0.30f},
                 {x + bw, y + bh * 0.70f}, {x, y + bh * 0.70f}}, roofA);
    hi.fillPoly({{x, y + bh * 0.70f}, {x + bw, y + bh * 0.70f}, {x + bw, y + bh},
                 {x, y + bh}}, roofC);
    hi.strokeLine(x, y + bh * 0.30f, x + bw, y + bh * 0.30f, 0.4f, shade(roofB, 1.15f));
    hi.strokeLine(x, y + bh * 0.70f, x + bw, y + bh * 0.70f, 0.4f, shade(roofA, 0.85f));
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.5f,
              col(0x17120E, 195), true);
    // Ridge vent cap.
    hi.fillRoundRect(x + bw * 0.42f, y + bh * 0.44f, 3.4f, 1.6f, 0.4f, shade(roofB, 1.1f));
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 3.0f, y + bh * 0.5f + 3.4f, bw * 0.64f, bh * 0.70f);
    if (variant) {
        // Small lean-to shed beside the barn (same canvas).
        Hi hi2(w, h);
        const float sx = 2.5f + bw + 3.0f, sy = 4.0f;
        hi2.fillPoly({{sx, sy}, {sx + 6.0f, sy}, {sx + 6.0f, sy + 8.0f}, {sx, sy + 8.0f}},
                     pal::roofMetalDk);
        hi2.strokeLine(sx, sy + 4.0f, sx + 6.0f, sy + 4.0f, 0.35f, pal::roofMetal);
        hi2.stroke({{sx, sy}, {sx + 6.0f, sy}, {sx + 6.0f, sy + 8.0f}, {sx, sy + 8.0f}},
                   0.4f, col(0x14160F, 180), true);
        PixelBuf s = hi2.bake();
        castShadow(s, sx + 4.5f, sy + 7.0f, 4.6f, 5.0f);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
                if (s.get(xx, yy) & 0xFF) p.set(xx, yy, s.get(xx, yy));
    }
    return p;
}

PixelBuf makeShed(int variant) {
    const float bw = variant ? 8.0f : 6.5f, bh = variant ? 6.0f : 5.0f;
    const int w = static_cast<int>(bw + 5), h = static_cast<int>(bh + 5);
    Hi hi(w, h);
    const float x = 1.5f, y = 1.5f;
    const std::uint32_t roof = variant ? pal::roofMetalDk : pal::roofOliveDk;
    wallFaces(hi, x, y, bw, bh, mix24(0x6E6250, 0x808080, 0.3f));
    // Mono-pitch: light half toward the sun, dark half away.
    hi.fillPoly({{x, y}, {x + bw, y}, {x + bw, y + bh * 0.5f}, {x, y + bh * 0.5f}},
                shade(roof, 1.35f));
    hi.fillPoly({{x, y + bh * 0.5f}, {x + bw, y + bh * 0.5f}, {x + bw, y + bh},
                 {x, y + bh}}, roof);
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.45f,
              col(0x14160F, 185), true);
    if (variant)  // corrugation ribs
        for (float cxp = x + 1.0f; cxp < x + bw; cxp += 1.6f)
            hi.strokeLine(cxp, y, cxp, y + bh, 0.22f, shade(roof, 1.12f));
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 2.0f, y + bh * 0.5f + 2.4f, bw * 0.60f, bh * 0.68f);
    return p;
}

PixelBuf makeWarehouse(int variant) {
    // Long corrugated shed: alternating rib stripes across the span, ridge
    // along the long axis, gable end caps.
    const float bw = variant ? 26.0f : 22.0f, bh = 13.0f;
    const int w = static_cast<int>(bw + 6), h = 20;
    Hi hi(w, h);
    const float x = 2.0f, y = 2.5f;
    const std::uint32_t metal = variant ? pal::roofMetal : pal::roofOlive;
    const std::uint32_t metalDk = shade(metal, 0.72f);
    wallFaces(hi, x, y, bw, bh, mix24(0x7C7668, 0x808080, 0.3f));
    gableRoof(hi, x, y, bw, bh, false, shade(metal, 1.12f), metalDk);
    for (float cxp = x + 1.2f; cxp < x + bw; cxp += 2.0f) {
        hi.strokeLine(cxp, y, cxp, y + bh * 0.5f, 0.3f, shade(metal, 1.04f));
        hi.strokeLine(cxp, y + bh * 0.5f, cxp, y + bh, 0.3f, shade(metalDk, 1.05f));
    }
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.5f,
              col(0x15171A, 190), true);
    // Loading bay at the near end: dark door + a pale apron.
    hi.fillPoly({{x + bw - 3.0f, y + 2.0f}, {x + bw, y + 2.0f}, {x + bw, y + bh - 2.0f},
                 {x + bw - 3.0f, y + bh - 2.0f}}, shade(metalDk, 0.6f));
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 3.0f, y + bh * 0.5f + 3.2f, bw * 0.56f, bh * 0.66f);
    return p;
}

PixelBuf makeBarracks(int variant) {
    // Long low hut, olive roof, row of small windows on the visible wall.
    const float bw = 20.0f, bh = 8.0f;
    const int w = 26, h = 16;
    Hi hi(w, h);
    const float x = 2.0f, y = 2.0f;
    const std::uint32_t roof = variant ? pal::roofOlive : pal::roofMetalDk;
    wallFaces(hi, x, y, bw, bh, mix24(0x77725F, 0x808080, 0.35f));
    gableRoof(hi, x, y, bw, bh, false, shade(roof, 1.22f), shade(roof, 0.8f));
    hi.stroke({{x, y}, {x + bw, y}, {x + bw, y + bh}, {x, y + bh}}, 0.45f,
              col(0x14160F, 185), true);
    for (int i = 0; i < 4; ++i)  // windows on the south wall sliver
        hi.strokeLine(x + 4.0f + i * 4.4f, y + bh + 1.1f, x + 5.6f + i * 4.4f,
                      y + bh + 1.1f, 0.5f, pal::glass);
    if (variant)  // camo netting blotches on the roof
        for (int i = 0; i < 6; ++i)
            hi.fillEllipse(x + 2.0f + fr(i, 7, 3) * (bw - 4.0f),
                           y + 1.5f + fr(i, 9, 5) * (bh - 3.0f), 2.2f, 1.4f,
                           mix24(0x4A5236, 0x2E3423, fr(i, 1, 7)));
    PixelBuf p = hi.bake();
    castShadow(p, x + bw * 0.5f + 2.6f, y + bh * 0.5f + 2.8f, bw * 0.58f, bh * 0.8f);
    return p;
}

// ---------------------------------------------------------------------------
// Props.
// ---------------------------------------------------------------------------
PixelBuf makeFuelTank(int variant) {
    const int size = variant ? 14 : 11;
    const int w = size + 6, h = size + 7;
    Hi hi(w, h);
    const float cx = w * 0.5f - 0.5f, cy = h * 0.5f - 0.5f;
    const float R = size * 0.5f;
    const std::uint32_t base = variant ? mute(0x8A8D84, 0.3f) : mute(0x6E7568, 0.3f);
    // Ground ring (bund) then the tank drum.
    hi.fillEllipse(cx + 1.0f, cy + 1.6f, R * 1.25f, R * 1.05f, col(0x4C483A, 200));
    hi.fillEllipse(cx, cy, R, R, base);
    // Cylindrical shading: crescent dark to the bottom-right, sunlit
    // crescent top-left, bright valve cap at centre.
    hi.fillEllipse(cx + R * 0.28f, cy + R * 0.30f, R * 0.80f, R * 0.74f,
                   mixPacked(base, col(0x1E221C), 0.45f));
    hi.fillEllipse(cx - R * 0.30f, cy - R * 0.32f, R * 0.52f, R * 0.46f,
                   mixPacked(base, col(0xFFFFFF), 0.30f));
    hi.fillEllipse(cx - R * 0.10f, cy - R * 0.10f, R * 0.22f, R * 0.22f,
                   mixPacked(base, col(0xFFFFFF), 0.5f));
    hi.stroke({{cx + R * 0.7f, cy}, {cx + R * 1.5f, cy}}, 0.6f, pal::barkDk);
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x161812, 160));
    castShadow(p, cx + 2.6f, cy + 3.2f, R * 1.05f, R * 0.85f);
    return p;
}

PixelBuf makeHaystack() {
    const int w = 12, h = 12;
    Hi hi(w, h);
    const float cx = 5.5f, cy = 5.5f;
    hi.fillEllipse(cx, cy, 4.6f, 4.2f, pal::hay);
    hi.fillEllipse(cx + 1.0f, cy + 1.2f, 3.4f, 2.8f, shade(pal::hay, 0.78f));
    hi.fillEllipse(cx - 1.1f, cy - 1.2f, 2.2f, 1.8f, pal::hayHi);
    hi.fillEllipse(cx - 1.4f, cy - 1.5f, 0.9f, 0.8f, mix24(0xDCC07A, 0xFFFFFF, 0.2f));
    // Thrash lines radiating on the lit side.
    for (int i = 0; i < 5; ++i) {
        const float a = 3.6f + i * 0.28f;
        hi.strokeLine(cx, cy, cx + std::cos(a) * 4.2f, cy + std::sin(a) * 3.9f, 0.25f,
                      shade(pal::hay, 0.9f));
    }
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x2A2010, 140));
    castShadow(p, cx + 2.0f, cy + 2.6f, 4.8f, 3.2f);
    return p;
}

PixelBuf makeWell() {
    const int w = 10, h = 11;
    Hi hi(w, h);
    const float cx = 4.6f, cy = 6.4f;
    hi.fillEllipse(cx, cy, 3.4f, 2.8f, pal::stone);          // kerb
    hi.fillEllipse(cx, cy, 2.2f, 1.8f, pal::stoneDk);        // inner wall
    hi.fillEllipse(cx, cy + 0.2f, 1.5f, 1.2f, pal::water);   // dark water
    hi.fillEllipse(cx - 0.5f, cy - 0.2f, 0.6f, 0.4f, pal::waterHi);
    // A-frame supports + tiny roof over the kerb.
    hi.strokeLine(cx - 2.6f, cy - 1.0f, cx, cy - 4.6f, 0.4f, pal::bark);
    hi.strokeLine(cx + 2.6f, cy - 1.0f, cx, cy - 4.6f, 0.4f, pal::bark);
    hi.fillPoly({{cx - 3.0f, cy - 3.6f}, {cx + 3.0f, cy - 3.6f}, {cx, cy - 5.6f}},
                pal::roofOliveDk);
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x1A160E, 140));
    castShadow(p, cx + 1.8f, cy + 2.2f, 3.8f, 2.6f);
    return p;
}

PixelBuf makeRock(int variant) {
    const int w = variant ? 20 : 13, h = w;
    Hi hi(w, h);
    const float cx = w * 0.5f, cy = h * 0.5f;
    const int n = variant ? 4 : 2;
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * i / n + fr(i, variant, 3);
        const float d = variant ? 4.5f : 2.6f;
        const float rx = (variant ? 4.2f : 3.4f) * (0.7f + fr(i, variant, 7) * 0.5f);
        const float ry = rx * 0.78f;
        const float rx0 = cx + std::cos(a) * d, ry0 = cy + std::sin(a) * d * 0.7f;
        hi.fillEllipse(rx0 + 1.0f, ry0 + 1.4f, rx * 1.02f, ry, col(0x120E08, 110));
        hi.fillEllipse(rx0, ry0, rx, ry, mix24(0x6E6A5E, 0x8B8578, fr(i, variant, 11)));
        hi.fillEllipse(rx0 - rx * 0.28f, ry0 - ry * 0.30f, rx * 0.5f, ry * 0.45f,
                       mute(0xA8A294, 0.25f));
        hi.strokeLine(rx0 - rx * 0.3f, ry0, rx0 + rx * 0.4f, ry0 + ry * 0.3f, 0.3f,
                      mute(0x4A463C, 0.2f));
    }
    PixelBuf p = hi.bake();
    outlineSilhouette(p, col(0x1A1812, 150));
    return p;
}

PixelBuf makePond() {
    const int w = 40, h = 30;
    Hi hi(w, h);
    // Organic shoreline: lobed water body, muddy rim, reed dots.
    const float cx = 20.0f, cy = 15.0f;
    for (int i = 0; i < 7; ++i) {
        const float a = 6.2831853f * i / 7 + fr(i, 0, 5) * 0.4f;
        const float d = 11.0f + fr(i, 0, 9) * 4.0f;
        hi.fillEllipse(cx + std::cos(a) * d * 0.85f, cy + std::sin(a) * d * 0.55f,
                       7.0f + fr(i, 0, 13) * 3.0f, 5.0f + fr(i, 0, 17) * 2.0f, pal::mud);
    }
    for (int i = 0; i < 7; ++i) {
        const float a = 6.2831853f * i / 7 + fr(i, 1, 5) * 0.4f;
        const float d = 9.0f + fr(i, 1, 9) * 3.0f;
        hi.fillEllipse(cx + std::cos(a) * d * 0.8f, cy + std::sin(a) * d * 0.5f,
                       6.0f + fr(i, 1, 13) * 2.5f, 4.2f + fr(i, 1, 17) * 1.6f,
                       pal::water);
    }
    hi.fillEllipse(cx - 4.0f, cy - 2.0f, 6.0f, 3.6f, mix24(0x2E5570, 0x5B87A2, 0.45f));
    // Sky glint streaks + lily pads.
    hi.strokeLine(cx - 8.0f, cy - 3.0f, cx - 1.0f, cy - 4.0f, 0.5f, pal::waterHi);
    hi.strokeLine(cx - 4.0f, cy + 1.0f, cx + 5.0f, cy + 0.0f, 0.5f,
                  shade(pal::waterHi, 0.9f));
    hi.fillEllipse(cx + 6.0f, cy + 3.0f, 1.6f, 1.2f, pal::leafDk);
    hi.fillEllipse(cx + 9.0f, cy + 1.0f, 1.2f, 0.9f, pal::leafMid);
    for (int i = 0; i < 10; ++i) {  // reeds on the rim
        const float a = fr(i, 3, 5) * 6.2831853f;
        const float d = 1.0f;
        hi.fillEllipse(cx + std::cos(a) * 13.5f * d, cy + std::sin(a) * 8.5f, 1.0f, 1.6f,
                       mix24(0x53663A, 0x36442A, fr(i, 3, 9)));
    }
    PixelBuf p = hi.bake();
    castShadow(p, cx + 1.5f, cy + 2.5f, 16.0f, 10.5f);  // very subtle depth ring
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
void registerEnvironmentSprites(
    const std::function<void(const std::string&, const PixelBuf&)>& add) {
    for (int i = 0; i < 3; ++i)
        add("env_tree_broadleaf_" + std::to_string(i), makeBroadleaf(22 + i * 5, i));
    for (int i = 0; i < 2; ++i)
        add("env_tree_small_" + std::to_string(i), makeBroadleaf(12 + i * 3, 1 - i));
    for (int i = 0; i < 2; ++i)
        add("env_tree_palm_" + std::to_string(i), makePalm(i));
    for (int i = 0; i < 2; ++i)
        add("env_tree_conifer_" + std::to_string(i), makeConifer(18 + i * 3, i));
    for (int i = 0; i < 2; ++i)
        add("env_bush_" + std::to_string(i), makeBush(i));
    for (int i = 0; i < 2; ++i)
        add("env_hedgerow_" + std::to_string(i), makeHedgerow(i));

    for (int i = 0; i < 2; ++i)
        add("env_farmhouse_" + std::to_string(i), makeFarmhouse(i));
    const HouseStyle styles[3] = {
        {pal::roofSlate, pal::roofSlateDk, pal::wallPale, true},
        {pal::roofThatch, pal::roofThatchDk, mix24(0xC8BCA2, 0x808080, 0.12f), false},
        {pal::roofWarm, pal::roofWarmDk, mix24(0xB0A48C, 0x808080, 0.18f), true},
    };
    for (int i = 0; i < 3; ++i)
        add("env_house_" + std::to_string(i),
            makeHouse(10.0f + i * 2.0f, 9.0f + (i % 2) * 2.0f, styles[i], i, i != 1));
    for (int i = 0; i < 2; ++i)
        add("env_barn_" + std::to_string(i), makeBarn(i));
    for (int i = 0; i < 2; ++i)
        add("env_shed_" + std::to_string(i), makeShed(i));
    for (int i = 0; i < 2; ++i)
        add("env_warehouse_" + std::to_string(i), makeWarehouse(i));
    for (int i = 0; i < 2; ++i)
        add("env_barracks_" + std::to_string(i), makeBarracks(i));

    for (int i = 0; i < 2; ++i)
        add("env_fuel_tank_" + std::to_string(i), makeFuelTank(i));
    add("env_haystack", makeHaystack());
    add("env_well", makeWell());
    for (int i = 0; i < 2; ++i)
        add("env_rock_" + std::to_string(i), makeRock(i));
    add("env_pond", makePond());
}

}  // namespace ww
