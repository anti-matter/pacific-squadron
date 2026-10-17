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
    // Volume: extra multiplier on top of the SFX category. pan: -1..1.
    // pitch: playback-rate multiplier (also shifts pitch/timbre).
    void play(const std::string& name, float volume = 1.0f, bool allowConcurrent = true,
              float pan = 0.0f, float pitch = 1.0f);
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

    // Master safety limiter, hooked into the engine's post-mix onProcess
    // callback (runs on the audio thread; allocation-free). The frame-count
    // parameter type matches miniaudio's ma_uint64 exactly.
    static void limiterProcess(void* userData, float* frames, unsigned long long frameCount);
};

}  // namespace ww
