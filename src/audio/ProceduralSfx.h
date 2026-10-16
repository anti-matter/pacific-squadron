#pragma once

#include <cstdint>
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
void generateAllSfx(std::unordered_map<std::string, SoundBuffer>& out);

}  // namespace ww
