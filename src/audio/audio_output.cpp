#include "audio/audio_output.h"
#include "log.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ap::audio {

SdlAudioOutput::SdlAudioOutput()  = default;
SdlAudioOutput::~SdlAudioOutput() { stop(); }

bool SdlAudioOutput::start(int sample_rate, int channels) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_ERROR << "SDL_InitSubSystem(AUDIO) failed: " << SDL_GetError();
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = sample_rate;
    want.format   = AUDIO_S16SYS;     // interleaved int16, host byte order
    want.channels = static_cast<Uint8>(channels);
    want.samples  = 1024;             // buffer granularity (~23 ms @ 44.1 kHz)
    want.callback = nullptr;          // push mode via SDL_QueueAudio

    SDL_AudioSpec got{};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE
                                | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (device_ == 0) {
        LOG_ERROR << "SDL_OpenAudioDevice failed: " << SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    sample_rate_ = got.freq;
    channels_    = got.channels;
    SDL_PauseAudioDevice(device_, 0);
    LOG_INFO << "SdlAudioOutput playing at " << sample_rate_ << " Hz x "
             << channels_ << "ch (format int16)";
    return true;
}

void SdlAudioOutput::stop() {
    if (device_) {
        SDL_PauseAudioDevice(device_, 1);
        SDL_ClearQueuedAudio(device_);
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void SdlAudioOutput::push(const int16_t* samples, int count) {
    if (!device_ || !samples || count <= 0) return;

    const float gain = gain_.load(std::memory_order_relaxed);

    // Fast path: unity gain → send as-is.
    if (gain >= 0.9999f && gain <= 1.0001f) {
        const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
        if (SDL_QueueAudio(device_, samples, bytes) != 0) {
            LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
        }
        return;
    }

    // Apply gain into a scratch buffer, saturate to int16 range.
    static thread_local std::vector<int16_t> scratch;
    scratch.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        float s = static_cast<float>(samples[i]) * gain;
        if (s >  32767.f) s =  32767.f;
        if (s < -32768.f) s = -32768.f;
        scratch[i] = static_cast<int16_t>(s);
    }
    const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
    if (SDL_QueueAudio(device_, scratch.data(), bytes) != 0) {
        LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
    }
}

void SdlAudioOutput::set_volume_db(float db) {
    // AirPlay: 0 dB = full, -144 dB = mute. Anything below -100 treat as 0.
    float lin = (db <= -100.f) ? 0.f
              : (db >=    0.f) ? 1.f
              : std::pow(10.f, db / 20.f);
    gain_.store(lin, std::memory_order_relaxed);
    LOG_INFO << "audio volume: " << db << " dB (gain=" << lin << ')';
}

uint32_t SdlAudioOutput::queued_bytes() const {
    return device_ ? SDL_GetQueuedAudioSize(device_) : 0u;
}

} // namespace ap::audio
