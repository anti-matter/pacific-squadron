#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ww {

struct SoundBuffer {
    std::vector<float> samples;  // interleaved
    std::uint32_t channels = 1;
    std::uint32_t sampleRate = 44100;
};

// Synthesizes all placeholder sound effects and music loops. All audio is
// generated at startup so missing audio files never block development.
// Synthesis is deterministic, so with a non-empty `cachePath` the buffers
// are loaded from / dumped to a binary blob, skipping the synth on later
// launches. `progress` receives 0..1 while generating.
void generateAllSfx(std::unordered_map<std::string, SoundBuffer>& out,
                    const std::string& cachePath = "",
                    const std::function<void(float)>& progress = {});

}  // namespace ww
