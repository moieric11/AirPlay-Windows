#include "video/hls_player.h"
#include "video/video_renderer.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// libavformat-backed HlsPlayer. Default backend; the GStreamer
// variant lives in hls_player_gstreamer.cpp and is selected with
// USE_GSTREAMER_HLS=ON at CMake time.

namespace ap::video {

struct HlsPlayer::Impl {
    std::atomic<bool> running{false};
    std::atomic<double> rate{1.0};
    std::atomic<double> pending_seek{-1.0};
    std::thread       thread;
    VideoRenderer*    renderer = nullptr;
    std::mutex        callback_mtx;
    std::function<void()> end_callback;

    void run(std::string url);
    static int interrupt_cb(void* opaque);
    void notify_end();
};

HlsPlayer::HlsPlayer()  : impl_(new Impl) {}
HlsPlayer::~HlsPlayer() { stop(); delete impl_; }

bool HlsPlayer::start(const std::string& url, VideoRenderer* renderer) {
    stop();
    impl_->running.store(true);
    impl_->rate.store(1.0);
    impl_->renderer = renderer;
    impl_->thread   = std::thread([this, url] { impl_->run(url); });
    return true;
}

void HlsPlayer::set_rate(double rate) {
    impl_->rate.store(rate);
}

void HlsPlayer::seek(double position_seconds) {
    if (position_seconds >= 0.0) {
        impl_->pending_seek.store(position_seconds);
    }
}

void HlsPlayer::set_end_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mtx);
    impl_->end_callback = std::move(callback);
}

void HlsPlayer::stop() {
    impl_->running.store(false);
    if (impl_->thread.joinable()) impl_->thread.join();
}

int HlsPlayer::Impl::interrupt_cb(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    return (self && !self->running.load()) ? 1 : 0;
}

void HlsPlayer::Impl::notify_end() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mtx);
        cb = end_callback;
    }
    if (cb) cb();
}

void HlsPlayer::Impl::run(std::string url) {
    LOG_INFO << "HlsPlayer open " << url;

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        LOG_ERROR << "HlsPlayer avformat_alloc_context failed";
        running = false;
        return;
    }
    fmt->interrupt_callback.callback = &HlsPlayer::Impl::interrupt_cb;
    fmt->interrupt_callback.opaque   = this;

    AVDictionary*    opts = nullptr;
    av_dict_set(&opts, "probesize",        "131072", 0);
    av_dict_set(&opts, "analyzeduration",  "500000", 0);
    av_dict_set(&opts, "fflags",           "nobuffer+discardcorrupt", 0);
    av_dict_set(&opts, "flags",            "low_delay", 0);
    av_dict_set(&opts, "max_delay",        "0", 0);
    av_dict_set(&opts, "reconnect",        "1", 0);
    av_dict_set(&opts, "reconnect_streamed","1", 0);
    av_dict_set(&opts, "extension_picky",    "0",   0);
    av_dict_set(&opts, "allowed_extensions", "ALL", 0);
    const int open_rc = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (open_rc < 0 || !fmt) {
        char errbuf[128] = {0};
        av_strerror(open_rc, errbuf, sizeof(errbuf));
        LOG_ERROR << "HlsPlayer avformat_open_input failed: " << errbuf;
        if (fmt) avformat_close_input(&fmt);
        running = false;
        return;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        LOG_ERROR << "HlsPlayer avformat_find_stream_info failed";
        avformat_close_input(&fmt);
        running = false;
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
        running = false;
        return;
    }

    AVCodecParameters* par = fmt->streams[video_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        LOG_ERROR << "HlsPlayer: no decoder for codec_id " << par->codec_id;
        avformat_close_input(&fmt);
        running = false;
        return;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, par) < 0 ||
        avcodec_open2(dec, codec, nullptr) < 0) {
        LOG_ERROR << "HlsPlayer: video decoder init failed";
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        running = false;
        return;
    }
    LOG_INFO << "HlsPlayer video: " << avcodec_get_name(par->codec_id)
             << ' ' << par->width << 'x' << par->height;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();

    while (running.load()) {
        const double seek_to = pending_seek.exchange(-1.0);
        if (seek_to >= 0.0) {
            const int64_t ts =
                static_cast<int64_t>(seek_to * static_cast<double>(AV_TIME_BASE));
            const int rc = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (rc >= 0) {
                avcodec_flush_buffers(dec);
                LOG_INFO << "HlsPlayer seek position=" << seek_to << 's';
            } else {
                char errbuf[128] = {0};
                av_strerror(rc, errbuf, sizeof(errbuf));
                LOG_WARN << "HlsPlayer seek failed position=" << seek_to
                         << "s: " << errbuf;
            }
        }
        while (running.load() && rate.load() <= 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!running.load()) break;

        const int read_rc = av_read_frame(fmt, pkt);
        if (read_rc == AVERROR_EOF) {
            LOG_INFO << "HlsPlayer: EOF";
            running.store(false);
            notify_end();
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
        while (running.load()) {
            const int recv_rc = avcodec_receive_frame(dec, frm);
            if (recv_rc == AVERROR(EAGAIN) || recv_rc == AVERROR_EOF) break;
            if (recv_rc < 0) break;
            if (renderer && frm->width > 0 && frm->height > 0) {
                renderer->push_frame(frm->data[0], frm->linesize[0],
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
