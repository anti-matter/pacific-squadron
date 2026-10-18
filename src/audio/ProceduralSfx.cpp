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

// ---------------------------------------------------------------------------
// Explosion synthesis engine.
//
// Physical explosions are built ONLY from noise layers — never from a clean
// sine/oscillator — following the shape:
//
//   CRACK/THUMP  ->  filtered-noise BLAST  ->  low RUMBLE  ->  DEBRIS decay
//
// addBoom() is the workhorse: white noise through a one-pole lowpass whose
// cutoff slides across the layer (bright crack -> dull body), shaped by a
// fast-attack power-decay envelope. Optional slow amplitude wobble makes the
// rumble organic without implying a musical pitch.
// ---------------------------------------------------------------------------
struct BoomLayer {
    float t0, dur, amp, rise, decayPow, lpStart, lpEnd;
};

void addBoom(std::vector<float>& b, const BoomLayer& L, Rng& rng,
             float tremolo = 0.0f, float tremHz = 0.0f) {
    const int s0 = static_cast<int>(L.t0 * kRate);
    const int n = static_cast<int>(L.dur * kRate);
    while (static_cast<int>(b.size()) < s0 + n) b.push_back(0.0f);
    float y = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float tt = static_cast<float>(i) / static_cast<float>(n);
        const float v = rng.nextFloat() * 2.0f - 1.0f;
        const float kRaw = L.lpStart + (L.lpEnd - L.lpStart) * tt;  // 1 = white
        // The one-pole recursion y += k(v-y) is only stable for 0 < k <= 1;
        // above that |1-k| > 1 and the filter diverges. Clamp as a guard.
        const float k = kRaw > 1.0f ? 1.0f : kRaw;
        y += k * (v - y);
        float env = (L.rise > 0.0f && tt < L.rise) ? tt / L.rise : 1.0f;
        env *= std::pow(1.0f - tt, L.decayPow);
        if (tremHz > 0.0f)
            env *= 1.0f + tremolo * std::sin(kTau * tremHz * (L.t0 + tt * L.dur));
        b[static_cast<std::size_t>(s0 + i)] += y * L.amp * env;
    }
}

// Hard pressure transient: a few milliseconds of near-white noise with an
// instantaneous attack plus a dull thump under it — physical impact, no ring.
void addCrack(std::vector<float>& b, float t0, float amp, Rng& rng) {
    addBoom(b, {t0, 0.012f, amp, 0.0f, 2.5f, 0.95f, 0.55f}, rng);
    addBoom(b, {t0, 0.045f, amp * 0.55f, 0.0f, 3.5f, 0.35f, 0.10f}, rng);
}

// Sparse, irregular debris grains with a diminishing level.
void addDebris(std::vector<float>& b, float t0, float window, int count,
               float amp, Rng& rng) {
    for (int j = 0; j < count; ++j) {
        const float p = static_cast<float>(j) / static_cast<float>(count);
        const float g0 = t0 + window * p * rng.range(0.55f, 1.35f) * 0.9f;
        const float gAmp = amp * (1.0f - p) * rng.range(0.25f, 1.0f);
        addBoom(b, {g0, rng.range(0.004f, 0.014f), gAmp, 0.0f,
                    rng.range(2.0f, 5.0f), rng.range(0.45f, 0.9f), 0.2f}, rng);
    }
}

// Tanh soft saturation. Fuses stacked noise layers into one physical event:
// the loudest moments compress and grit (like overdriven air), quiet tails
// pass through. Maps [-1,1] -> [-1,1] so normalization stays predictable.
void saturate(std::vector<float>& b, float drive) {
    if (drive <= 1.0f) return;
    const float norm = 1.0f / std::tanh(drive);
    for (float& s : b) s = std::tanh(s * drive) * norm;
}

// --- explosion classes ------------------------------------------------------
// Each class layers: CRACK -> body blast (bright->dull) -> rumble -> debris.
// Class identity comes from spectrum, layering and time structure, not just
// amplitude: smalls are short and bass-light, larges carry long sub rumble,
// the boss detonation adds staggered secondary blasts inside one buffer.

void buildSmall(std::vector<float>& b, Rng& rng) {   // ~0.24 s
    addCrack(b, 0.0f, rng.range(0.85f, 1.0f), rng);
    addBoom(b, {0.001f, rng.range(0.11f, 0.15f), 0.65f, 0.0f, 2.2f, 0.30f, 0.08f}, rng);
    addBoom(b, {0.004f, rng.range(0.18f, 0.26f), 0.35f, 0.01f, 1.8f, 0.07f, 0.02f}, rng);
    // Light chest-thump: present but clearly bass-light vs. the medium class.
    addBoom(b, {0.01f, rng.range(0.16f, 0.24f), 0.30f, 0.02f, 1.6f, 0.045f, 0.015f}, rng);
    addDebris(b, 0.05f, 0.16f, rng.rangeInt(2, 4), 0.35f, rng);
}

