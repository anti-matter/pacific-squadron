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
const std::uint32_t kCanopy = col(0x6FA8D8);
const std::uint32_t kInsigniaBlue = col(0x24407A);
const std::uint32_t kWhite = col(0xF2F2F2);
const std::uint32_t kBlack = col(0x101010);

const std::uint32_t kEnTan = col(0x8A7A4E);
const std::uint32_t kEnTanDk = col(0x66583A);
const std::uint32_t kEnGreen = col(0x4E5A38);
const std::uint32_t kEnGreenDk = col(0x38422A);
const std::uint32_t kEnYellow = col(0xC8A83C);
const std::uint32_t kEnGrey = col(0x5A5F63);
const std::uint32_t kEnGreyDk = col(0x40444A);
const std::uint32_t kRed = col(0xC23A2A);
const std::uint32_t kNavalGrey = col(0x74797E);
const std::uint32_t kNavalDk = col(0x51565B);
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
// Aircraft builder (drawn nose-up, mirrored for downward-facing enemies)
// ---------------------------------------------------------------------------
struct PlaneStyle {
    int span;          // total wingspan in px
    int len;           // fuselage length
    std::uint32_t body, bodyHi, bodyDk;
    bool twin = false; // twin engine
    bool redDot = false; // enemy marking
};

