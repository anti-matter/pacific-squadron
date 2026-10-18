#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "audio/ProceduralSfx.h"

using namespace ww;

namespace {

float peakOf(const SoundBuffer& s) {
    float m = 0.0f;
    for (float v : s.samples) m = std::max(m, std::fabs(v));
    return m;
}
float rmsOf(const SoundBuffer& s) {
    double acc = 0.0;
    for (float v : s.samples) {
        const double d = static_cast<double>(v);
        acc += d * d;
    }
    return static_cast<float>(std::sqrt(acc / static_cast<double>(s.samples.size())));
}
float durOf(const SoundBuffer& s) {
    return static_cast<float>(s.samples.size()) / static_cast<float>(s.sampleRate);
}

// Strongest normalized autocorrelation of the sustained body (25%..75%)
// over lags corresponding to 40-735 Hz. A pitched/tone-driven sound spikes
// near 1.0; broadband noise stays well below. Guards the "no musical ding"
// requirement without being brittle about the rumble's slow wobble.
float tonalityOf(const SoundBuffer& s) {
    const auto& x = s.samples;
    const std::size_t n = x.size();
    const std::size_t a = n / 4, b = (n * 3) / 4;
    if (b - a < 1200) return 0.0f;
    double mean = 0.0;
    for (std::size_t i = a; i < b; ++i) mean += static_cast<double>(x[i]);
    mean /= static_cast<double>(b - a);
    double energy = 0.0;
    for (std::size_t i = a; i < b; ++i) {
        const double d = static_cast<double>(x[i]) - mean;
        energy += d * d;
    }
    float best = 0.0f;
    for (std::size_t lag = 60; lag <= 1100 && a + 1100 < b; ++lag) {
        double acc = 0.0;
        for (std::size_t i = a; i + lag < b; ++i)
            acc += (static_cast<double>(x[i]) - mean) *
                   (static_cast<double>(x[i + lag]) - mean);
        const float r = static_cast<float>(acc / (energy + 1e-9));
        if (r > best) best = r;
    }
    return best;
}

// Longest run of near-silence (10 ms blocks below 0.1% of the loudest
// block) before `until` seconds. One continuous catastrophe has none; a
// string of discrete drum-like hits is defined by the gaps between hits.
float longestSilenceOf(const SoundBuffer& s, float until) {
    const auto& x = s.samples;
    const auto sr = static_cast<std::size_t>(s.sampleRate);
    const std::size_t lim = std::min(x.size(), static_cast<std::size_t>(until) * sr);
    const std::size_t blk = sr / 100;  // 10 ms
    if (lim < blk * 2) return 0.0f;
    double peakE = 0.0;
    for (std::size_t i = 0; i + blk <= lim; i += blk) {
        double e = 0.0;
        for (std::size_t j = 0; j < blk; ++j)
            e += static_cast<double>(x[i + j]) * static_cast<double>(x[i + j]);
        peakE = std::max(peakE, e);
    }
    float run = 0.0f, best = 0.0f;
    for (std::size_t i = 0; i + blk <= lim; i += blk) {
        double e = 0.0;
        for (std::size_t j = 0; j < blk; ++j)
            e += static_cast<double>(x[i + j]) * static_cast<double>(x[i + j]);
        if (e < peakE * 0.001)
            run += 0.01f;
        else {
            best = std::max(best, run);
            run = 0.0f;
        }
    }
    return std::max(best, run);
}

}  // namespace

void test_sfx_explosions() {
    std::unordered_map<std::string, SoundBuffer> sfx;
    generateAllSfx(sfx);

    auto have = [&](const char* n) {
        CHECK(sfx.count(n) == 1 && !sfx.at(n).samples.empty());
    };
    for (int v = 1; v <= 3; ++v) {
        have(("explosion_small_" + std::to_string(v)).c_str());
        have(("explosion_medium_" + std::to_string(v)).c_str());
        have(("explosion_large_" + std::to_string(v)).c_str());
    }
    have("explosion_boss_1");
    have("explosion_boss_2");
    have("explosion");          // legacy aliases still resolve
    have("explosion_large");

    // Class durations: small 150-300 ms, medium 300-750 ms, large 0.6-1.4 s,
    // boss 1.5 s+ (layers make the class durations clearly distinct).
    for (int v = 1; v <= 3; ++v) {
        const auto& sm = sfx["explosion_small_" + std::to_string(v)];
        const auto& md = sfx["explosion_medium_" + std::to_string(v)];
        const auto& lg = sfx["explosion_large_" + std::to_string(v)];
        CHECK(durOf(sm) > 0.12f && durOf(sm) < 0.40f);
        CHECK(durOf(md) > 0.28f && durOf(md) < 0.80f);
        CHECK(durOf(lg) > 0.60f && durOf(lg) < 1.40f);
        CHECK(peakOf(sm) <= 0.96f && rmsOf(sm) > 0.015f);
        CHECK(peakOf(md) <= 0.96f && rmsOf(md) > 0.015f);
        CHECK(peakOf(lg) <= 0.96f && rmsOf(lg) > 0.015f);
        // The classes must be more than copies at different volume:
        // loudness hierarchy + genuinely longer structure.
        CHECK(durOf(md) > durOf(sm));
        CHECK(durOf(lg) > durOf(md));
    }
    // Boss finale: ONE catastrophic detonation of 3-5 s whose body never
    // falls silent (no gaps => no discrete hits / drum roll).
    for (int v = 1; v <= 2; ++v) {
        const auto& bs = sfx["explosion_boss_" + std::to_string(v)];
        CHECK(durOf(bs) > 3.2f && durOf(bs) < 5.0f);
        CHECK(peakOf(bs) <= 0.96f && rmsOf(bs) > 0.015f);
        CHECK(longestSilenceOf(bs, 2.6f) < 0.13f);
    }
    // Death-roll bed: continuous rumble+crackle, quieter than the blasts,
    // also gap-free while it runs.
    for (int v = 1; v <= 2; ++v) {
        const std::string n = "boss_breakup_" + std::to_string(v);
        CHECK(sfx.count(n) == 1 && !sfx[n].samples.empty());
        CHECK(durOf(sfx[n]) > 2.2f && durOf(sfx[n]) < 3.4f);
        CHECK(peakOf(sfx[n]) <= 0.70f && rmsOf(sfx[n]) > 0.010f);
        CHECK(longestSilenceOf(sfx[n], 2.2f) < 0.15f);
    }

    // Variants differ (not the same buffer under different names).
    const auto& a = sfx["explosion_medium_1"].samples;
    const auto& b = sfx["explosion_medium_2"].samples;
    bool differ = a.size() != b.size();
    if (!differ)
        for (std::size_t i = 0; i < a.size() && !differ; i += 97)
            differ = a[i] != b[i];
    CHECK(differ);

    // Non-tonal guard: explosions must not be dominated by a pitched tone.
    CHECK(tonalityOf(sfx["explosion_medium_1"]) < 0.85f);
    CHECK(tonalityOf(sfx["explosion_large_1"]) < 0.85f);
    CHECK(tonalityOf(sfx["explosion_boss_1"]) < 0.85f);

    // Other sfx keep existing (no regressions from the explosion pass).
    have("player_gun");
    have("enemy_gun");
    have("bomb");
    have("powerup");
    have("music_stage1");
}
