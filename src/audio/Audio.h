#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/ProceduralSfx.h"

namespace ww {

// Audio facade. Game code registers named buffers and plays them; the miniaudio
// backend is entirely hidden behind this interface.
class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    bool init();
    void shutdown();

    void registerBuffer(const std::string& name, SoundBuffer buf);
    // Volume: extra multiplier on top of the SFX category.
    void play(const std::string& name, float volume = 1.0f, bool allowConcurrent = true);
    void playMusic(const std::string& name);
    void stopMusic();

    void setMasterVolume(float v);
    void setMusicVolume(float v);
    void setSfxVolume(float v);

    // Release finished voices. Call once per frame.
    void pump();

    bool available() const { return available_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};

}  // namespace ww
