#include "audio/ProceduralSfx.h"

#include <algorithm>
#include <cmath>

#include "core/Rng.h"

namespace ww {
namespace {

constexpr float kRate = 44100.0f;
constexpr float kTau = 6.2831853f;

// --- tiny synth helpers ----------------------------------------------------
void addSweep(std::vector<float>& b, float t0, float dur, float f0, float f1,
              float amp, float decay, bool square, float duty = 0.5f) {
    const int s0 = static_cast<int>(t0 * kRate);
    const int n = static_cast<int>(dur * kRate);
    while (static_cast<int>(b.size()) < s0 + n) b.push_back(0.0f);
    float phase = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        const float f = f0 + (f1 - f0) * t;
        phase += f / kRate;
        float v = square ? (std::fmod(phase, 1.0f) < duty ? 1.0f : -1.0f)
                         : std::sin(kTau * phase);
        b[static_cast<std::size_t>(s0 + i)] += v * amp * std::exp(-decay * t);
    }
}

void addNoise(std::vector<float>& b, float t0, float dur, float amp, float decay,
              int smooth, Rng& rng) {
    const int s0 = static_cast<int>(t0 * kRate);
    const int n = static_cast<int>(dur * kRate);
    while (static_cast<int>(b.size()) < s0 + n) b.push_back(0.0f);
    float lp = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        float v = rng.nextFloat() * 2.0f - 1.0f;
        for (int s = 1; s < smooth; ++s) {
            lp = lp + (v - lp) * 0.5f;
            v = lp;
        }
        b[static_cast<std::size_t>(s0 + i)] += v * amp * std::exp(-decay * t);
    }
}

void normalize(std::vector<float>& b, float peak = 0.85f) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::abs(v));
    if (m < 1e-4f) return;
    const float s = peak / m;
    for (float& v : b) v *= s;
}

SoundBuffer finish(std::vector<float>&& b) {
    normalize(b);
    SoundBuffer out;
    out.samples = std::move(b);
    return out;
}

// --- music sequencer -------------------------------------------------------
struct MusicTrack {
    int bpm;
    // One bar = 16 steps; tracks repeat/transpose via the bar list.
    std::vector<std::vector<int>> melody;  // MIDI notes, 0 = rest
    std::vector<std::vector<int>> bass;
    std::vector<int> hat;  // 0 none, 1 hat, 2 open
};

SoundBuffer renderMusic(const MusicTrack& tr) {
    const float stepDur = 60.0f / static_cast<float>(tr.bpm) / 4.0f;  // 16ths
    const int totalSteps = static_cast<int>(tr.melody.size()) * 16;
    std::vector<float> mix;
    Rng rng(0xBEEFu);

    auto midiHz = [](int m) { return 440.0f * std::pow(2.0f, (m - 69) / 12.0f); };

    for (int bar = 0; bar < static_cast<int>(tr.melody.size()); ++bar) {
        for (int s = 0; s < 16; ++s) {
            const float t0 = (bar * 16 + s) * stepDur;
            const int mel = tr.melody[static_cast<std::size_t>(bar)][static_cast<std::size_t>(s)];
            if (mel > 0)
                addSweep(mix, t0, stepDur * 1.6f, midiHz(mel), midiHz(mel), 0.20f,
                         2.2f, true, 0.5f);
            const int bas = tr.bass[static_cast<std::size_t>(bar)][static_cast<std::size_t>(s)];
            if (bas > 0)
                addSweep(mix, t0, stepDur * 1.8f, midiHz(bas), midiHz(bas) * 0.995f,
                         0.30f, 1.6f, false);
            const int h = tr.hat[static_cast<std::size_t>(bar * 16 + s)];
            if (h > 0)
                addNoise(mix, t0, h == 2 ? 0.09f : 0.03f, h == 2 ? 0.10f : 0.07f,
                         h == 2 ? 18.0f : 60.0f, 14, rng);
        }
    }
    // Pad the loop boundary with a small tail.
    while (static_cast<int>(mix.size()) < static_cast<int>((totalSteps + 8) * stepDur * kRate))
        mix.push_back(0.0f);
    return finish(std::move(mix));
}

}  // namespace