PixelBuf makePlane(const PlaneStyle& st, int bankRows = 0) {
    const int W = 52, H = 52;
    PixelBuf p(W, H);
    const int cx = W / 2;
    const int cy = H / 2;
    const int half = st.span / 2;
    const int bodyHalf = st.twin ? 8 : 4;

    // Fuselage (nose at top).
    for (int y = 0; y < st.len; ++y) {
        const float t = static_cast<float>(y) / st.len;
        int hw = static_cast<int>(bodyHalf * (1.0f - t * t * 0.45f));
        if (y < 4) hw = std::max(1, (y + 1) / 2);
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = st.body;
            if (x == -hw) c = st.bodyHi;
            if (x == hw) c = st.bodyDk;
            p.set(cx + x, cy - st.len / 2 + y, c);
        }
    }
    // Prop spinner / nose highlight.
    p.set(cx, cy - st.len / 2, shade(kBlack, 1.4f));
    p.set(cx, cy - st.len / 2 + 1, kDrabDk);
    if (st.twin) {
        // Engine nacelles.
        for (int side = -1; side <= 1; side += 2) {
            const int ex = cx + side * (half / 2);
            for (int y = -7; y <= 5; ++y)
                for (int x = -3; x <= 3; ++x)
                    p.set(ex + x, cy + y, std::abs(x) == 3 ? st.bodyDk : st.body);
            p.set(ex, cy - 8, kBlack);  // prop hub
        }
    }
    // Main wing: trapezoid, tips shifted vertically for banking.
    const int wingY = cy + 1;
    for (int wing = 0; wing < 2; ++wing) {
        const int side = wing == 0 ? -1 : 1;
        for (int d = 1; d <= half; ++d) {
            const float t = static_cast<float>(d) / half;
            const int chord = std::max(2, static_cast<int>(8.0f * (1.0f - t * 0.55f)));
            const int tipShift = static_cast<int>(bankRows * t * side);
            for (int c = 0; c < chord; ++c) {
                std::uint32_t cc = st.body;
                if (c == 0) cc = st.bodyHi;
                if (c == chord - 1) cc = st.bodyDk;
                if (d <= 2 && c >= 1 && c <= chord - 2) cc = st.bodyDk;  // root shadow
                // Aileron seam outboard.
                if (d > half * 2 / 3 && c == chord - 2) cc = st.bodyDk;
                p.set(cx + side * d, wingY + c + tipShift, cc);
            }
        }
    }
    // Tailplane.
    const int tailY = cy + st.len / 2 - 6;
    for (int side = -1; side <= 1; side += 2)
        for (int d = 1; d <= st.span / 5; ++d)
            for (int c = 0; c < 3; ++c)
                p.set(cx + side * d, tailY + c, c == 0 ? st.bodyHi : st.body);
    // Rudder.
    for (int y = 0; y < 5; ++y) p.set(cx, tailY + y, st.bodyDk);

    // Cockpit.
    if (!st.twin) {
        for (int y = -4; y <= 1; ++y)
            for (int x = -2; x <= 2; ++x)
                p.set(cx + x, cy - 2 + y, (x == -2 || x == 2) ? shade(kCanopy, 0.6f) : kCanopy);
        // Canopy glass reflection streak.
        p.set(cx - 1, cy - 3, shade(kCanopy, 1.45f));
        p.set(cx, cy - 2, shade(kCanopy, 1.45f));
        p.set(cx - 1, cy - 1, shade(kCanopy, 1.2f));
    } else {
        for (int y = -2; y <= 1; ++y)
            for (int x = -2; x <= 2; ++x)
                p.set(cx + x, cy - 6 + y, kCanopy);
        p.set(cx - 1, cy - 6, shade(kCanopy, 1.45f));
        p.set(cx, cy - 5, shade(kCanopy, 1.45f));
    }

    // Fuselage spine highlight (light from upper-left).
    for (int y = -st.len / 2 + 3; y < st.len / 2 - 4; ++y)
        p.set(cx - 1, cy + y, st.bodyHi);

    // Exhaust stubs + faint burn streaks behind the engine.
    for (int side : {-1, 1}) {
        const int ex = cx + side * (st.twin ? half / 2 : 3);
        const int ey = st.twin ? cy + 6 : cy + 4;
        p.set(ex, ey, kBlack);
        for (int q = 1; q <= 4; ++q)
            p.set(ex + side * (q / 3), ey + q, col(0x1A1D20, 90 - q * 18));
    }

    // Prop blur arc in front of the nose (single-engine).
    if (!st.twin) {
        for (int x = -5; x <= 5; ++x)
            p.set(cx + x, cy - st.len / 2 - 2 + (std::abs(x) > 3 ? 1 : 0),
                  col(0xC8D4DC, 55));
    }

    // Markings.
    if (st.redDot) {
        for (int wing = 0; wing < 2; ++wing) {
            const int side = wing == 0 ? -1 : 1;
            const int mx = cx + side * (half * 2 / 3);
            p.fillEllipse(static_cast<float>(mx), static_cast<float>(wingY + 3), 3.5f, 3.5f, kRed);
            // White sun-disc halo for silhouette punch.
            p.set(mx - 4, wingY + 3, kWhite);
            p.set(mx + 4, wingY + 3, kWhite);
            p.set(mx, wingY - 1, kWhite);
            p.set(mx, wingY + 7, kWhite);
        }
        // Red tail band.
        for (int x = -3; x <= 3; ++x)
            p.set(cx + x, tailY + 4, kRed);
    }
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
// Explosion animation sheets
// ---------------------------------------------------------------------------
PixelBuf makeExplosionSheet(int frames, int frameSize, std::uint64_t seed,
                            bool splash) {
    PixelBuf sheet(frameSize * frames, frameSize);
    Rng rng(seed);
    for (int f = 0; f < frames; ++f) {
        const float t = static_cast<float>(f) / (frames - 1);
        const float cx = f * frameSize + frameSize * 0.5f;
        const float cy = frameSize * 0.5f;
        const float rMax = frameSize * 0.48f;
        const float r = splash ? rMax * (0.25f + 0.75f * std::sqrt(t))
                               : rMax * (0.30f + 0.70f * std::sqrt(t));
        const int alpha = static_cast<int>(255.0f * (1.0f - t * t));
        // Pre-jittered lobes so each explosion looks organic but stable.
        std::vector<float> lobes(24);
        for (float& v : lobes) v = rng.range(0.72f, 1.18f);
        for (int y = 0; y < frameSize; ++y) {
            for (int x = 0; x < frameSize; ++x) {
                const float dx = x + 0.5f - cx;
                const float dy = y + 0.5f - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 1.0f) continue;
                int ang = static_cast<int>((std::atan2(dy, dx) + 3.15f) / 6.29f * 24.0f) % 24;
                const float edge = r * lobes[static_cast<std::size_t>(ang)];
                if (dist > edge) continue;
                const float tEdge = dist / std::max(0.01f, edge);
                std::uint32_t c;
                if (splash) {
                    c = tEdge < 0.55f ? col(0xFFFFFF, 255) : col(0x9CC8E8, 230);
                } else {
                    // Hot core -> orange -> red -> smoke, with a bright rim
                    // in the first half and soot in the second.
                    if (t < 0.18f)
                        c = tEdge < 0.85f ? col(0xFFFFF0) : col(0xFFD860);
                    else if (t < 0.38f)
                        c = tEdge < 0.40f ? col(0xFFF4C0)
                          : tEdge < 0.72f ? col(0xFFB22E) : col(0xF0501A);
                    else if (t < 0.65f)
                        c = tEdge < 0.30f ? col(0xFFC23A)
                          : tEdge < 0.62f ? col(0xE8601E) : col(0x8A2A10);
                    else
                        c = tEdge < 0.40f ? col(0xA8481A)
                          : tEdge < 0.75f ? col(0x54301C) : col(0x2E2C2C);
                    // Bright shock rim on the early frames.
                    if (t < 0.30f && tEdge > 0.88f) c = col(0xFFE8A0);
                }
                const int a = static_cast<int>((c & 0xFF) *
                                               (1.0f - tEdge * tEdge * 0.7f) *
                                               (alpha / 255.0f) *
                                               (1.0f + 0.0f));
                sheet.set(x, y, (c & 0xFFFFFF00u) |
                                    static_cast<std::uint32_t>(std::min(255, a)));
            }
        }
        // Splashes for water variant.
        if (splash && f < frames - 1) {
            for (int k = 0; k < 6; ++k) {
                const float a = rng.range(0.0f, 6.28f);
                const float d = r * rng.range(1.0f, 1.5f);
                sheet.set(static_cast<int>(cx + std::cos(a) * d),
                          static_cast<int>(cy + std::sin(a) * d * 0.7f),
                          col(0xFFFFFF, 200));
            }
        }
        // Debris embers flung outward on the early frames.
        if (!splash && t < 0.5f) {
            for (int k = 0; k < 7; ++k) {
                const float a = rng.range(0.0f, 6.28f);
                const float d = r * rng.range(1.05f, 1.6f + t);
                const int ex = static_cast<int>(cx + std::cos(a) * d);
                const int ey = static_cast<int>(cy + std::sin(a) * d);
                sheet.set(ex, ey, t < 0.25f ? col(0xFFE8A0, 230) : col(0xE86A20, 200));
            }
        }
        // Late-frame soot ring for weight.
        if (!splash && t > 0.62f) {
            for (int k = 0; k < 14; ++k) {
                const float a = k / 14.0f * 6.283f + 0.4f;
                const float d = r * rng.range(0.82f, 1.02f);
                sheet.set(static_cast<int>(cx + std::cos(a) * d),
                          static_cast<int>(cy + std::sin(a) * d), col(0x262424, 150));
            }
        }
    }
    return sheet;
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
    PixelBuf p(32, 32);
    const float cx = 16.0f;
    // Sandbag ring outer perimeter.
    p.fillEllipse(cx, 21, 15.0f, 12.0f, col(0x5C4C30));
    p.fillEllipse(cx, 20, 14.0f, 11.0f, col(0x7A6844));
    for (int k = 0; k < 10; ++k) {   // individual bags
        const float a = k * 0.6283f + 0.3f;
        const float bx = cx + 13.5f * std::cos(a);
        const float by = 20.0f + 10.5f * std::sin(a);
        p.fillEllipse(bx, by, 2.6f, 2.0f, (k % 2) ? col(0x8A7A52) : col(0x6A5A3C));
    }
    // Concrete gun platform inside.
    p.fillEllipse(cx, 19, 10.0f, 8.0f, col(0x5C5C52));
    p.fillEllipse(cx, 18, 9.0f, 7.0f, col(0x8C8C80));
    p.fillEllipse(cx - 2, 16, 5.0f, 3.5f, col(0x9C9C90));   // lit top
    p.fillEllipse(cx, 20, 4.0f, 3.0f, col(0x6A6A60));       // mount base
    // Armored gun shield facing the player.
    for (int x = -7; x <= 7; ++x) {
        const int h = 5 - (std::abs(x) > 5 ? 1 : 0);
        p.fillRect(static_cast<int>(cx) + x - 1, 9, 1, h, x <= 0 ? col(0x8A9094) : col(0x4E5459));
    }
    // Twin AA barrels with flash hiders.
    for (int bx : {13, 18}) {
        p.fillRect(bx, 1, 2, 14, kBlack);
        p.fillRect(bx, 1, 1, 14, shade(kBlack, 2.2f));
        p.fillRect(bx - 1, 1, 4, 2, col(0x2B2F33));
    }
    // Ready-rack box and ladder.
    p.fillRect(22, 20, 5, 4, col(0x4A4A40));
    p.fillRect(22, 20, 5, 1, col(0x7A7A6C));
    p.hline(8, 10, 26, col(0x3A3A32));
    p.hline(8, 10, 29, col(0x3A3A32));
    return p;
}

