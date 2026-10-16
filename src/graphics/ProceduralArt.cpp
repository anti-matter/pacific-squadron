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
    } else {
        for (int y = -2; y <= 1; ++y)
            for (int x = -2; x <= 2; ++x)
                p.set(cx + x, cy - 6 + y, kCanopy);
    }

    // Markings.
    if (st.redDot) {
        for (int wing = 0; wing < 2; ++wing) {
            const int side = wing == 0 ? -1 : 1;
            const int mx = cx + side * (half * 2 / 3);
            p.fillEllipse(static_cast<float>(mx), static_cast<float>(wingY + 3), 3.5f, 3.5f, kRed);
        }
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
                    if (t < 0.35f)
                        c = tEdge < 0.45f ? col(0xFFFFE0) : (tEdge < 0.75f ? col(0xFFC23A) : col(0xE85A20));
                    else if (t < 0.7f)
                        c = tEdge < 0.35f ? col(0xFFD268) : (tEdge < 0.7f ? col(0xE86A20) : col(0x9A3A18));
                    else
                        c = tEdge < 0.5f ? col(0x8A3A18) : col(0x3A3A3A);
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
    p.fillEllipse(16, 20, 12, 9, kConcreteDk);
    p.fillEllipse(16, 18, 10, 8, kConcrete);
    p.fillEllipse(16, 17, 7, 5, shade(kConcrete, 0.8f));
    // Twin barrels pointing up.
    for (int bx : {13, 18}) {
        p.fillRect(bx, 2, 2, 16, kBlack);
        p.fillRect(bx, 2, 1, 16, shade(kBlack, 2.2f));
    }
    p.fillRect(11, 14, 10, 6, shade(kConcrete, 0.65f));
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
    PixelBuf p(32, 32);
    p.fill(col(0x1A3C6E));
    Rng rng(0x0CE4Au + variant * 77);
    for (int i = 0; i < 40; ++i) {
        const int x = rng.rangeInt(0, 31), y = rng.rangeInt(0, 31);
        p.set(x, y, col(0x24508A));
    }
    // Wave strokes.
    for (int i = 0; i < 3; ++i) {
        const int x = rng.rangeInt(0, 22), y = rng.rangeInt(0, 30);
        p.hline(x, x + 5, y, col(0x3A68A0));
        p.hline(x + 1, x + 4, y - 1, col(0x2C5890));
    }
    if (variant == 2) {
        const int x = rng.rangeInt(4, 20), y = rng.rangeInt(4, 20);
        p.hline(x, x + 8, y, col(0x4C7CB8));
        p.hline(x + 1, x + 7, y + 1, col(0x2C5890));
    }
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
PixelBuf makeBattleshipHull() {
    const int W = 160, H = 96;
    PixelBuf p(W, H);
    // Hull pointing down.
    for (int y = 0; y < H; ++y) {
        const float t = static_cast<float>(y) / H;
        float halfW = 34.0f;
        if (t < 0.18f) halfW = 34.0f * (t / 0.18f) * 0.9f + 3;
        if (t > 0.8f) halfW = 34.0f * (1.0f - (t - 0.8f) * 3.0f);
        halfW = std::max(4.0f, halfW);
        for (int x = -static_cast<int>(halfW); x <= static_cast<int>(halfW); ++x) {
            std::uint32_t c = kNavalGrey;
            if (std::abs(x) > halfW - 3) c = kNavalDk;
            p.set(W / 2 + x, y, c);
        }
    }
    // Deck plating & details.
    for (int y = 8; y < H - 6; y += 3)
        for (int x = W / 2 - 28; x <= W / 2 + 28; ++x)
            if ((x + y) % 7 == 0) p.set(x, y, shade(kNavalGrey, 1.12f));
    // Superstructure mid.
    p.fillRect(W / 2 - 16, H / 2 - 10, 32, 24, shade(kNavalGrey, 1.25f));
    p.fillRect(W / 2 - 12, H / 2 - 6, 24, 16, shade(kNavalGrey, 1.1f));
    // Deck guns (static decoration; real turrets are components).
    for (int dy : {-34, 30}) {
        p.fillEllipse(static_cast<float>(W / 2), static_cast<float>(H / 2 + dy), 6.0f, 5.0f, kNavalDk);
    }
    return p;
}

PixelBuf makeBigTurret() {
    PixelBuf p(40, 40);
    p.fillEllipse(20, 22, 14, 12, kNavalDk);
    p.fillEllipse(20, 20, 12, 10, shade(kNavalGrey, 1.15f));
    for (int bx : {14, 22}) {
        p.fillRect(bx, 24, 4, 16, kBlack);
        p.fillRect(bx, 24, 1, 16, shade(kBlack, 2.0f));
    }
    return p;
}

PixelBuf makeBossAA() {
    PixelBuf p(24, 24);
    p.fillEllipse(12, 13, 9, 8, kEnGreyDk);
    p.fillEllipse(12, 12, 7, 6, kEnGrey);
    for (int bx : {9, 13}) p.fillRect(bx, 1, 2, 11, kBlack);
    return p;
}

PixelBuf makeBridge() {
    PixelBuf p(36, 44);
    p.fillRect(4, 6, 28, 36, shade(kNavalGrey, 1.2f));
    p.fillRect(8, 10, 20, 12, kNavalDk);
    p.fillRect(10, 12, 16, 3, col(0x30404A));   // windows
    p.fillRect(12, 2, 12, 4, kNavalDk);         // rangefinder
    p.vline(18, 24, 42, shade(kNavalGrey, 1.4f)); // mast
    p.hline(12, 24, 30, shade(kNavalGrey, 1.4f));
    return p;
}

PixelBuf makeGiantBomberHull() {
    const int W = 144, H = 96;
    PixelBuf p(W, H);
    const PlaneStyle st{128, 88, kEnGrey, shade(kEnGrey, 1.25f), kEnGreyDk, true, true};
    PixelBuf plane = makePlane(st, 0);
    p.blit(plane, (W - plane.w) / 2, (H - plane.h) / 2);
    // Extra fuselage width for the "flying fortress" feel.
    p.fillRect(W / 2 - 10, H / 2 - 34, 20, 68, kEnGrey);
    p.fillRect(W / 2 - 10, H / 2 - 34, 3, 68, shade(kEnGrey, 1.3f));
    p.fillRect(W / 2 + 7, H / 2 - 34, 3, 68, kEnGreyDk);
    p.fillEllipse(W / 2, H / 2 - 30, 9.0f, 7.0f, shade(kEnGrey, 1.35f));
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
    add("boss_battleship_hull", makeBattleshipHull());
    add("boss_big_turret", makeBigTurret());
    add("boss_aa", makeBossAA());
    add("boss_bridge", makeBridge());
    add("boss_bomber_hull", makeGiantBomberHull());
    add("boss_turret_small", makeBossAA());

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
        "boss_battleship_hull", "boss_big_turret", "boss_aa", "boss_bridge",
        "boss_bomber_hull",
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