void generateAllSfx(std::unordered_map<std::string, SoundBuffer>& out) {
    Rng rng(0xA0D10u);

    { std::vector<float> b;
      addSweep(b, 0, 0.07f, 1300, 320, 0.25f, 26, true, 0.35f);
      addNoise(b, 0, 0.05f, 0.10f, 40, 2, rng);
      out["player_gun"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0, 0.11f, 430, 130, 0.22f, 20, true, 0.5f);
      addNoise(b, 0, 0.06f, 0.08f, 35, 4, rng);
      out["enemy_gun"] = finish(std::move(b)); }

    { std::vector<float> b;
      addNoise(b, 0, 0.55f, 0.6f, 7, 6, rng);
      addSweep(b, 0, 0.4f, 150, 40, 0.4f, 8, false);
      out["explosion"] = finish(std::move(b)); }

    { std::vector<float> b;
      addNoise(b, 0, 1.1f, 0.8f, 4.5, 9, rng);
      addSweep(b, 0, 0.9f, 110, 28, 0.6f, 5, false);
      addSweep(b, 0.05f, 0.5f, 70, 22, 0.5f, 4, false);
      out["explosion_large"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0, 0.28f, 320, 70, 0.4f, 10, true, 0.7f);
      addNoise(b, 0, 0.15f, 0.2f, 25, 4, rng);
      out["player_damage"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0.00f, 0.07f, 660, 660, 0.3f, 8, true, 0.5f);
      addSweep(b, 0.07f, 0.07f, 880, 880, 0.3f, 8, true, 0.5f);
      addSweep(b, 0.14f, 0.11f, 1320, 1320, 0.3f, 8, true, 0.5f);
      out["powerup"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0, 0.04f, 520, 520, 0.25f, 30, true, 0.5f);
      out["menu_select"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0, 0.05f, 660, 660, 0.28f, 18, true, 0.5f);
      addSweep(b, 0.06f, 0.09f, 990, 990, 0.28f, 14, true, 0.5f);
      out["menu_confirm"] = finish(std::move(b)); }

    { std::vector<float> b;
      addNoise(b, 0, 0.5f, 0.35f, 5, 3, rng);
      addSweep(b, 0.35f, 0.7f, 120, 30, 0.6f, 5, false);
      addNoise(b, 0.35f, 0.6f, 0.7f, 6, 8, rng);
      out["bomb"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0, 0.18f, 1450, 1420, 0.3f, 22, false);
      addSweep(b, 0, 0.12f, 2150, 2100, 0.2f, 26, false);
      out["clang"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0.00f, 0.14f, 520, 520, 0.3f, 3, true, 0.5f);
      addSweep(b, 0.16f, 0.14f, 390, 390, 0.3f, 3, true, 0.5f);
      addSweep(b, 0.32f, 0.14f, 520, 520, 0.3f, 3, true, 0.5f);
      addSweep(b, 0.48f, 0.20f, 390, 390, 0.3f, 6, true, 0.5f);
      out["boss_phase"] = finish(std::move(b)); }

    { std::vector<float> b;
      addSweep(b, 0.00f, 0.12f, 523, 523, 0.3f, 4, true, 0.5f);
      addSweep(b, 0.13f, 0.12f, 659, 659, 0.3f, 4, true, 0.5f);
      addSweep(b, 0.26f, 0.12f, 784, 784, 0.3f, 4, true, 0.5f);
      addSweep(b, 0.39f, 0.35f, 1047, 1047, 0.32f, 5, true, 0.5f);
      out["boss_defeated"] = finish(std::move(b)); }

    out["formation_bonus"] = out["menu_confirm"];
    out["splash"] = out["player_damage"];

    // ---- music -------------------------------------------------------------
    // Stage 1: steady patrol march (A minor).
    MusicTrack t1;
    t1.bpm = 126;
    t1.melody = {
        {57,0,60,0,64,0,60,0,62,0,64,0,67,0,64,0},
        {57,0,60,0,64,0,60,0,69,0,67,0,64,0,60,0},
        {55,0,59,0,62,0,59,0,60,0,62,0,64,0,62,0},
        {57,0,64,0,62,0,60,0,59,0,57,0,55,0,52,0},
        {57,0,60,0,64,0,67,0,69,0,67,0,64,0,60,0},
        {62,0,65,0,69,0,65,0,67,0,64,0,62,0,60,0},
        {55,0,59,0,62,0,67,0,64,0,62,0,59,0,55,0},
        {52,0,55,0,57,0,60,0,64,0,60,0,57,0,0,0},
    };
    t1.bass = {
        {45,0,0,0,45,0,0,0,45,0,0,0,45,0,0,0},
        {45,0,0,0,45,0,0,0,40,0,0,0,40,0,0,0},
        {43,0,0,0,43,0,0,0,43,0,0,0,43,0,0,0},
        {45,0,0,0,45,0,0,0,45,0,0,0,40,0,0,0},
        {45,0,0,0,45,0,0,0,48,0,0,0,48,0,0,0},
        {50,0,0,0,50,0,0,0,50,0,0,0,50,0,0,0},
        {43,0,0,0,43,0,0,0,43,0,0,0,43,0,0,0},
        {40,0,0,0,40,0,0,0,45,0,0,0,45,0,0,0},
    };
    t1.hat.assign(8 * 16, 0);
    for (int bar = 0; bar < 8; ++bar)
        for (int s = 0; s < 16; ++s) {
            if (s % 2 == 0) t1.hat[static_cast<std::size_t>(bar * 16 + s)] = 1;
            if (s == 14) t1.hat[static_cast<std::size_t>(bar * 16 + s)] = 2;
        }
    out["music_stage1"] = renderMusic(t1);

    // Stage 2: driving battle theme (D minor, faster).
    MusicTrack t2;
    t2.bpm = 150;
    t2.melody = {
        {62,62,0,62,65,0,62,0,69,0,67,0,65,0,62,0},
        {62,62,0,62,65,0,69,0,72,0,69,0,65,0,62,0},
        {60,60,0,60,64,0,67,0,70,0,67,0,64,0,60,0},
        {67,0,65,0,64,0,62,0,60,0,58,0,55,0,53,0},
        {62,62,0,62,65,0,62,0,74,0,72,0,69,0,65,0},
        {72,0,71,0,69,0,67,0,65,0,62,0,60,0,58,0},
        {67,0,70,0,74,0,70,0,67,0,65,0,62,0,59,0},
        {62,0,0,0,58,0,0,0,55,0,0,0,50,0,0,0},
    };
    t2.bass = {
        {50,0,50,0,38,0,50,0,50,0,50,0,38,0,50,0},
        {50,0,50,0,38,0,50,0,53,0,53,0,41,0,53,0},
        {48,0,48,0,36,0,48,0,48,0,48,0,36,0,48,0},
        {45,0,45,0,33,0,45,0,45,0,45,0,33,0,45,0},
        {50,0,50,0,38,0,50,0,50,0,50,0,38,0,50,0},
        {41,0,41,0,41,0,41,0,46,0,46,0,46,0,46,0},
        {43,0,43,0,43,0,43,0,43,0,43,0,43,0,43,0},
        {38,0,38,0,38,0,38,0,45,0,45,0,45,0,45,0},
    };
    t2.hat.assign(8 * 16, 0);
    for (int bar = 0; bar < 8; ++bar)
        for (int s = 0; s < 16; ++s) {
            if (s % 2 == 1) t2.hat[static_cast<std::size_t>(bar * 16 + s)] = 1;
            if (s == 7 || s == 15) t2.hat[static_cast<std::size_t>(bar * 16 + s)] = 2;
        }
    out["music_stage2"] = renderMusic(t2);

    // Title screen theme: reuse a slowed stage-1 feel.
    out["music_title"] = out["music_stage1"];
}

}  // namespace ww
