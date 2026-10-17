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
    bool amberMark = false; // elite accent (amber nose band + wingtip caps)
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
    // Elite accent: amber nose band + wingtip caps (matches the boss
    // hazard-ring language so tough targets read instantly).
    if (st.amberMark) {
        const std::uint32_t amber = col(0xC8A83C);
        for (int x = -3; x <= 3; ++x) p.set(cx + x, cy - st.len / 2 + 4, amber);
        for (int side = -1; side <= 1; side += 2) {
            const int tipShift = static_cast<int>(bankRows * side);
            for (int d = half - 1; d <= half; ++d)
                for (int c = 0; c < 3; ++c)
                    if (p.get(cx + side * d, wingY + c + tipShift))
                        p.set(cx + side * d, wingY + c + tipShift, amber);
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

// ---------------------------------------------------------------------------
// Boss-death explosion: 6 frames x 128px. White detonation core + expanding
// shockwave ring + lobed fireball + flung embers, collapsing into smoke.
// ---------------------------------------------------------------------------
PixelBuf makeBossExplosionSheet() {
    const int FR = 6, S = 128;
    PixelBuf sheet(FR * S, S);
    Rng rng(0xB5DEA1u);
    for (int f = 0; f < FR; ++f) {
        const float t = static_cast<float>(f) / (FR - 1);
        const float cx = f * S + S * 0.5f, cy = S * 0.5f;
        // Organic lobes, stable across frames.
        float lobes[24];
        {
            Rng lr(0xB055u + static_cast<std::uint64_t>(f) * 13u);
            for (int k = 0; k < 24; ++k) lobes[k] = lr.range(0.78f, 1.16f);
        }
        const float fireR = 16.0f + 46.0f * std::sqrt(t);
        const float ringR = 12.0f + 104.0f * t;
        const int ringA = static_cast<int>(t < 0.55f ? 235.0f * (1.0f - t / 0.55f) : 0.0f);
        const int outA = t > 0.72f ? static_cast<int>(255.0f * (1.0f - t) / 0.28f) : 255;
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                int ang = static_cast<int>((std::atan2(dy, dx) + 3.15f) / 6.29f * 24.0f) % 24;
                const float edge = fireR * lobes[static_cast<std::size_t>(ang)];
                std::uint32_t c = 0;
                if (dist <= edge) {
                    const float te = dist / std::max(0.01f, edge);
                    if (t < 0.16f)
                        c = te < 0.82f ? col(0xFFFFF8) : col(0xFFE070);
                    else if (t < 0.42f)
                        c = te < 0.34f ? col(0xFFFCE0)
                          : te < 0.62f ? col(0xFFC23A)
                          : te < 0.86f ? col(0xF0641E) : col(0xC23018);
                    else if (t < 0.72f)
                        c = te < 0.22f ? col(0xFFD870)
                          : te < 0.55f ? col(0xE8681E)
                          : te < 0.82f ? col(0x9A3414) : col(0x4A2A1C);
                    else
                        c = te < 0.40f ? col(0xA8481A, 220)
                          : te < 0.72f ? col(0x5C3420, 200) : col(0x302A28, 170);
                    // Inner heat shimmer keeps the core alive late.
                    if (t >= 0.42f && t < 0.72f && te < 0.16f) c = col(0xFF8A30);
                }
                // Shockwave ring (outside the fireball).
                if (ringA > 0 && dist > edge) {
                    const float rd = std::abs(dist - ringR);
                    if (rd <= 2.5f)
                        c = col(t < 0.3f ? 0xFFFFFF : 0xFFCFA0,
                                static_cast<std::uint8_t>(ringA * (1.0f - rd / 3.0f)));
                }
                if (c != 0) {
                    if (outA < 255)  // global late fade
                        c = col((c >> 8) & 0xFFFFFF,
                                static_cast<std::uint8_t>((c & 0xFF) * outA / 255));
                    sheet.set(x + f * S, y, c);
                }
            }
        }
        // Flung embers on the early frames.
        if (t < 0.6f) {
            for (int k = 0; k < 18; ++k) {
                const float a = rng.range(0.0f, 6.28f);
                const float d = fireR * rng.range(1.05f, 1.5f + t * 1.4f);
                sheet.set(static_cast<int>(cx + std::cos(a) * d),
                          static_cast<int>(cy + std::sin(a) * d),
                          t < 0.3f ? col(0xFFF4C0, 240) : col(0xF07A20, 210));
            }
        }
        // Late smoke clumps.
        if (t > 0.6f) {
            for (int k = 0; k < 16; ++k) {
                const float a = k / 16.0f * 6.283f + 0.9f;
                const float d = fireR * rng.range(0.6f, 1.05f);
                const int sx = static_cast<int>(cx + std::cos(a) * d);
                const int sy = static_cast<int>(cy + std::sin(a) * d) - (f - 3) * 5;
                for (int q = 0; q < 4; ++q)
                    sheet.set(sx + (q % 2), sy + (q / 2),
                              col(0x34302E, static_cast<std::uint8_t>(120 - k * 4)));
            }
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
    // Amber hazard dashes (armored-target language shared with bosses).
    for (int k = 0; k < 16; ++k) {
        if (k % 2) continue;
        const float a = k * 0.3927f + 0.15f;
        p.set(static_cast<int>(cx + 14.5f * std::cos(a)),
              static_cast<int>(20.5f + 11.5f * std::sin(a)), col(0xC8A83C));
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
    add("wake_foam", makeWake());
    add("fire_puff", makeFirePuff());
    add("smoke_puff", makeSmokePuff());
    add("explosion_xlarge", makeExplosionSheet(8, 96, 0xE5D4u, false));
    add("boss_explosion", makeBossExplosionSheet());

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
        "explosion_xlarge", "boss_explosion",
        "splash_small", "splash_large", "muzzle_flash",
        "boss_battleship_hull", "boss_battleship_hull_light",
        "boss_battleship_hull_dmg",
        "boss_battleship_hull_wreck", "boss_battleship_hull_flash",
        "boss_big_turret", "boss_aa",
        "boss_bridge", "boss_bomber_hull", "boss_bomber_hull_dmg",
        "boss_bomber_hull_flash",
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
