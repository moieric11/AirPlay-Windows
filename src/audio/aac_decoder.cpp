#include "audio/aac_decoder.h"
#include "log.h"

#include <cstring>
#include <deque>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/channel_layout.h>
}

namespace ap::audio {
namespace {

// MPEG-4 sampling frequency index per ISO 14496-3 Table 1.16.
int freq_index(int sample_rate) {
    switch (sample_rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case  8000: return 11;
        case  7350: return 12;
        default:    return 15; // "explicit in ASC" — we don't handle this path
    }
}

// Minimal bit-writer backed by a growing byte vector.
struct BitWriter {
    std::vector<uint8_t> buf;
    int                  bit_pos = 0;

    void put(int value, int nbits) {
        while (nbits > 0) {
            if ((bit_pos / 8) >= static_cast<int>(buf.size())) buf.push_back(0);
            int byte_off   = bit_pos / 8;
            int local_bit  = 7 - (bit_pos % 8);
            int bit        = (value >> (nbits - 1)) & 1;
            if (bit) buf[byte_off] |= static_cast<uint8_t>(1 << local_bit);
            ++bit_pos;
            --nbits;
        }
    }
};

} // namespace

std::vector<uint8_t> build_asc_aac_eld(int sample_rate, int channels, int spf) {
    BitWriter bw;

    // audioObjectType field: AAC-ELD = 39, which is > 31 so we need the
    // 5-bit escape prefix (11111) + 6-bit audioObjectTypeExt (39 - 32 = 7).
    bw.put(31, 5);
    bw.put(7,  6);

    bw.put(freq_index(sample_rate), 4);
    bw.put(channels,                4);

    // ELDSpecificConfig (ISO 14496-3 §4.6.21.2).
    const int frame_length_flag = (spf == 480) ? 1 : 0;  // 0 = 512
    bw.put(frame_length_flag,  1);
    bw.put(0,                  1);  // aacSectionDataResilienceFlag
    bw.put(0,                  1);  // aacScalefactorDataResilienceFlag
    bw.put(0,                  1);  // aacSpectralDataResilienceFlag
    bw.put(0,                  1);  // ldSbrPresentFlag (no SBR extension)
    bw.put(0,                  4);  // eldExtType = ELDEXT_TERM

    return std::move(bw.buf);
}

// ---------------------------------------------------------------------------

struct AacDecoder::Impl {
    const AVCodec*   codec = nullptr;
    AVCodecContext*  ctx   = nullptr;
    AVPacket*        pkt   = nullptr;
    AVFrame*         frame = nullptr;

    int              sample_rate = 0;
    int              channels    = 0;
    uint64_t         frames_out  = 0;

    // Interleaved int16 samples waiting for the sink.
    std::deque<int16_t> pcm;

    ~Impl() {
        if (frame) av_frame_free(&frame);
        if (pkt)   av_packet_free(&pkt);
        if (ctx)   avcodec_free_context(&ctx);
    }
};

AacDecoder::AacDecoder()  : impl_(std::make_unique<Impl>()) {}
AacDecoder::~AacDecoder() = default;

bool AacDecoder::init(const Config& cfg) {
    // RAOP ct values:
    //   2       = ALAC
    //   3       = AAC-LC
    //   4 / 8   = AAC-ELD (8 is "AAC-ELD 44.1k" per observed streams)
    const bool is_alac = (cfg.ct == 2);

    const AVCodecID codec_id = is_alac ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
    impl_->codec = avcodec_find_decoder(codec_id);
    if (!impl_->codec) {
        LOG_ERROR << "AacDecoder: libavcodec has no "
                  << (is_alac ? "ALAC" : "AAC") << " decoder";
        return false;
    }
    impl_->ctx   = avcodec_alloc_context3(impl_->codec);
    impl_->pkt   = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->ctx || !impl_->pkt || !impl_->frame) return false;

    impl_->ctx->sample_rate = cfg.sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
    av_channel_layout_default(&impl_->ctx->ch_layout, cfg.channels);
#else
    impl_->ctx->channels       = cfg.channels;
    impl_->ctx->channel_layout = (cfg.channels == 2) ? AV_CH_LAYOUT_STEREO
                                                     : AV_CH_LAYOUT_MONO;
#endif

    std::vector<uint8_t> extradata;
    if (is_alac) {
        // Minimal ALAC "magic cookie" (36 bytes, Apple spec):
        //   frameLength, compatibleVersion, bitDepth, rice params,
        //   channels, maxRun, maxFrameBytes, avgBitRate, sampleRate.
        // We fill sane defaults for AirPlay 1 audio and let libavcodec
        // adapt from the first frames.
        extradata = {
            0x00, 0x00, 0x00, 0x24,  //  size
            'a','l','a','c',         //  atom tag
            0x00, 0x00, 0x00, 0x00,  //  version + flags
            0x00, 0x00, 0x01, 0x60,  //  frameLength = 352
            0x00,                    //  compatibleVersion
            0x10,                    //  bitDepth = 16
            0x28,                    //  pb = 40
            0x0a,                    //  mb = 10
            0x0e,                    //  kb = 14
            static_cast<uint8_t>(cfg.channels), // numChannels
            0x00, 0xff,              //  maxRun
            0x00, 0x00, 0x00, 0x00,  //  maxFrameBytes
            0x00, 0x00, 0x00, 0x00,  //  avgBitRate
            0x00, 0x00, static_cast<uint8_t>((cfg.sample_rate >> 8) & 0xff),
                       static_cast<uint8_t>(cfg.sample_rate & 0xff),
        };
    } else {
        extradata = build_asc_aac_eld(cfg.sample_rate, cfg.channels, cfg.spf);
    }

