#include "video/hls_player.h"
#include "video/video_renderer.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <cstring>

namespace ap::video {

bool HlsPlayer::start(const std::string& url, VideoRenderer* renderer) {
    if (running_.exchange(true)) return false;
    renderer_ = renderer;
    thread_   = std::thread(&HlsPlayer::run, this, url);
    return true;
}

void HlsPlayer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void HlsPlayer::run(std::string url) {
    LOG_INFO << "HlsPlayer open " << url;

    AVFormatContext* fmt = nullptr;
    AVDictionary*    opts = nullptr;
    // Trim probe + analyze to the minimum FFmpeg tolerates for HLS.
    // 128 KB / 500 ms is enough to see one segment, and gives us the
    // first frame much sooner than the defaults.
    av_dict_set(&opts, "probesize",        "131072", 0);
    av_dict_set(&opts, "analyzeduration",  "500000", 0);
    // Low-latency playback options. nobuffer + low_delay + max_delay=0
    // tell the demuxer/decoder to prefer "emit now" over "smooth".
    // discardcorrupt hides the few lost TS packets typical of an HLS
    // segment boundary so they don't jam the timeline.
    av_dict_set(&opts, "fflags",           "nobuffer+discardcorrupt", 0);
    av_dict_set(&opts, "flags",            "low_delay", 0);
    av_dict_set(&opts, "max_delay",        "0", 0);
    av_dict_set(&opts, "reconnect",        "1", 0);
    av_dict_set(&opts, "reconnect_streamed","1", 0);
    // YouTube HLS segment URLs end with "/<index>" or query params, no
    // .ts/.m4s extension. Bypass FFmpeg's whitelist check.
    av_dict_set(&opts, "extension_picky",    "0",   0);
    av_dict_set(&opts, "allowed_extensions", "ALL", 0);
    const int open_rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (open_rc < 0 || !fmt) {
        char errbuf[128] = {0};
        av_strerror(open_rc, errbuf, sizeof(errbuf));
        LOG_ERROR << "HlsPlayer avformat_open_input failed: " << errbuf;
        running_ = false;
        return;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_ERROR << "HlsPlayer avformat_find_stream_info failed";
        avformat_close_input(&fmt);
        running_ = false;
        return;
    }

    int video_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }
    if (video_idx < 0) {
        LOG_ERROR << "HlsPlayer: no video stream in " << url;
        avformat_close_input(&fmt);
        running_ = false;
        return;
    }

    AVCodecParameters* par = fmt->streams[video_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        LOG_ERROR << "HlsPlayer: no decoder for codec_id " << par->codec_id;
        avformat_close_input(&fmt);
        running_ = false;
        return;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, par) < 0 ||
        avcodec_open2(dec, codec, nullptr) < 0) {
        LOG_ERROR << "HlsPlayer: video decoder init failed";
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        running_ = false;
        return;
    }
    LOG_INFO << "HlsPlayer video: " << avcodec_get_name(par->codec_id)
             << ' ' << par->width << 'x' << par->height;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();

    while (running_.load()) {
        const int read_rc = av_read_frame(fmt, pkt);
        if (read_rc == AVERROR_EOF) {
            LOG_INFO << "HlsPlayer: EOF";
            break;
        }
        if (read_rc < 0) {
            char errbuf[128] = {0};
            av_strerror(read_rc, errbuf, sizeof(errbuf));
            LOG_WARN << "HlsPlayer av_read_frame: " << errbuf;
            break;
        }
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        while (running_.load()) {
            const int recv_rc = avcodec_receive_frame(dec, frm);
            if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) break;
            if (recv_rc < 0) break;
            if (renderer_ && frm->width > 0 && frm->height > 0) {
                renderer_->push_frame(frm->data[0], frm->linesize[0],
                                      frm->data[1], frm->linesize[1],
                                      frm->data[2], frm->linesize[2],
                                      frm->width, frm->height);
            }
            av_frame_unref(frm);
        }
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    LOG_INFO << "HlsPlayer stopped";
}

} // namespace ap::video
