#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::video {

// Minimal FFmpeg-backed H.264 decoder sized for the mirror stream path.
//
// Feed it:
//   1. `set_parameter_sets_from_avcc(avcc_bytes)` once per resolution change
//      (received as SPS_PPS mirror frames). The avcC blob contains the SPS
//      and PPS NAL units; we extract both and keep them in Annex-B form so
//      they can be prepended to every IDR packet we dispatch.
//   2. `decode(annexb_nal_bytes, is_idr)` for every decrypted mirror frame.
//      For IDR packets we prepend the stored SPS+PPS automatically
//      (libavcodec needs them to initialise the reference frames).
//
// Decoded frames are kept internally; use `last_frame_size()` to know the
// resolution and `dump_last_frame_ppm()` to write the most recent YUV →
// RGB-converted frame to disk as proof the pipeline works.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&)            = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Initialise with an H.264 decoder (the historical path for the
    // AirPlay mirror stream). If iOS later sends an hvcC blob in a
    // SPS_PPS frame — because we now advertise bit 42 / HEVC — the
    // decoder transparently switches to H.265 via reinit_for_hevc().
    //
    // hwaccel=true requests D3D11VA hardware decoding on Windows
    // (GPU-side decode + sys-memory transfer + NV12→I420 conversion).
    // Falls back transparently to software when the platform / driver
    // can't honor it.
    bool init(bool hwaccel = false);

    // Parse SPS/PPS out of an avcC (H.264) or hvcC (HEVC) blob iOS
    // sent as an SPS_PPS mirror frame, caching them as Annex-B bytes
    // for later prepend to IDR packets. Switches the internal decoder
    // to the matching codec if needed. Returns false on malformed
    // input (both parsers tried).
    bool set_parameter_sets_from_avcc(const uint8_t* avcc, std::size_t size);

    // Returns true iff the packet was accepted by avcodec. `got_frame` is set
    // when a complete picture comes out; width/height describe that frame.
    bool decode(const uint8_t* nal_data, std::size_t nal_size, bool is_idr,
                bool& got_frame, int& width, int& height);

    bool dump_last_frame_ppm(const std::string& path);

    // Raw YUV420P pointers to the most recently decoded frame. Only valid
    // until the next decode() call. Returns false when nothing is available
    // yet or the last frame was not 4:2:0.
    bool last_frame_yuv(const uint8_t*& y, int& y_stride,
                        const uint8_t*& u, int& u_stride,
                        const uint8_t*& v, int& v_stride,
                        int& width, int& height) const;

    // Raw NV12 plane pointers (Y + interleaved UV) to the most recently
    // decoded frame, when the decoder pulled it from D3D11VA. Returns
    // false on the software path (use last_frame_yuv() there) or when
    // no HW frame is available. Skipping the NV12→I420 sws conversion
    // saves a CPU pass per frame at 4K.
    bool last_frame_nv12(const uint8_t*& y,  int& y_stride,
                         const uint8_t*& uv, int& uv_stride,
                         int& width, int& height) const;

    // Number of frames successfully decoded since init().
    uint64_t frames_decoded() const;

    // True after set_parameter_sets_from_avcc() has reinitialised the
    // backend with the HEVC codec (because iOS sent an hvcC config).
    // Lets the mirror_listener pick the right NAL-header layout when
    // logging decrypted IDR / non-IDR frames.
    bool is_hevc() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::video
