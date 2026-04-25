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

    // Annex-B bytes (with leading 00 00 00 01 start code) cached from
    // the last config blob so every IDR can be prepended with its
    // parameter sets. HEVC has three (VPS+SPS+PPS), H.264 has two.
    std::vector<uint8_t> vps_annexb;   // HEVC only; empty for H.264
    std::vector<uint8_t> sps_annexb;
    std::vector<uint8_t> pps_annexb;

    AVCodecID            codec_id    = AV_CODEC_ID_H264;

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

    // Open (or re-open after a codec switch) libavcodec for `id`.
    bool open(AVCodecID id) {
        const AVCodec* c = avcodec_find_decoder(id);
        if (!c) {
            LOG_ERROR << "decoder: codec " << avcodec_get_name(id)
                      << " not available in libavcodec";
            return false;
        }
        if (ctx) avcodec_free_context(&ctx);
        codec    = c;
        codec_id = id;
        ctx      = avcodec_alloc_context3(c);
        if (!ctx) return false;
        if (avcodec_open2(ctx, c, nullptr) != 0) {
            LOG_ERROR << "decoder: avcodec_open2 failed for "
                      << avcodec_get_name(id);
            return false;
        }
        LOG_INFO << "decoder ready: " << avcodec_get_name(id)
                 << " (libavcodec " << LIBAVCODEC_IDENT << ')';
        return true;
    }
};

H264Decoder::H264Decoder()  : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::init() {
    impl_->pkt = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->pkt || !impl_->frame) return false;
    return impl_->open(AV_CODEC_ID_H264);
}