void buildMedium(std::vector<float>& b, Rng& rng) {  // ~0.5 s
    addCrack(b, 0.0f, 1.0f, rng);
    addBoom(b, {0.001f, rng.range(0.16f, 0.22f), 0.85f, 0.0f, 2.0f, 0.42f, 0.10f}, rng);
    addBoom(b, {0.006f, rng.range(0.30f, 0.42f), 0.45f, 0.015f, 1.6f, 0.10f, 0.03f}, rng);
    addBoom(b, {0.03f, rng.range(0.34f, 0.46f), 0.35f, 0.04f, 1.4f, 0.045f, 0.015f},
            rng, 0.30f, rng.range(6.0f, 11.0f));
    addDebris(b, 0.08f, 0.38f, rng.rangeInt(6, 10), 0.4f, rng);
}

void buildLarge(std::vector<float>& b, Rng& rng) {   // ~1.0 s
    addCrack(b, 0.0f, 1.0f, rng);
    addBoom(b, {0.001f, rng.range(0.24f, 0.32f), 1.0f, 0.0f, 1.9f, 0.50f, 0.10f}, rng);
    addBoom(b, {0.0f, 0.10f, 0.90f, 0.0f, 3.0f, 0.06f, 0.02f}, rng);      // sub thump
    addBoom(b, {0.01f, rng.range(0.55f, 0.75f), 0.55f, 0.02f, 1.5f, 0.09f, 0.03f}, rng);
    addBoom(b, {0.05f, rng.range(0.85f, 1.10f), 0.50f, 0.06f, 1.3f, 0.04f, 0.012f},
            rng, 0.35f, rng.range(5.0f, 9.0f));
    addDebris(b, 0.12f, 0.85f, rng.rangeInt(10, 16), 0.45f, rng);
}

// Boss destruction: ONE catastrophic event (~4.4 s), not a string of hits.
//
//   0.00 s  concussion slap (near-white pressure wave, instant attack)
//   0.00 s  main blast body: three overlapping soft-attack noise bodies,
//           bright collapsing to dull, saturating into one enormous event
//   0.15 s  deep pressure-wave rumble: two very-low, very-long noise layers
//           that RISE into the blast and smear underneath everything
//   0.2-2.4 s  irregular internal secondary detonations (Poisson spacing,
//           three spectral characters, per-event randomized envelope)
//   0.9-3.8 s  crackling debris tail, thinning out
//   3-4.4 s  the lowest rumble decays to silence
//
// Percussion-avoidance rules baked in: no layer has an instant attack
// except the concussion and the sharp crack secondaries (which are quiet
// and rare), the low end is long-smeared noise (never a short resonant
// burst), secondary timings are drawn from wide irregular ranges, and the
// whole buffer is saturated so the layers weld into one body.
void buildBoss(std::vector<float>& b, Rng& rng) {
    // --- main blast -------------------------------------------------------
    addBoom(b, {0.0f, 0.030f, 1.00f, 0.0f, 2.4f, 1.0f, 0.65f}, rng);  // concussion
    addBoom(b, {0.000f, rng.range(0.50f, 0.62f), 1.00f, 0.012f, 1.85f,
                rng.range(0.55f, 0.68f), 0.085f}, rng);               // chest body
    addBoom(b, {0.008f, rng.range(0.85f, 1.05f), 0.62f, 0.030f, 1.50f,
                rng.range(0.30f, 0.42f), 0.055f}, rng);               // mid body
    addBoom(b, {0.020f, rng.range(1.30f, 1.60f), 0.48f, 0.060f, 1.30f,
                rng.range(0.17f, 0.26f), 0.035f}, rng);               // deep body
    // --- pressure-wave rumble (soft rise: rolls in, never "hits") ----------
    addBoom(b, {0.0f, rng.range(3.0f, 3.5f), 0.55f, 0.12f, 1.10f,
                rng.range(0.050f, 0.065f), 0.010f}, rng);
    addBoom(b, {0.20f, rng.range(3.8f, 4.4f), 0.38f, 0.30f, 1.00f,
                rng.range(0.030f, 0.042f), 0.006f}, rng);
    // --- internal secondary detonations (irregular, heterogeneous) ---------
    float t = rng.range(0.22f, 0.50f);
    while (t < 2.35f) {
        const int kind = rng.rangeInt(0, 2);
        if (kind == 0) {  // dull internal WHUMP: muffled, soft attack
            addBoom(b, {t, rng.range(0.18f, 0.36f), rng.range(0.28f, 0.48f),
                        rng.range(0.012f, 0.050f), rng.range(1.6f, 2.4f),
                        rng.range(0.09f, 0.18f), rng.range(0.020f, 0.045f)}, rng);
        } else if (kind == 1) {  // sharp internal CRACK: bright, very short
            addBoom(b, {t, rng.range(0.020f, 0.050f), rng.range(0.16f, 0.34f),
                        0.0f, rng.range(2.6f, 4.0f),
                        rng.range(0.90f, 1.0f), rng.range(0.5f, 0.9f)}, rng);
        } else {  // deep MUFFLED BLAST behind the hull plates
            addBoom(b, {t, rng.range(0.45f, 0.85f), rng.range(0.22f, 0.38f),
                        rng.range(0.008f, 0.030f), rng.range(1.8f, 2.6f),
                        rng.range(0.18f, 0.32f), rng.range(0.040f, 0.090f)}, rng);
        }
        if (rng.chance(0.45f))  // some secondations throw sparks of debris
            addDebris(b, t + 0.02f, rng.range(0.20f, 0.50f), rng.rangeInt(3, 7),
                      0.30f, rng);
        t += rng.range(0.15f, 0.65f);  // strongly irregular spacing
    }
    // --- crackling debris tail ---------------------------------------------
    addDebris(b, 0.90f, 2.90f, rng.rangeInt(20, 32), 0.38f, rng);
    // --- weld everything into one physical event ----------------------------
    saturate(b, 2.1f);
}