    impl_->ctx->extradata_size = static_cast<int>(extradata.size());
    impl_->ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(impl_->ctx->extradata, extradata.data(), extradata.size());

    if (avcodec_open2(impl_->ctx, impl_->codec, nullptr) != 0) {
        LOG_ERROR << "AacDecoder: avcodec_open2("
                  << (is_alac ? "ALAC" : "AAC") << ") failed";
        return false;
    }

    impl_->sample_rate = cfg.sample_rate;
    impl_->channels    = cfg.channels;

    std::string extra_hex;
    for (uint8_t b : extradata) {
        char tmp[4]; std::snprintf(tmp, sizeof(tmp), "%02x ", b);
        extra_hex += tmp;
    }
    LOG_INFO << "AacDecoder ready: "
             << (is_alac ? "ALAC" : "AAC-ELD") << " " << cfg.channels
             << "ch @" << cfg.sample_rate << "Hz"
             << (is_alac ? "" : " spf=") << (is_alac ? "" : std::to_string(cfg.spf))
             << " extradata=" << extra_hex;
    return true;
}

int AacDecoder::decode(const uint8_t* frame, int size) {
    if (!impl_->ctx || !frame || size <= 0) return 0;

    av_packet_unref(impl_->pkt);
    impl_->pkt->data = const_cast<uint8_t*>(frame);
    impl_->pkt->size = size;

    int ret = avcodec_send_packet(impl_->ctx, impl_->pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(ret, err, sizeof(err));
        LOG_WARN << "AacDecoder: send_packet error " << err;
        return -1;
    }

    int produced = 0;
    while (true) {
        ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(ret, err, sizeof(err));
            LOG_WARN << "AacDecoder: receive_frame error " << err;
            return -1;
        }

        const int nb = impl_->frame->nb_samples;
        const int ch = impl_->channels;

        // Convert to interleaved int16, handling the several sample formats
        // libavcodec can emit depending on the codec:
        //   AAC decoders   → AV_SAMPLE_FMT_FLTP (planar float)
        //   ALAC decoder   → AV_SAMPLE_FMT_S16P / S32P (planar int)
        //   Obscure paths  → non-planar variants, rarer but handled.
        // Mixing these up silently produces noise because
        // reinterpret_cast<float*>(int16_data) reads integers as floats.
        const AVSampleFormat fmt =
            static_cast<AVSampleFormat>(impl_->frame->format);
        const bool planar = av_sample_fmt_is_planar(fmt);

        auto sample_to_s16 = [&](int channel, int idx) -> int16_t {
            const uint8_t* plane = planar
                ? impl_->frame->extended_data[channel]
                : impl_->frame->extended_data[0];
            const int pos = planar ? idx : (idx * ch + channel);
            switch (fmt) {
                case AV_SAMPLE_FMT_FLTP:
                case AV_SAMPLE_FMT_FLT: {
                    float s = reinterpret_cast<const float*>(plane)[pos];
                    if (s >  1.f) s =  1.f;
                    if (s < -1.f) s = -1.f;
                    return static_cast<int16_t>(s * 32767.f);
                }
                case AV_SAMPLE_FMT_DBLP:
                case AV_SAMPLE_FMT_DBL: {
                    double s = reinterpret_cast<const double*>(plane)[pos];
                    if (s >  1.0) s =  1.0;
                    if (s < -1.0) s = -1.0;
                    return static_cast<int16_t>(s * 32767.0);
                }
                case AV_SAMPLE_FMT_S16P:
                case AV_SAMPLE_FMT_S16:
                    return reinterpret_cast<const int16_t*>(plane)[pos];
                case AV_SAMPLE_FMT_S32P:
                case AV_SAMPLE_FMT_S32:
                    return static_cast<int16_t>(
                        reinterpret_cast<const int32_t*>(plane)[pos] >> 16);
                default:
                    return 0;
            }
        };

        if (impl_->frames_out == 0) {
            LOG_INFO << "AacDecoder first frame: fmt="
                     << av_get_sample_fmt_name(fmt)
                     << " ch=" << ch << " nb=" << nb;
        }

        for (int i = 0; i < nb; ++i) {
            for (int c = 0; c < ch; ++c) {
                impl_->pcm.push_back(sample_to_s16(c, i));
            }
        }

        ++impl_->frames_out;
        produced += nb * ch;
    }
    return produced;
}

int AacDecoder::pull_pcm_s16(int16_t* dst, int max_samples) {
    int n = static_cast<int>(std::min<std::size_t>(impl_->pcm.size(),
                                                  static_cast<std::size_t>(max_samples)));
    for (int i = 0; i < n; ++i) dst[i] = impl_->pcm.front(), impl_->pcm.pop_front();
    return n;
}

int      AacDecoder::sample_rate()    const { return impl_ ? impl_->sample_rate : 0; }
int      AacDecoder::channels()       const { return impl_ ? impl_->channels    : 0; }
uint64_t AacDecoder::frames_decoded() const { return impl_ ? impl_->frames_out  : 0; }

} // namespace ap::audio
