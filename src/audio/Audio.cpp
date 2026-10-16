#include "audio/Audio.h"

#include <algorithm>
#include <cstdlib>

#include "core/MathUtil.h"
#include "miniaudio.h"

namespace ww {
namespace {

// Creates a raw in-memory f32 data source over persistent sample data.
ma_data_source* openSampleSource(const std::vector<float>& samples,
                                 std::uint32_t channels,
                                 std::uint32_t sampleRate) {
    auto* buf = new ma_audio_buffer_ref();
    const ma_uint64 frames =
        static_cast<ma_uint64>(samples.size() / (channels ? channels : 1));
    if (ma_audio_buffer_ref_init(ma_format_f32, channels, samples.data(), frames,
                                 buf) != MA_SUCCESS) {
        delete buf;
        return nullptr;
    }
    (void)sampleRate;  // engine resamples to device rate via its mixer
    return reinterpret_cast<ma_data_source*>(buf);
}

void closeSampleSource(ma_data_source* ds) {
    if (!ds) return;
    auto* buf = reinterpret_cast<ma_audio_buffer_ref*>(ds);
    ma_audio_buffer_ref_uninit(buf);
    delete buf;
}

}  // namespace

struct Audio::Impl {
    ma_engine engine{};
    ma_sound_group musicGroup{};
    ma_sound_group sfxGroup{};
    bool engineUp = false;

    std::unordered_map<std::string, SoundBuffer> buffers;
    struct Voice {
        ma_sound sound;
        ma_data_source* ds;
    };
    std::vector<Voice*> voices;
    Voice* music = nullptr;
    std::string musicName;
};

bool Audio::init() {
    impl_ = std::make_unique<Impl>();
    ma_engine_config ecfg = ma_engine_config_init();
    ecfg.sampleRate = 44100;  // matches our synthesized buffers
    if (ma_engine_init(&ecfg, &impl_->engine) != MA_SUCCESS) {
        impl_.reset();
        return false;
    }
    if (ma_sound_group_init(&impl_->engine, 0, nullptr, &impl_->musicGroup) != MA_SUCCESS ||
        ma_sound_group_init(&impl_->engine, 0, nullptr, &impl_->sfxGroup) != MA_SUCCESS) {
        ma_engine_uninit(&impl_->engine);
        impl_.reset();
        return false;
    }
    impl_->engineUp = true;
    available_ = true;
    return true;
}

Audio::Audio() = default;

Audio::~Audio() {
    shutdown();
}

void Audio::shutdown() {
    if (!impl_) return;
    if (impl_->engineUp) {
        if (impl_->music) {
            ma_sound_stop(&impl_->music->sound);
            ma_sound_uninit(&impl_->music->sound);
            closeSampleSource(impl_->music->ds);
            delete impl_->music;
            impl_->music = nullptr;
        }
        for (auto* v : impl_->voices) {
            ma_sound_stop(&v->sound);
            ma_sound_uninit(&v->sound);
            closeSampleSource(v->ds);
            delete v;
        }
        impl_->voices.clear();
        ma_sound_group_uninit(&impl_->sfxGroup);
        ma_sound_group_uninit(&impl_->musicGroup);
        ma_engine_uninit(&impl_->engine);
    }
    impl_.reset();
    available_ = false;
}

void Audio::registerBuffer(const std::string& name, SoundBuffer buf) {
    if (!impl_) return;
    impl_->buffers[name] = std::move(buf);
}

void Audio::play(const std::string& name, float volume, bool allowConcurrent) {
    if (!impl_ || !impl_->engineUp) return;
    auto it = impl_->buffers.find(name);
    if (it == impl_->buffers.end() || it->second.samples.empty()) return;
    if (!allowConcurrent) {
        for (auto* v : impl_->voices) {
            ma_sound_stop(&v->sound);
            ma_sound_uninit(&v->sound);
            closeSampleSource(v->ds);
            delete v;
        }
        impl_->voices.clear();
    }
    // Cap concurrent voices.
    if (impl_->voices.size() > 24) {
        auto* oldest = impl_->voices.front();
        ma_sound_stop(&oldest->sound);
        ma_sound_uninit(&oldest->sound);
        closeSampleSource(oldest->ds);
        delete oldest;
        impl_->voices.erase(impl_->voices.begin());
    }
    const SoundBuffer& sb = it->second;
    ma_data_source* ds = openSampleSource(sb.samples, sb.channels, sb.sampleRate);
    if (!ds) return;
    auto* voice = new Impl::Voice();
    if (ma_sound_init_from_data_source(&impl_->engine, ds, 0, &impl_->sfxGroup,
                                       &voice->sound) != MA_SUCCESS) {
        closeSampleSource(ds);
        delete voice;
        return;
    }
    voice->ds = ds;
    ma_sound_set_volume(&voice->sound, clamp(volume, 0.0f, 1.0f));
    ma_sound_start(&voice->sound);
    impl_->voices.push_back(voice);
}

void Audio::playMusic(const std::string& name) {
    if (!impl_ || !impl_->engineUp) return;
    if (impl_->music && impl_->musicName == name) return;  // already playing
    stopMusic();
    auto it = impl_->buffers.find(name);
    if (it == impl_->buffers.end() || it->second.samples.empty()) return;
    const SoundBuffer& sb = it->second;
    ma_data_source* ds = openSampleSource(sb.samples, sb.channels, sb.sampleRate);
    if (!ds) return;
    auto* voice = new Impl::Voice();
    if (ma_sound_init_from_data_source(&impl_->engine, ds, MA_SOUND_FLAG_LOOPING,
                                       &impl_->musicGroup, &voice->sound) != MA_SUCCESS) {
        closeSampleSource(ds);
        delete voice;
        return;
    }
    voice->ds = ds;
    ma_sound_start(&voice->sound);
    impl_->music = voice;
    impl_->musicName = name;
}

void Audio::stopMusic() {
    if (!impl_ || !impl_->engineUp) return;
    if (impl_->music) {
        ma_sound_stop(&impl_->music->sound);
        ma_sound_uninit(&impl_->music->sound);
        closeSampleSource(impl_->music->ds);
        delete impl_->music;
        impl_->music = nullptr;
        impl_->musicName.clear();
    }
}

void Audio::setMasterVolume(float v) {
    if (!impl_ || !impl_->engineUp) return;
    ma_engine_set_volume(&impl_->engine, clamp(v, 0.0f, 1.0f));
}
void Audio::setMusicVolume(float v) {
    if (!impl_ || !impl_->engineUp) return;
    ma_sound_group_set_volume(&impl_->musicGroup, clamp(v, 0.0f, 1.0f));
}
void Audio::setSfxVolume(float v) {
    if (!impl_ || !impl_->engineUp) return;
    ma_sound_group_set_volume(&impl_->sfxGroup, clamp(v, 0.0f, 1.0f));
}

void Audio::pump() {
    if (!impl_) return;
    auto& v = impl_->voices;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](Impl::Voice* voice) {
                               if (!ma_sound_is_playing(&voice->sound)) {
                                   ma_sound_uninit(&voice->sound);
                                   closeSampleSource(voice->ds);
                                   delete voice;
                                   return true;
                               }
                               return false;
                           }),
            v.end());
}

}  // namespace ww
