#include "video/h264_decoder.h"
#include "log.h"

#include <cstdio>
#include <cstring>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

constexpr uint8_t kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

void append_annexb_nal(std::vector<uint8_t>& dst,
                       const uint8_t* nal, std::size_t size) {
    dst.insert(dst.end(), kStartCode, kStartCode + 4);
    dst.insert(dst.end(), nal, nal + size);
}

} // namespace

struct H264Decoder::Impl {
    const AVCodec*       codec       = nullptr;
    AVCodecContext*      ctx         = nullptr;
    AVPacket*            pkt         = nullptr;
    AVFrame*             frame       = nullptr;
    SwsContext*          sws         = nullptr;

    std::vector<uint8_t> sps_annexb;
    std::vector<uint8_t> pps_annexb;

    int                  last_w      = 0;
    int                  last_h      = 0;
    std::vector<uint8_t> last_rgb;       // WxHx3 filled on every successful decode
    bool                 last_is_yuv420 = false;
    uint64_t             frames_out  = 0;

    ~Impl() {
        if (sws)   sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (pkt)   av_packet_free(&pkt);
        if (ctx)   avcodec_free_context(&ctx);
    }
};

H264Decoder::H264Decoder()  : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::init() {
    impl_->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!impl_->codec) {
        LOG_ERROR << "H264Decoder: H.264 decoder not available in libavcodec";
        return false;
    }
    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    impl_->pkt = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->ctx || !impl_->pkt || !impl_->frame) return false;

    if (avcodec_open2(impl_->ctx, impl_->codec, nullptr) != 0) {
        LOG_ERROR << "H264Decoder: avcodec_open2 failed";
        return false;
    }
    LOG_INFO << "H264Decoder ready (libavcodec " << LIBAVCODEC_IDENT << ')';
    return true;
}