PixelBuf makeTank() {
    PixelBuf p(40, 28);
    // Hull (facing down the screen).
    p.fillRect(6, 4, 28, 22, kEnGreenDk);
    p.fillRect(8, 6, 24, 18, kEnGreen);
    // Tracks.
    for (int y = 4; y < 26; y += 2) {
        p.fillRect(4, y, 4, 1, kBlack);
        p.fillRect(32, y, 4, 1, kBlack);
    }
    // Turret + barrel pointing down.
    p.fillEllipse(20.0f, 12.0f, 7.0f, 6.0f, shade(kEnGreen, 1.15f));
    p.fillRect(19, 14, 2, 13, kBlack);
    p.fillRect(18, 25, 4, 3, kEnGreenDk);
    return p;
}

PixelBuf makeBunker() {
    PixelBuf p(32, 32);
    p.fillEllipse(16, 18, 13, 11, kConcreteDk);
    p.fillEllipse(16, 16, 12, 10, kConcrete);
    p.fillEllipse(13, 13, 5, 3, shade(kConcrete, 1.18f));
    p.fillRect(8, 16, 16, 3, kBlack);  // embrasure
    p.fillRect(6, 26, 20, 2, shade(kConcrete, 0.7f));
    return p;
}

PixelBuf makeShip(int w, int h, bool destroyer) {
    PixelBuf p(w, h);
    // Hull: pointed bow at bottom (ships sail toward the player... they face
    // down for the boss battleship; patrol boats face up). Draw facing down.
    for (int y = 0; y < h; ++y) {
        const float t = static_cast<float>(y) / h;
        const int half = static_cast<int>((w / 2 - 2) * (t > 0.75f ? (1.0f - (t - 0.75f) * 3.2f) : 1.0f));
        if (half <= 0) continue;
        for (int x = w / 2 - half; x <= w / 2 + half; ++x) {
            std::uint32_t c = kNavalGrey;
            if (y < 4 || x == w / 2 - half || x == w / 2 + half) c = kNavalDk;
            p.set(x, y, c);
        }
    }
    // Deck structures.
    p.fillRect(w / 2 - 6, h / 2 - 8, 12, 16, shade(kNavalGrey, 1.2f));
    p.fillRect(w / 2 - 4, h / 2 - 6, 8, 10, kNavalDk);
    // Gun turrets.
    auto turret = [&](int tx, int ty) {
        p.fillEllipse(static_cast<float>(tx), static_cast<float>(ty), 3.5f, 3.5f, kNavalDk);
        p.fillRect(tx - 1, ty + 2, 2, 5, kBlack);
    };
    turret(w / 2, h - 8);
    if (destroyer) { turret(w / 2, 10); turret(w / 2 - 10, h / 2); turret(w / 2 + 10, h / 2); }
    return p;
}

// ---------------------------------------------------------------------------
// Power-ups / bullets / misc
// ---------------------------------------------------------------------------
PixelBuf makePowerup(char letter, std::uint32_t c1, std::uint32_t c2) {
    PixelBuf p(22, 22);
    p.fillEllipse(11, 11, 11, 11, c2);
    p.fillEllipse(11, 11, 9.5f, 9.5f, c1);
    p.fillEllipse(11, 9, 7, 6, shade(c1, 1.2f));
    p.glyph(letter, 9, 8, kWhite);
    return p;
}

PixelBuf makeBulletPlayer() {
    PixelBuf p(4, 10);
    p.fillRect(1, 0, 2, 6, col(0xFFE870));
    p.fillRect(1, 0, 2, 3, kWhite);
    p.fillRect(1, 6, 2, 4, col(0xFFB028, 160));
    return p;
}
PixelBuf makeBulletCannon() {
    PixelBuf p(7, 13);
    p.fillEllipse(3.5f, 4.0f, 3.0f, 4.0f, col(0xFF9830));
    p.fillEllipse(3.5f, 3.0f, 1.6f, 2.2f, kWhite);
    p.fillRect(2, 8, 3, 4, col(0xE85A20, 170));
    return p;
}
PixelBuf makeBulletRocket() {
    PixelBuf p(6, 14);
    p.fillRect(2, 0, 2, 8, col(0xCCCCC8));
    p.fillRect(2, 0, 2, 2, kRed);
    p.set(1, 7, kDrabDk); p.set(4, 7, kDrabDk);
    p.fillRect(2, 9, 2, 3, col(0xFFC860, 200));
    p.fillRect(2, 12, 2, 2, col(0xFF7020, 120));
    return p;
}
PixelBuf makeBulletEnemy() {
    PixelBuf p(5, 5);
    p.fillEllipse(2.5f, 2.5f, 2.5f, 2.5f, col(0xE8482A));
    p.fillEllipse(2.5f, 2.5f, 1.2f, 1.2f, col(0xFFE0B0));
    return p;
}
PixelBuf makeShellEnemy() {
    PixelBuf p(8, 8);
    p.fillEllipse(4, 4, 3.8f, 3.8f, col(0x2A2A30));
    p.fillEllipse(4, 4, 2.2f, 2.2f, col(0xB85A20));
    p.fillEllipse(3.4f, 3.4f, 1.0f, 1.0f, col(0xFFE8A0));
    return p;
}
PixelBuf makeFlak() {
    PixelBuf p(8, 8);
    p.fillEllipse(4, 4, 3.6f, 3.6f, col(0x505050, 220));
    p.fillEllipse(3.2f, 3.2f, 1.8f, 1.8f, col(0x909090, 240));
    return p;
}