namespace {

// Parse avcC (ISO 14496-15 §5.2.4.1.1) and populate sps/pps Annex-B.
// Returns true only when the blob is fully consumed — partial parses
// get rejected so we can fall through to the hvcC attempt.
bool parse_avcc(const uint8_t* blob, std::size_t size,
                std::vector<uint8_t>& sps_out,
                std::vector<uint8_t>& pps_out) {
    if (!blob || size < 7 || blob[0] != 0x01) return false;
    const uint8_t num_sps = blob[5] & 0x1f;
    if (num_sps < 1) return false;

    std::size_t p = 6;
    sps_out.clear();
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (p + 2 > size) return false;
        const uint16_t len =
            (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(sps_out, blob + p, len);
        p += len;
    }
    if (p >= size) return false;
    const uint8_t num_pps = blob[p++];
    pps_out.clear();
    for (uint8_t i = 0; i < num_pps; ++i) {
        if (p + 2 > size) return false;
        const uint16_t len =
            (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
        p += 2;
        if (p + len > size) return false;
        if (i == 0) append_annexb_nal(pps_out, blob + p, len);
        p += len;
    }
    // iOS avcC has no trailing bytes; a malformed blob with extras
    // looks superficially like hvcC and should fall through.
    return !sps_out.empty() && !pps_out.empty();
}

// Parse hvcC (ISO 14496-15 §8.3.3.1.2): header is 22 bytes, then
// numOfArrays (u8), each array is
//   u8  array_completeness(1) | reserved(1) | NAL_unit_type(6)
//   u16 numNalus
//   for each nalu: u16 length, byte[length] data
// HEVC NAL types: VPS=32, SPS=33, PPS=34.
bool parse_hvcc(const uint8_t* blob, std::size_t size,
                std::vector<uint8_t>& vps_out,
                std::vector<uint8_t>& sps_out,
                std::vector<uint8_t>& pps_out) {
    if (!blob || size < 23 || blob[0] != 0x01) return false;
    std::size_t p = 22;
    const uint8_t num_arrays = blob[p++];
    vps_out.clear(); sps_out.clear(); pps_out.clear();
    for (uint8_t i = 0; i < num_arrays; ++i) {
        if (p + 3 > size) return false;
        const uint8_t  nal_type = blob[p] & 0x3f;
        const uint16_t num_nalus =
            (static_cast<uint16_t>(blob[p + 1]) << 8) | blob[p + 2];
        p += 3;
        for (uint16_t j = 0; j < num_nalus; ++j) {
            if (p + 2 > size) return false;
            const uint16_t len =
                (static_cast<uint16_t>(blob[p]) << 8) | blob[p + 1];
            p += 2;
            if (p + len > size) return false;
            if (j == 0) {
                if      (nal_type == 32) append_annexb_nal(vps_out, blob + p, len);
                else if (nal_type == 33) append_annexb_nal(sps_out, blob + p, len);
                else if (nal_type == 34) append_annexb_nal(pps_out, blob + p, len);
            }
            p += len;
        }
    }
    return !vps_out.empty() && !sps_out.empty() && !pps_out.empty();
}

// Some iOS mirror flows (notably HEVC) deliver the config wrapped
// inside an MP4 VisualSampleEntry: the payload begins with
// [size:u32 BE][fourcc "avc1"|"hvc1"|"hev1"|"avc3"][...VisualSampleEntry
// fields...][child box "avcC"|"hvcC"|...]. Find the inner config
// box by scanning for its fourcc and return a pointer to the box
// content (after the 8-byte size+type header). Returns false if no
// known config child box is present.
bool find_inner_config_box(const uint8_t* blob, std::size_t size,
                           const uint8_t*& inner_blob,
                           std::size_t&    inner_size) {
    static const char* const kFourccs[] = {"avcC", "hvcC"};
    for (const char* fourcc : kFourccs) {
        // Need at least 4 bytes of size field before the fourcc.
        for (std::size_t i = 4; i + 4 <= size; ++i) {
            if (blob[i + 0] == static_cast<uint8_t>(fourcc[0]) &&
                blob[i + 1] == static_cast<uint8_t>(fourcc[1]) &&
                blob[i + 2] == static_cast<uint8_t>(fourcc[2]) &&
                blob[i + 3] == static_cast<uint8_t>(fourcc[3])) {
                const uint8_t* sz = blob + i - 4;
                const std::size_t box_size =
                    (static_cast<std::size_t>(sz[0]) << 24) |
                    (static_cast<std::size_t>(sz[1]) << 16) |
                    (static_cast<std::size_t>(sz[2]) << 8)  |
                     static_cast<std::size_t>(sz[3]);
                if (box_size >= 9 && (i - 4) + box_size <= size) {
                    inner_blob = blob + i + 4;            // after fourcc
                    inner_size = box_size - 8;            // minus header
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

// Accept either an H.264 avcC or an HEVC hvcC blob, reinit the
// decoder on codec change. Historical name kept for callers.
bool H264Decoder::set_parameter_sets_from_avcc(const uint8_t* blob,
                                               std::size_t size) {
    if (!blob || size < 7) {
        LOG_WARN << "decoder: config blob missing or too short ("
                 << size << "B)";
        return false;
    }

    // Most iOS H.264 mirror flows hand us the raw avcC blob whose
    // configurationVersion byte is 0x01. Newer HEVC mirror flows
    // wrap the config in an MP4 SampleEntry — first byte is 0x00
    // (the high byte of the box size), and the inner avcC / hvcC
    // box sits a few dozen bytes in. Detect the wrapped case and
    // unwrap once before trying the parsers.
    const uint8_t* cfg = blob;
    std::size_t    csz = size;
    if (cfg[0] != 0x01) {
        const uint8_t* inner = nullptr;
        std::size_t    isize = 0;
        if (find_inner_config_box(blob, size, inner, isize)) {
            LOG_INFO << "decoder: unwrapped MP4 SampleEntry, inner "
                     << "config box " << isize << "B";
            cfg = inner;
            csz = isize;
        } else {
            LOG_WARN << "decoder: config blob has no avcC/hvcC inner "
                        "box and no leading 0x01 (" << size << "B)";
            return false;
        }
    }
    if (cfg[0] != 0x01) {
        LOG_WARN << "decoder: inner config wrong version (0x"
                 << std::hex << static_cast<int>(cfg[0]) << ')';
        return false;
    }

    // Try H.264 avcC first (common path).
    std::vector<uint8_t> new_sps, new_pps;
    if (parse_avcc(cfg, csz, new_sps, new_pps)) {
        if (impl_->codec_id != AV_CODEC_ID_H264 &&
            !impl_->open(AV_CODEC_ID_H264)) {
            return false;
        }
        impl_->vps_annexb.clear();
        impl_->sps_annexb = std::move(new_sps);
        impl_->pps_annexb = std::move(new_pps);
        LOG_INFO << "decoder: H.264 SPS="
                 << (impl_->sps_annexb.size() - 4) << "B PPS="
                 << (impl_->pps_annexb.size() - 4) << "B cached";
        return true;
    }

    // Fall through to HEVC hvcC.
    std::vector<uint8_t> new_vps;
    new_sps.clear(); new_pps.clear();
    if (parse_hvcc(cfg, csz, new_vps, new_sps, new_pps)) {
        if (impl_->codec_id != AV_CODEC_ID_HEVC &&
            !impl_->open(AV_CODEC_ID_HEVC)) {
            return false;
        }
        impl_->vps_annexb = std::move(new_vps);
        impl_->sps_annexb = std::move(new_sps);
        impl_->pps_annexb = std::move(new_pps);
        LOG_INFO << "decoder: HEVC VPS="
                 << (impl_->vps_annexb.size() - 4) << "B SPS="
                 << (impl_->sps_annexb.size() - 4) << "B PPS="
                 << (impl_->pps_annexb.size() - 4) << "B cached";
        return true;
    }

    LOG_WARN << "decoder: config blob matched neither avcC nor hvcC ("
             << size << "B)";
    return false;
}

bool H264Decoder::decode(const uint8_t* nal_data, std::size_t nal_size,
                         bool is_idr,
                         bool& got_frame, int& width, int& height) {
    got_frame = false;
    width = height = 0;
    if (!impl_->ctx) return false;
    if (!nal_data || nal_size == 0) return false;

    // Build the Annex-B packet. Every IDR gets the parameter sets
    // prepended. H.264 needs SPS+PPS; HEVC additionally needs VPS
    // before them.
    std::vector<uint8_t> buf;
    buf.reserve(nal_size + impl_->vps_annexb.size() +
                impl_->sps_annexb.size() + impl_->pps_annexb.size());
    if (is_idr) {
        if (!impl_->vps_annexb.empty()) {
            buf.insert(buf.end(),
                       impl_->vps_annexb.begin(), impl_->vps_annexb.end());
        }
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

bool H264Decoder::is_hevc() const {
    return impl_ && impl_->codec_id == AV_CODEC_ID_HEVC;
}

} // namespace ap::video