// Parse the avcC blob iOS sends in SPS_PPS frames (ISO 14496-15 §5.2.4.1.1)
// and extract the SPS + PPS NAL units. See docs/PROTOCOL.md for layout.
bool H264Decoder::set_parameter_sets_from_avcc(const uint8_t* avcc,
                                               std::size_t size) {
    if (!avcc || size < 8 || avcc[0] != 0x01) {
        LOG_WARN << "H264Decoder: avcC missing or wrong version";
        return false;
    }
    uint8_t num_sps = avcc[5] & 0x1f;
    if (num_sps < 1) return false;

    std::size_t p = 6;

    impl_->sps_annexb.clear();
    impl_->pps_annexb.clear();

    // -- SPS list --
    for (uint8_t i = 0; i < num_sps && p + 2 <= size; ++i) {
        uint16_t len = (static_cast<uint16_t>(avcc[p]) << 8) | avcc[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(impl_->sps_annexb, avcc + p, len);
        p += len;
    }
    if (p >= size) return false;

    // -- PPS list --
    uint8_t num_pps = avcc[p++];
    for (uint8_t i = 0; i < num_pps && p + 2 <= size; ++i) {
        uint16_t len = (static_cast<uint16_t>(avcc[p]) << 8) | avcc[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(impl_->pps_annexb, avcc + p, len);
        p += len;
    }

    LOG_INFO << "H264Decoder: SPS=" << (impl_->sps_annexb.size() - 4) << "B, "
             << "PPS=" << (impl_->pps_annexb.size() - 4) << "B cached";
    return !impl_->sps_annexb.empty() && !impl_->pps_annexb.empty();
}

bool H264Decoder::decode(const uint8_t* nal_data, std::size_t nal_size,
                         bool is_idr,
                         bool& got_frame, int& width, int& height) {
    got_frame = false;
    width = height = 0;
    if (!impl_->ctx) return false;
    if (!nal_data || nal_size == 0) return false;

    // Build the Annex-B packet. Every IDR gets SPS + PPS prepended so the
    // decoder can re-initialise references if needed.
    std::vector<uint8_t> buf;
    buf.reserve(nal_size + impl_->sps_annexb.size() + impl_->pps_annexb.size());
    if (is_idr) {
        buf.insert(buf.end(),
                   impl_->sps_annexb.begin(), impl_->sps_annexb.end());
        buf.insert(buf.end(),
                   impl_->pps_annexb.begin(), impl_->pps_annexb.end());
    }
    buf.insert(buf.end(), nal_data, nal_data + nal_size);

    av_packet_unref(impl_->pkt);
    impl_->pkt->data = buf.data();
    impl_->pkt->size = static_cast<int>(buf.size());

    int ret = avcodec_send_packet(impl_->ctx, impl_->pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(ret, err, sizeof(err));
        LOG_WARN << "H264Decoder: send_packet error " << err;
        return false;
    }

    ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(ret, err, sizeof(err));
        LOG_WARN << "H264Decoder: receive_frame error " << err;
        return false;
    }

    width   = impl_->frame->width;
    height  = impl_->frame->height;
    impl_->last_w = width;
    impl_->last_h = height;
    impl_->last_is_yuv420 = (impl_->frame->format == AV_PIX_FMT_YUV420P ||
                             impl_->frame->format == AV_PIX_FMT_YUVJ420P);

    // iOS delivers full-range YUV as "YUVJ420P". That pixel format is
    // deprecated in modern libswscale (prints a warning per frame) — remap
    // to the regular YUV420P and tell sws the source range is full via
    // sws_setColorspaceDetails so the colour conversion stays correct.
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(impl_->frame->format);
    bool src_full_range = false;
    switch (src_fmt) {
        case AV_PIX_FMT_YUVJ420P: src_fmt = AV_PIX_FMT_YUV420P; src_full_range = true; break;
        case AV_PIX_FMT_YUVJ422P: src_fmt = AV_PIX_FMT_YUV422P; src_full_range = true; break;
        case AV_PIX_FMT_YUVJ444P: src_fmt = AV_PIX_FMT_YUV444P; src_full_range = true; break;
        default: break;
    }

    impl_->sws = sws_getCachedContext(
        impl_->sws, width, height, src_fmt,
        width, height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl_->sws) {
        // Propagate the "source is full range" flag.
        const int* inv_tbl = nullptr;
        const int* tbl     = nullptr;
        int src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
        sws_getColorspaceDetails(impl_->sws,
            const_cast<int**>(&inv_tbl), &src_range,
            const_cast<int**>(&tbl),     &dst_range,
            &brightness, &contrast, &saturation);
        src_range = src_full_range ? 1 : 0;
        sws_setColorspaceDetails(impl_->sws,
            inv_tbl, src_range, tbl, dst_range,
            brightness, contrast, saturation);

        impl_->last_rgb.assign(static_cast<std::size_t>(width) * height * 3, 0);
        uint8_t* dst[1]        = { impl_->last_rgb.data() };
        int      dst_stride[1] = { width * 3 };
        sws_scale(impl_->sws, impl_->frame->data, impl_->frame->linesize,
                  0, height, dst, dst_stride);
    }

    ++impl_->frames_out;
    got_frame = true;
    return true;
}

bool H264Decoder::dump_last_frame_ppm(const std::string& path) {
    if (impl_->last_rgb.empty() || impl_->last_w == 0) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", impl_->last_w, impl_->last_h);
    std::fwrite(impl_->last_rgb.data(), 1, impl_->last_rgb.size(), f);
    std::fclose(f);
    LOG_INFO << "H264Decoder: dumped " << impl_->last_w << 'x' << impl_->last_h
             << " frame to " << path;
    return true;
}

bool H264Decoder::last_frame_yuv(const uint8_t*& y, int& y_stride,
                                 const uint8_t*& u, int& u_stride,
                                 const uint8_t*& v, int& v_stride,
                                 int& width, int& height) const {
    if (!impl_ || !impl_->last_is_yuv420 || !impl_->frame ||
        impl_->last_w == 0) return false;
    y = impl_->frame->data[0];      y_stride = impl_->frame->linesize[0];
    u = impl_->frame->data[1];      u_stride = impl_->frame->linesize[1];
    v = impl_->frame->data[2];      v_stride = impl_->frame->linesize[2];
    width  = impl_->last_w;
    height = impl_->last_h;
    return y && u && v;
}

uint64_t H264Decoder::frames_decoded() const { return impl_ ? impl_->frames_out : 0; }

} // namespace ap::video
