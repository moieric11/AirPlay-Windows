#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ap::audio {

// libavcodec-backed decoder for the raw AAC-ELD (and AAC-LC) frames iOS
// ships through the RAOP audio stream. FFmpeg's AAC decoder handles
// AAC-ELD via AudioSpecificConfig (ASC) supplied as `extradata`.
//
// Input:  a variable-size raw AAC-ELD frame (no ADTS, no LATM wrapping)
//         — exactly what comes out of AES-CBC decryption.
// Output: float-planar PCM (AV_SAMPLE_FMT_FLTP typically). Each decoded
//         frame's samples are appended to an internal interleaved int16
//         buffer that the audio sink consumer can drain.
class AacDecoder {
public:
    struct Config {
        int ct          = 8;      // 2=ALAC, 3=AAC-LC, 4/8=AAC-ELD (see AudioReceiver)
        int sample_rate = 44100;
        int channels    = 2;
        int spf         = 480;    // samples per frame (480 or 512 for AAC-ELD)
    };

    AacDecoder();
    ~AacDecoder();

    AacDecoder(const AacDecoder&)            = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    bool init(const Config& cfg);

    // Feed one raw AAC frame. Returns the number of PCM samples now available.
    // On decoder error returns -1 but keeps the context alive (iOS audio
    // occasionally carries one corrupted frame in a hundred; dropping it is
    // better than re-initialising the decoder).
    int  decode(const uint8_t* frame, int size);

    // Pulls up to `max_samples` interleaved int16 stereo samples out of the
    // internal buffer. Returns how many were copied.
    int  pull_pcm_s16(int16_t* dst, int max_samples);

    int  sample_rate()       const;
    int  channels()          const;
    uint64_t frames_decoded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Build the 4-byte MPEG-4 AudioSpecificConfig for AAC-ELD given sample
// rate, channel count and samples-per-frame. Returns a vector (usually
// 4 bytes) that can be handed to libavcodec as `extradata`.
std::vector<uint8_t> build_asc_aac_eld(int sample_rate, int channels, int spf);

} // namespace ap::audio