PixelBuf makeShadow() {
    PixelBuf p(20, 12);
    p.fillEllipse(10, 6, 9, 5, col(0x000000, 70));
    return p;
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------
PixelBuf makeOceanTile(int variant) {
    // Two-scale wave texture: long darker swells, short bright ripples, and
    // sparse specular sparkle. All features wrap modulo 32 so the tile
    // stays seamless.
    PixelBuf p(32, 32);
    Rng rng(0x0CE4Au + variant * 77);
    auto wset = [&](int x, int y, std::uint32_t c) {
        p.set(((x % 32) + 32) % 32, ((y % 32) + 32) % 32, c);
    };
    p.fill(col(0x173C6C));
    // Large soft colour patches (depth variation).
    for (int i = 0; i < 3; ++i) {
        const int px0 = rng.rangeInt(0, 31), py0 = rng.rangeInt(0, 31);
        const int rx = rng.rangeInt(5, 9), ry = rng.rangeInt(4, 7);
        for (int y = -ry; y <= ry; ++y)
            for (int x = -rx; x <= rx; ++x)
                if ((x * x) / (rx * rx) + (y * y) / (ry * ry) <= 1)
                    wset(px0 + x, py0 + y, col(0x1E4676));
    }
    // Long swell lines with bright crests.
    const int swells = 2 + (variant % 2);
    for (int i = 0; i < swells; ++i) {
        const int x = rng.rangeInt(0, 24), y = rng.rangeInt(0, 30);
        const int len = rng.rangeInt(12, 20);
        for (int q = 0; q < len; ++q) {
            const int yy = y + (q / 4) % 2;
            wset(x + q, yy, col(0x26508A));
            wset(x + q, yy - 1, col(0x3C6CA4));
        }
        for (int q = 2; q < len - 2; ++q) wset(x + q, y + 2, col(0x10305A));
    }
    // Short ripple dashes.
    for (int i = 0; i < 6; ++i) {
        const int x = rng.rangeInt(0, 28), y = rng.rangeInt(0, 31);
        wset(x, y, col(0x2C5890));
        wset(x + 1, y, col(0x2C5890));
        wset(x + 2, y - 1, col(0x2C5890));
    }
    // Scattered texture pixels.
    for (int i = 0; i < 26; ++i)
        wset(rng.rangeInt(0, 31), rng.rangeInt(0, 31),
             rng.chance(0.5f) ? col(0x204A80) : col(0x123462));
    // Specular sparkles.
    const int sparkles = 2 + variant * 2;
    for (int i = 0; i < sparkles; ++i)
        wset(rng.rangeInt(0, 31), rng.rangeInt(0, 31),
             col(0x6FA0CC, static_cast<std::uint8_t>(rng.rangeInt(150, 220))));
    return p;
}

PixelBuf makeSandTile(int variant) {
    PixelBuf p(32, 32);
    p.fill(col(0xC2A868));
    Rng rng(0x5ABDu + variant);
    for (int i = 0; i < 70; ++i) {
        const int x = rng.rangeInt(0, 31), y = rng.rangeInt(0, 31);
        p.set(x, y, rng.chance(0.5f) ? col(0xD0B878) : col(0xA88E50));
    }
    return p;
}

PixelBuf makeJungleTile(int variant) {
    PixelBuf p(32, 32);
    p.fill(col(0x2E4A20));
    Rng rng(0x1DFFu + variant * 101);
    for (int i = 0; i < 60; ++i)
        p.set(rng.rangeInt(0, 31), rng.rangeInt(0, 31), col(0x385A28));
    // Tree canopies.
    const int trees = 2 + (variant % 2);
    for (int t = 0; t < trees; ++t) {
        const int tx = rng.rangeInt(6, 26), ty = rng.rangeInt(6, 26);
        const float r = rng.range(4.0f, 6.5f);
        p.fillEllipse(static_cast<float>(tx), static_cast<float>(ty), r, r, col(0x1E3814));
        p.fillEllipse(static_cast<float>(tx - 1), static_cast<float>(ty - 1), r * 0.7f, r * 0.7f, col(0x2A4A1C));
        p.fillEllipse(static_cast<float>(tx - 2), static_cast<float>(ty - 2), r * 0.35f, r * 0.35f, col(0x466A2E));
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
    PixelBuf p(32, 32);
    p.fill(col(0x84847E));
    Rng rng(0xAF1);
    for (int i = 0; i < 40; ++i)
        p.set(rng.rangeInt(0, 31), rng.rangeInt(0, 31), col(0x787872));
    p.hline(0, 31, 0, col(0x6A6A64));
    p.vline(0, 0, 31, col(0x6A6A64));
    p.fillRect(4, 14, 24, 4, kWhite);   // runway stripe
    return p;
}

PixelBuf makeFortressTile() {
    PixelBuf p(32, 32);
    p.fill(kConcreteDk);
    for (int y = 0; y < 32; y += 8) {
        const int off = (y / 8) % 2 == 0 ? 0 : 8;
        for (int x = -8; x < 32; x += 16) {
            p.fillRect(x + off + 1, y + 1, 14, 6, shade(kConcrete, 0.95f));
            p.fillRect(x + off + 1, y + 1, 14, 1, shade(kConcrete, 1.15f));
        }
    }
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
// Battleship Yamashiro-class: 176x128 canvas, hull centred, bow pointing
// DOWN toward the player. damage: 0 intact, 1 damaged, 2 wreck.
// The canvas is padded beyond the 160x96 hitbox so the water shadow and
// foam can sell scale (hitbox is defined in JSON and is unaffected).
// ---------------------------------------------------------------------------
inline float shipHalfWidth(float t) {
    // t: 0 = stern (top) .. 1 = bow tip (bottom).
    if (t < 0.06f) return 26.0f + 12.0f * std::sqrt(std::max(0.0f, t / 0.06f));
    if (t < 0.52f) return 38.0f;                       // broad waist
    if (t < 0.80f) return 38.0f - 15.0f * ((t - 0.52f) / 0.28f);
    return std::max(0.0f, 23.0f * (1.0f - (t - 0.80f) / 0.20f));
}

PixelBuf makeBattleshipHull(int damage) {
    const int W = 176, H = 128;
    const int cx = W / 2;
    const int yTop = 8, yBot = 120;
    const int L = yBot - yTop;
    PixelBuf p(W, H);
    auto halfAt = [&](int y) {
        return shipHalfWidth(static_cast<float>(y - yTop) / static_cast<float>(L));
    };

    // --- water shadow (light from upper-left => offset down-right) --------
    for (int y = yTop; y <= yBot + 4; ++y) {
        const int hw = static_cast<int>(halfAt(std::min(y - 4, yBot))) + 3;
        for (int x = -hw; x <= hw; ++x)
            p.set(cx + x + 4, y + 5, col(0x06121E, 96));
    }
    // --- bow wave & side foam ---------------------------------------------
    for (int k = 0; k < 46; ++k) {
        const float a = static_cast<float>(k) / 45.0f;
        const float bowY = yBot - 4.0f - 30.0f * a;
        const float spread = 6.0f + 26.0f * a;
        const std::uint32_t foam = col(0xE8F2FA, static_cast<std::uint8_t>(190 - 150 * a));
        for (int sx : {-1, 1}) {
            const int fx = cx + static_cast<int>(sx * (halfAt(static_cast<int>(bowY)) + spread));
            p.set(fx, static_cast<int>(bowY), foam);
            p.set(fx + sx, static_cast<int>(bowY) + 1, col(0xC8DCF0, 110));
        }
    }
    for (int y = yTop + 6; y < yBot - 8; y += 3) {
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw - 2, y, col(0xC8DCF0, 70));
        p.set(cx + hw + 2, y + 1, col(0xC8DCF0, 70));
    }

    // --- hull body ---------------------------------------------------------
    for (int y = yTop; y <= yBot; ++y) {
        const int hw = static_cast<int>(halfAt(y));
        for (int x = -hw; x <= hw; ++x) {
            const int ax = cx + x;
            std::uint32_t c = col(0x666D73);              // hull mid grey
            const int dEdge = hw - std::abs(x);
            if (dEdge <= 1) c = col(0x33383D);            // dark gunwale edge
            else if (dEdge <= 3) c = col(0x4A5056);       // belt
            // Counter-shading: light from upper-left.
            else if (x < 0 && y < yTop + L / 2) c = col(0x71787E);
            p.set(ax, y, c);
        }
    }
    // Armor-belt seams along both sides.
    for (int y = yTop + 4; y < yBot - 4; y += 11) {
        const int hw = static_cast<int>(halfAt(y));
        p.set(cx - hw + 2, y, col(0x2E3338));
        p.set(cx + hw - 2, y, col(0x2E3338));
    }

    // --- teak main deck ----------------------------------------------------
    for (int y = yTop + 3; y <= yBot - 3; ++y) {
        const int hw = static_cast<int>(halfAt(y)) - 5;
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x6E5636);
            if ((y % 3) == 0) c = col(0x5C4728);          // plank seam
            else if ((x + y) % 13 == 0) c = col(0x7E6540);
            p.set(cx + x, y, c);
        }
    }
    // Deck centerline spine.
    for (int y = yTop + 6; y < yBot - 6; ++y)
        if (std::abs(y % 4) != 3) p.set(cx, y, col(0x84693F));

    // --- barbette rings (remain visible when turrets are destroyed) --------
    for (int dy : {-32, 30}) {
        const float by = static_cast<float>(H / 2 + dy);
        p.fillEllipse(static_cast<float>(cx), by, 15.0f, 15.0f, col(0x3C4247));
        p.fillEllipse(static_cast<float>(cx), by, 12.0f, 12.0f, col(0x565C61));
        p.fillEllipse(static_cast<float>(cx), by - 1, 9.0f, 9.0f, col(0x2E3338));
        for (int r = 0; r < 8; ++r) {                     // ring bolts
            const float a = r * 0.7854f;
            p.set(cx + static_cast<int>(13.5f * std::cos(a)),
                  static_cast<int>(by + 13.5f * std::sin(a)), col(0x7A8085));
        }
    }

    // --- funnels (two, starboard of centreline) with catwalk ---------------
    for (int fyy : {54, 74}) {
        const float fy = static_cast<float>(H / 2 + fyy - 64);
        p.fillEllipse(static_cast<float>(cx + 14), fy, 6.5f, 7.5f, col(0x33383D));
        p.fillEllipse(static_cast<float>(cx + 14), fy, 5.0f, 6.0f, col(0x1B1E21));
        p.fillEllipse(static_cast<float>(cx + 13), fy - 2, 2.5f, 2.5f, col(0x4E5459));
    }
    p.hline(cx + 8, cx + 20, H / 2, col(0x8A9094, 160));      // catwalk

    // --- casemate secondary guns along the waist ----------------------------
    for (int dy : {-14, -6, 2, 10}) {
        const int y = H / 2 + dy;
        for (int sx : {-1, 1}) {
            const int hw = static_cast<int>(halfAt(y));
            const int gx = cx + sx * (hw - 3);
            p.fillRect(gx - 1, y - 2, 3, 5, col(0x2B2F33));
            p.set(gx + sx * 2, y, col(0x17191B));           // barrel tip
        }
    }

    // --- bow details: anchor housings, hawsepipes --------------------------
    for (int sx : {-1, 1}) {
        const int ax = cx + sx * 14;
        p.set(ax, yBot - 18, col(0x1E2226));
        p.set(ax + sx, yBot - 17, col(0x1E2226));
        p.fillRect(ax - 2, yBot - 26, 5, 3, col(0x4A5056));
    }
    // Stern: depth-charge racks + mooring dots.
    p.fillRect(cx - 10, yTop + 2, 4, 7, col(0x2B2F33));
    p.fillRect(cx + 7, yTop + 2, 4, 7, col(0x2B2F33));
    for (int sx : {-16, 16}) p.set(cx + sx, yTop + 4, col(0x8A9094));
    // Crane on the starboard waist.
    p.vline(cx + 24, H / 2 + 16, H / 2 + 26, col(0x3C4247));
    p.hline(cx + 20, cx + 30, H / 2 + 16, col(0x565C61));
    // Rust streaks for lived-in wear.
    for (int k = 0; k < 14; ++k) {
        const int rx = cx - 30 + (k * 47) % 61;
        const int ry = yTop + 12 + (k * 83) % (L - 24);
        p.set(rx, ry, col(0x6E4A2C, 120));
        p.set(rx, ry + 1, col(0x6E4A2C, 70));
    }

    // --- damage states ------------------------------------------------------
    if (damage >= 1) {
        struct Scorch { int dx, dy, r; };
        const Scorch sc1[] = {{20, 20, 8}, {-8, 40, 7}, {26, -14, 6}, {-24, 24, 7}};
        for (const auto& s : sc1) {
            p.fillEllipse(static_cast<float>(cx + s.dx), static_cast<float>(H / 2 + s.dy),
                          static_cast<float>(s.r), static_cast<float>(s.r), col(0x14100C, 140));
            p.fillEllipse(static_cast<float>(cx + s.dx), static_cast<float>(H / 2 + s.dy),
                          static_cast<float>(s.r) * 0.5f, static_cast<float>(s.r) * 0.5f,
                          col(0x0A0806, 190));
            p.set(cx + s.dx, H / 2 + s.dy, col(0xFF8A30));      // ember
            p.set(cx + s.dx + 1, H / 2 + s.dy, col(0xC25A18));
        }
        // Smoke staining drifting up from burns.
        for (const auto& s : sc1)
            for (int q = 1; q <= 6; ++q)
                p.fillEllipse(static_cast<float>(cx + s.dx - q / 2),
                              static_cast<float>(H / 2 + s.dy - s.r - q * 2),
                              2.0f, 2.0f, col(0x2A2A2A, 70 - q * 9));
    }
    if (damage >= 2) {
        struct Scorch2 { int dx, dy, r; };
        const Scorch2 sc2[] = {{-18, -24, 10}, {12, -40, 8}, {-2, 12, 9}, {30, 34, 8}};
        for (const auto& s : sc2) {
            p.fillEllipse(static_cast<float>(cx + s.dx), static_cast<float>(H / 2 + s.dy),
                          static_cast<float>(s.r), static_cast<float>(s.r), col(0x0E0B08, 170));
            p.fillEllipse(static_cast<float>(cx + s.dx), static_cast<float>(H / 2 + s.dy),
                          static_cast<float>(s.r) * 0.45f, static_cast<float>(s.r) * 0.45f,
                          col(0x060504, 220));
            p.set(cx + s.dx - 1, H / 2 + s.dy, col(0xFFC23A));
            p.set(cx + s.dx, H / 2 + s.dy + 1, col(0xFF8A30));
            p.set(cx + s.dx + 2, H / 2 + s.dy - 1, col(0xC25A18));
        }
        // Hull breach on the starboard waist: dark jagged gap + red interior.
        for (int i = 0; i < 10; ++i) {
            const int by = H / 2 + 2 + i;
            const int hw = static_cast<int>(halfAt(by));
            for (int x = hw - 6 + (i % 3); x <= hw; ++x) {
                p.set(cx + x, by, col(0x141210));
                if ((i + x) % 3 == 0) p.set(cx + x - 1, by, col(0x8A2A10));
            }
        }
    }
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

// Main gun turret 40x40: octagonal armored face on a barbette ring, twin
// barrels with muzzle bands, flat facets lit from the upper-left.
PixelBuf makeBigTurret() {
    PixelBuf p(40, 40);
    const float cx = 20.0f, cy = 20.0f;
    // Barbette ring.
    p.fillEllipse(cx, cy + 1, 17.5f, 17.5f, col(0x2E3338));
    p.fillEllipse(cx, cy, 16.5f, 16.5f, col(0x4A5056));
    p.fillEllipse(cx, cy, 14.5f, 14.5f, col(0x33383D));
    // Octagonal turret body (scanline widths).
    const int oct[16] = {6, 10, 13, 14, 14, 14, 14, 13, 13, 14, 14, 14, 14, 13, 10, 6};
    for (int r = 0; r < 16; ++r) {
        const int y = 12 + r;
        const int hw = oct[static_cast<std::size_t>(r)];
        for (int x = -hw; x <= hw; ++x) {
            std::uint32_t c = col(0x666D73);
            const int dEdge = hw - std::abs(x);
            if (dEdge <= 1) c = col(0x2E3338);                 // dark facet edge
            else if (x <= 0 && r <= 8) c = col(0x7A8187);      // lit facets
            else if (x >= 0 && r > 8) c = col(0x51575C);       // shaded facets
            p.set(static_cast<int>(cx) + x, y, c);
        }
    }
    // Roof hatch + sight.
    p.fillEllipse(cx, cy - 3, 4.0f, 3.0f, col(0x51575C));
    p.fillEllipse(cx, cy - 4, 2.0f, 1.5f, col(0x8A9094));
    p.set(static_cast<int>(cx) + 6, static_cast<int>(cy) - 6, col(0x1B1E21));
    // Twin barrels (point down = toward player), with recoil sleeves.
    for (int bx : {15, 22}) {
        p.fillRect(bx, 24, 3, 14, col(0x17191B));
        p.fillRect(bx, 24, 1, 14, col(0x4E5459));              // lit edge
        p.fillRect(bx - 1, 26, 5, 3, col(0x2B2F33));           // mantlet boot
        p.fillRect(bx, 35, 3, 2, col(0x33383D));               // muzzle band
    }
    // Mantlet bar across the barrel bases.
    p.fillRect(13, 27, 14, 3, col(0x3C4247));
    p.fillRect(13, 27, 14, 1, col(0x666D73));
    return p;
}

// AA mount 24x24: octagonal platform, shielded twin barrels pointing up.
PixelBuf makeBossAA() {
    PixelBuf p(24, 24);
    const float cx = 12.0f, cy = 14.0f;
    p.fillEllipse(cx, cy + 1, 10.5f, 9.0f, col(0x2B2F33));
    p.fillEllipse(cx, cy, 9.0f, 8.0f, col(0x4E5459));
    p.fillEllipse(cx - 1, cy - 1, 6.0f, 5.0f, col(0x666D73));
    p.fillEllipse(cx - 2, cy - 2, 3.0f, 2.5f, col(0x7E858B));
    // Gun shield (front arc toward the player).
    for (int x = -8; x <= 8; ++x)
        for (int y = 5; y <= 8; ++y)
            if (x * x + (y - 14) * (y - 14) <= 81 && x * x + (y - 14) * (y - 14) >= 49)
                p.set(static_cast<int>(cx) + x, y, y <= 6 ? col(0x7A8187) : col(0x51575C));
    // Twin barrels with flash guards.
    for (int bx : {9, 13}) {
        p.fillRect(bx, 1, 2, 13, col(0x17191B));
        p.fillRect(bx, 1, 1, 13, col(0x4E5459));
        p.fillRect(bx - 1, 1, 4, 2, col(0x2B2F33));
    }
    // Seat box + ammo drum behind.
    p.fillRect(10, 15, 5, 4, col(0x3C4247));
    p.fillEllipse(17.0f, 16.0f, 2.5f, 2.5f, col(0x2B2F33));
    return p;
}

// Pagoda bridge 36x44: tiered command structure with rangefinder, windows,
// tripod mast and ensign yard.
PixelBuf makeBridge() {
    PixelBuf p(36, 44);
    const int cx = 18;
    // Base deck plate.
    p.fillRect(3, 34, 30, 8, col(0x4A5056));
    p.fillRect(3, 34, 30, 1, col(0x7A8187));
    p.fillRect(3, 41, 30, 1, col(0x2E3338));
    // Lower tier.
    p.fillRect(6, 24, 24, 11, col(0x5C6368));
    p.fillRect(6, 24, 24, 1, col(0x8A9094));
    p.fillRect(30, 24, 1, 11, col(0x33383D));
    // Pagoda roof 1 (overhanging eaves).
    p.fillRect(4, 22, 28, 2, col(0x2E3338));
    p.fillRect(5, 21, 26, 1, col(0x666D73));
    // Middle tier with window band.
    p.fillRect(9, 13, 18, 9, col(0x666D73));
    p.fillRect(9, 13, 18, 1, col(0x8A9094));
    p.fillRect(26, 13, 1, 9, col(0x33383D));
    for (int x = 11; x <= 23; x += 3) {
        p.fillRect(x, 15, 2, 3, col(0x3A5A6E));             // glass
        p.set(x, 15, col(0x7FB8D8));                        // reflection
    }
    // Pagoda roof 2.
    p.fillRect(7, 11, 22, 2, col(0x2E3338));
    p.fillRect(8, 10, 20, 1, col(0x666D73));
    // Top control tier + rangefinder cupola.
    p.fillRect(13, 4, 10, 7, col(0x5C6368));
    p.fillRect(13, 4, 10, 1, col(0x8A9094));
    p.fillEllipse(18.0f, 3.0f, 5.0f, 2.5f, col(0x33383D));  // rangefinder
    p.hline(11, 25, 3, col(0x4E5459));                      // range arms
    p.set(11, 2, col(0x8A9094));
    p.set(25, 2, col(0x8A9094));
    // Tripod mast and ensign yard.
    p.vline(cx, 8, 40, col(0x2E3338));
    p.vline(cx - 1, 8, 40, col(0x666D73));
    p.hline(cx - 6, cx + 6, 9, col(0x4E5459));
    p.fillRect(cx + 1, 6, 5, 3, col(0xC23A2A));              // ensign
    // Searchlight on the base deck.
    p.fillEllipse(8.0f, 37.0f, 3.0f, 3.0f, col(0x33383D));
    p.fillEllipse(8.0f, 36.5f, 2.0f, 2.0f, col(0xF2E8B8));
    return p;
}

// Sky Fortress GX-7 144x96: four-engine heavy bomber, drawn nose-DOWN
// (it faces the player), full-span wings, gun blisters, prop blur discs.
PixelBuf makeGiantBomberHull(int damage) {
    const int W = 144, H = 96;
    const int cx = W / 2;
    PixelBuf p(W, H);

    // --- wings: high straight span, rounded tips ---------------------------
    const int wingTop = 34, wingBot = 60;
    for (int x = 4; x < W - 4; ++x) {
        const int tipIn = std::min(x - 4, W - 5 - x);
        const int tInset = tipIn < 8 ? (8 - tipIn) / 2 : 0;
        for (int y = wingTop + tInset; y <= wingBot - tInset; ++y) {
            std::uint32_t c = col(0x5A5F63);                 // wing mid
            if (y == wingTop) c = col(0x7E838A);             // leading edge lit
            else if (y >= wingBot - 1) c = col(0x3C4046);    // trailing edge
            else if ((x * 7 + y) % 23 == 0) c = col(0x51565B);
            p.set(x, y, c);
        }
        // Panel line every 8px.
        if (x % 8 == 0)
            for (int y = wingTop + 1; y < wingBot - 1; ++y)
                if (p.get(x, y) && (y != wingTop)) p.set(x, y, col(0x484D52));
    }
    // Enemy roundels on both wings.
    for (int rx : {30, 114}) {
        p.fillEllipse(static_cast<float>(rx), 47.0f, 7.0f, 7.0f, col(0xC23A2A));
        p.set(rx - 7, 47, col(0xF2F2F2));
        p.set(rx + 7, 47, col(0xF2F2F2));
        p.set(rx, 40, col(0xF2F2F2));
        p.set(rx, 54, col(0xF2F2F2));
    }

    // --- fuselage (nose toward the player = bottom) -------------------------
    for (int y = 6; y <= 90; ++y) {
        const float t = static_cast<float>(y - 6) / 84.0f;  // 0 tail .. 1 nose
        float hw = 9.0f;
        if (t > 0.80f) hw = 9.0f * (1.0f - (t - 0.80f) / 0.20f * 0.55f);
        if (t < 0.10f) hw = 4.0f + 5.0f * (t / 0.10f);
        for (int x = -static_cast<int>(hw); x <= static_cast<int>(hw); ++x) {
            std::uint32_t c = col(0x666B70);
            const int dEdge = static_cast<int>(hw) - std::abs(x);
            if (dEdge <= 1) c = col(0x3C4046);
            else if (x < 0) c = col(0x767B81);               // top-of-left lit
            p.set(cx + x, y, c);
        }
    }
    // Cockpit greenhouse toward the nose.
    p.fillRect(cx - 4, 66, 9, 8, col(0x3A5A6E));
    p.vline(cx - 1, 66, 73, col(0x2A4454));
    p.vline(cx + 2, 66, 73, col(0x2A4454));
    p.hline(cx - 4, cx + 4, 70, col(0x2A4454));
    p.set(cx - 3, 67, col(0x9FD0E8));                        // glass shine
    p.set(cx - 2, 67, col(0x7FB8D8));
    // Nose gun blister.
    p.fillEllipse(static_cast<float>(cx), 82.0f, 3.5f, 3.5f, col(0x3C4046));
    p.fillRect(cx, 84, 2, 5, col(0x17191B));
    // Dorsal + radio operator blisters.
    p.fillEllipse(static_cast<float>(cx), 44.0f, 3.5f, 4.5f, col(0x4E5459));
    p.fillEllipse(static_cast<float>(cx), 43.0f, 2.0f, 2.5f, col(0x8A9094));
    p.fillEllipse(static_cast<float>(cx), 26.0f, 2.5f, 3.0f, col(0x4E5459));
    // Machine-gun ports along the fuselage flanks.
    for (int y : {34, 40, 52, 58}) {
        p.set(cx - 9, y, col(0x2B2F33));
        p.set(cx + 9, y, col(0x2B2F33));
    }

    // --- four engine nacelles with prop blur -------------------------------
    for (int ex : {36, 54, 90, 108}) {
        const int nx = ex;
        // Exhaust staining behind each nacelle (toward the tail = up).
        for (int q = 0; q < 10; ++q)
            p.set(nx + 2 + q / 3, 30 - q, col(0x1A1D20, 80 - q * 7));
        // Nacelle body.
        p.fillRect(nx - 4, 36, 9, 22, col(0x4E5459));
        p.fillRect(nx - 4, 36, 2, 22, col(0x71787E));
        p.fillRect(nx + 3, 36, 2, 22, col(0x33383D));
        p.fillRect(nx - 3, 56, 7, 3, col(0x33383D));         // cowl
        // Prop disc (spinning blur) ahead of the cowl (toward the player).
        p.fillEllipse(static_cast<float>(nx + 0.5f), 62.0f, 9.0f, 3.5f,
                      col(0xB8C4CC, 60));
        p.fillEllipse(static_cast<float>(nx + 0.5f), 62.0f, 6.0f, 2.0f,
                      col(0xD8E4EC, 50));
        p.fillRect(nx, 58, 2, 3, col(0x2B2F33));             // hub
        // Blade streaks.
        p.hline(nx - 8, nx + 9, 62, col(0xE8F2FA, 40));
    }

    // --- tail group ---------------------------------------------------------
    // Horizontal stabilizers.
    for (int x = cx - 30; x <= cx + 30; ++x) {
        const int tipIn = std::min(x - (cx - 30), (cx + 30) - x);
        const int ins = tipIn < 4 ? (4 - tipIn) : 0;
        for (int y = 10 + ins; y <= 18 - ins; ++y) {
            std::uint32_t c = col(0x51565B);
            if (y == 10 + ins) c = col(0x767B81);
            if (y >= 17 - ins) c = col(0x3C4046);
            p.set(x, y, c);
        }
    }
    // Tall vertical fin (rendered as a dark central keel with shading).
    p.fillRect(cx - 2, 2, 5, 16, col(0x3C4046));
    p.fillRect(cx - 2, 2, 2, 16, col(0x5C6368));
    p.set(cx, 2, col(0x8A9094));
    p.fillRect(cx - 1, 0, 2, 3, col(0xC23A2A));              // fin flash

    return addScorchTo(std::move(p), damage);
}

// Wake foam trail (drawn behind the battleship's stern).
PixelBuf makeWake() {
    const int W = 64, H = 104;
    PixelBuf p(W, H);
    // Foam widens toward the bottom (stern) and dissipates upward.
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / H;          // 0 far .. 1 stern
        const float w = 2.0f + 26.0f * t * t;
        const int a = static_cast<int>(140.0f * t * (1.0f - (1.0f - t) * 0.25f));
        for (int sx : {-1, 1}) {
            const int fx = W / 2 + static_cast<int>(sx * w);
            p.set(fx, y, col(0xE8F2FA, static_cast<std::uint8_t>(a)));
            if (y % 2 == 0)
                p.set(fx + sx, y, col(0xC8DCF0, static_cast<std::uint8_t>(a / 2)));
        }
        // Churned water core.
        if (y % 3 == 0 && t > 0.25f)
            p.set(W / 2 + ((y * 7) % 9) - 4, y,
                  col(0x9CC8E8, static_cast<std::uint8_t>(60 * t)));
    }
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
    addDown("enemy_heavy_fighter", {40, 30, kEnGreen, shade(kEnGreen, 1.2f), kEnGreenDk, true});
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
    add("powerup_p", makePowerup('P', col(0x2E7A2E), col(0x183A18)));
    add("powerup_spread", makePowerup('S', col(0x9A3A8A), col(0x4A1842)));
    add("powerup_cannon", makePowerup('C', col(0x8A4A1A), col(0x42220C)));
    add("powerup_rocket", makePowerup('R', col(0x8A2A2A), col(0x421414)));
    add("powerup_bomb", makePowerup('B', col(0x2A4A9A), col(0x142242)));
    add("powerup_repair", makePowerup('+', col(0xD8D8D0), col(0x606058)));
    add("powerup_life", makePowerup('L', col(0xC23A2A), col(0x5A1A12)));

    // Effects.
    add("explosion_small", makeExplosionSheet(8, 24, 0xE5A1u, false));
    add("explosion_medium", makeExplosionSheet(8, 40, 0xE5B2u, false));
    add("explosion_large", makeExplosionSheet(8, 64, 0xE5C3u, false));
    add("splash_small", makeExplosionSheet(6, 24, 0x5D1Au, true));
    add("splash_large", makeExplosionSheet(6, 48, 0x5D2Bu, true));
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
    add("tile_fortress", makeFortressTile());
    add("tile_island_edge", makeGrassEdgeTile());

    // Boss parts.
    add("boss_battleship_hull", makeBattleshipHull(0));
    add("boss_battleship_hull_dmg", makeBattleshipHull(1));
    add("boss_battleship_hull_wreck", makeBattleshipHull(2));
    add("boss_big_turret", makeBigTurret());
    add("boss_aa", makeBossAA());
    add("boss_bridge", makeBridge());
    add("boss_bomber_hull", makeGiantBomberHull(0));
    add("boss_bomber_hull_dmg", makeGiantBomberHull(2));
    add("boss_turret_small", makeBossAA());
    add("wake_foam", makeWake());
    add("fire_puff", makeFirePuff());
    add("smoke_puff", makeSmokePuff());

    return true;
}

void loadPngOverrides(Renderer& renderer, const std::string& assetDir) {
    namespace fs = std::filesystem;
    static const char* names[] = {
        "player_fighter", "player_fighter_l", "player_fighter_r",
        "enemy_fighter_light", "enemy_interceptor", "enemy_interceptor_m",
        "enemy_heavy_fighter", "enemy_bomber", "turret_aa", "tank", "bunker",
        "boat_patrol", "ship_destroyer", "bullet_player", "bullet_cannon",
        "bullet_rocket", "bullet_enemy", "shell_enemy", "flak_enemy",
        "powerup_p", "powerup_spread", "powerup_cannon", "powerup_rocket",
        "powerup_bomb", "powerup_repair", "powerup_life",
        "explosion_small", "explosion_medium", "explosion_large",
        "splash_small", "splash_large", "muzzle_flash",
        "boss_battleship_hull", "boss_battleship_hull_dmg",
        "boss_battleship_hull_wreck", "boss_big_turret", "boss_aa",
        "boss_bridge", "boss_bomber_hull", "boss_bomber_hull_dmg",
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