// Continuous breakup bed (~2.9 s) for the boss's death roll: low rumble +
// irregular crackle with NO percussive attacks (every layer rises softly and
// decays long). Replaces the old fixed-interval chain of full boom hits.
void buildBreakup(std::vector<float>& b, Rng& rng) {
    addBoom(b, {0.0f, 2.9f, 0.55f, 0.20f, 1.05f,
                rng.range(0.050f, 0.065f), 0.012f}, rng);
    addBoom(b, {0.3f, 2.6f, 0.40f, 0.35f, 1.00f,
                rng.range(0.030f, 0.045f), 0.008f}, rng);
    addBoom(b, {0.1f, rng.range(1.2f, 1.8f), 0.30f, 0.15f, 1.4f,
                rng.range(0.16f, 0.26f), 0.05f}, rng);
    // Poisson-ish crackle bursts: soft grain clusters at uneven times.
    float t = rng.range(0.05f, 0.30f);
    while (t < 2.6f) {
        addDebris(b, t, rng.range(0.15f, 0.40f), rng.rangeInt(2, 6),
                  rng.range(0.15f, 0.35f), rng);
        t += rng.range(0.25f, 0.80f);
    }
    saturate(b, 1.4f);
}

// Synthesize `n` variants per class from independent seeds so repeated
// playback can switch timbres, not just pitch.
SoundBuffer makeBoomVariant(void (*build)(std::vector<float>&, Rng&),
                            std::uint64_t seed, float peak) {
    Rng rng(seed);
    std::vector<float> b;
    build(b, rng);
    normalize(b, peak);
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

    auto midiHz = [](int m) {
        return 440.0f * std::pow(2.0f, (static_cast<float>(m) - 69.0f) / 12.0f);
    };

    for (int bar = 0; bar < static_cast<int>(tr.melody.size()); ++bar) {
        for (int s = 0; s < 16; ++s) {
            const float t0 = static_cast<float>(bar * 16 + s) * stepDur;
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
    while (static_cast<int>(mix.size()) < static_cast<int>(static_cast<float>(totalSteps + 8) * stepDur * kRate))
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

    // ---- explosion families ------------------------------------------------
    // Four physical classes (noise layers, no pitched oscillators), several
    // hand-seeded variants each. The audio layer picks a variant at random
    // per play and adds pitch/pan jitter, so repeat detonations never sound
    // identical. Peak levels set a loudness hierarchy with headroom for the
    // master limiter (see Audio::init).
    for (int v = 1; v <= 3; ++v) {
        const auto vv = static_cast<std::uint64_t>(v);
        out["explosion_small_" + std::to_string(v)] =
            makeBoomVariant(buildSmall, 0x5EA11u + vv * 7717u, 0.60f);
        out["explosion_medium_" + std::to_string(v)] =
            makeBoomVariant(buildMedium, 0xB0000u + vv * 9931u, 0.75f);
        out["explosion_large_" + std::to_string(v)] =
            makeBoomVariant(buildLarge, 0xB1600u + vv * 12203u, 0.88f);
    }
    for (int v = 1; v <= 2; ++v) {
        const auto vv = static_cast<std::uint64_t>(v);
        out["explosion_boss_" + std::to_string(v)] =
            makeBoomVariant(buildBoss, 0xB0550u + vv * 15731u, 0.95f);
        out["boss_breakup_" + std::to_string(v)] =
            makeBoomVariant(buildBreakup, 0xB9E4u + vv * 4421u, 0.50f);
    }
    // Legacy names keep working (older code paths / data references).
    out["explosion"] = out["explosion_medium_1"];
    out["explosion_large"] = out["explosion_large_1"];

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

    { std::vector<float> b;   // LOOP BOMB: huge pressure-wave blast.
      addCrack(b, 0.0f, 1.0f, rng);
      addBoom(b, {0.0f, 0.12f, 1.0f, 0.0f, 2.6f, 0.05f, 0.015f}, rng);
      addBoom(b, {0.001f, 0.30f, 0.90f, 0.0f, 1.8f, 0.50f, 0.10f}, rng);
      addBoom(b, {0.02f, 0.70f, 0.50f, 0.03f, 1.4f, 0.08f, 0.025f}, rng);
      addBoom(b, {0.06f, 0.90f, 0.45f, 0.06f, 1.2f, 0.04f, 0.012f}, rng, 0.35f, 6.0f);
      addDebris(b, 0.10f, 0.80f, 12, 0.45f, rng);
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
