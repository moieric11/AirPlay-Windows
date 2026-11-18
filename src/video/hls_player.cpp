#include "video/hls_player.h"
#include "video/video_renderer.h"
#include "log.h"
#include "audio/audio_output.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <algorithm>
#include <cmath>
#include <vector>

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
    impl_->pending_seek.store(-1.0);
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
    av_dict_set(&opts, "rw_timeout",       "10000000", 0);
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

    int video_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, video_idx, nullptr, 0);
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
    AVFrame* converted = av_frame_alloc();
    if (!pkt || !frm || !converted) {
        av_packet_free(&pkt);
        av_frame_free(&frm);
        av_frame_free(&converted);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        running.store(false);
        return;
    }
    SwsContext* scaler = nullptr;
    AVCodecContext* audio_dec = nullptr;
    if (audio_idx >= 0) {
        auto* audio_par = fmt->streams[audio_idx]->codecpar;
        const AVCodec* audio_codec = avcodec_find_decoder(audio_par->codec_id);
        if (audio_codec) audio_dec = avcodec_alloc_context3(audio_codec);
        if (audio_dec && (avcodec_parameters_to_context(audio_dec, audio_par) < 0 ||
                         avcodec_open2(audio_dec, audio_codec, nullptr) < 0)) {
            avcodec_free_context(&audio_dec);
        }
    }
    ap::audio::SdlAudioOutput audio;
    int audio_rate = 0, audio_channels = 0;
    bool anchored = false;
    double media_anchor = 0.0;
    auto wall_anchor = std::chrono::steady_clock::now();
    // Pace decoded timestamps against a monotonic clock. Pause moves the
    // anchor so resuming never races through all the buffered video.
    auto wait_until = [&](double pts) {
        if (!anchored) {
            anchored = true;
            media_anchor = pts;
            wall_anchor = std::chrono::steady_clock::now();
        }
        while (running.load() && pending_seek.load() < 0.0) {
            const auto before = std::chrono::steady_clock::now();
            if (rate.load() <= 0.0) {
                audio.pause(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                wall_anchor += std::chrono::steady_clock::now() - before;
                continue;
            }
            audio.pause(false);
            const double elapsed = std::chrono::duration<double>(before - wall_anchor).count();
            if (pts - media_anchor <= elapsed) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    bool eof = false;
    auto receive_frames = [&](AVCodecContext* decoder, int index) {
        while (running.load()) {
            const int rc = avcodec_receive_frame(decoder, frm);
            if (rc < 0) break;
            const double pts = frm->best_effort_timestamp == AV_NOPTS_VALUE
                ? media_anchor : frm->best_effort_timestamp * av_q2d(fmt->streams[index]->time_base);
            if (!wait_until(pts)) { av_frame_unref(frm); break; }
            if (index == video_idx && renderer && frm->width > 0 && frm->height > 0) {
                AVFrame* display = frm;
                if (frm->format != AV_PIX_FMT_YUV420P) {
                    scaler = sws_getCachedContext(scaler, frm->width, frm->height,
                        static_cast<AVPixelFormat>(frm->format), frm->width, frm->height,
                        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    av_frame_unref(converted);
                    converted->format = AV_PIX_FMT_YUV420P;
                    converted->width = frm->width;
                    converted->height = frm->height;
                    if (!scaler || av_frame_get_buffer(converted, 32) < 0) {
                        av_frame_unref(frm);
                        continue;
                    }
                    sws_scale(scaler, frm->data, frm->linesize, 0, frm->height,
                              converted->data, converted->linesize);
                    display = converted;
                }
                renderer->push_frame(display->data[0], display->linesize[0],
                                     display->data[1], display->linesize[1],
                                     display->data[2], display->linesize[2],
                                     display->width, display->height);
            } else if (index == audio_idx) {
                const int channels = frm->ch_layout.nb_channels;
                if (channels > 0 && frm->sample_rate > 0) {
                    if (audio_rate != frm->sample_rate || audio_channels != channels) {
                        audio.stop();
                        if (audio.start(frm->sample_rate, channels)) {
                            audio_rate = frm->sample_rate;
                            audio_channels = channels;
                        }
                    }
                    const auto format = static_cast<AVSampleFormat>(frm->format);
                    const auto packed = av_get_packed_sample_fmt(format);
                    const bool planar = av_sample_fmt_is_planar(format) != 0;
                    std::vector<int16_t> pcm(static_cast<size_t>(frm->nb_samples) * channels);
                    for (int n = 0; n < frm->nb_samples; ++n) {
                        for (int c = 0; c < channels; ++c) {
                            const auto* data = frm->extended_data[planar ? c : 0];
                            const int offset = planar ? n : n * channels + c;
                            double value = 0.0;
                            switch (packed) {
                                case AV_SAMPLE_FMT_U8: value = (data[offset] - 128) / 128.0; break;
                                case AV_SAMPLE_FMT_S16: value = reinterpret_cast<const int16_t*>(data)[offset] / 32768.0; break;
                                case AV_SAMPLE_FMT_S32: value = reinterpret_cast<const int32_t*>(data)[offset] / 2147483648.0; break;
                                case AV_SAMPLE_FMT_S64: value = static_cast<double>(reinterpret_cast<const int64_t*>(data)[offset]) / 9223372036854775808.0; break;
                                case AV_SAMPLE_FMT_FLT: value = reinterpret_cast<const float*>(data)[offset]; break;
                                case AV_SAMPLE_FMT_DBL: value = reinterpret_cast<const double*>(data)[offset]; break;
                                default: break;
                            }
                            if (!std::isfinite(value)) value = 0.0;
                            pcm[static_cast<size_t>(n) * channels + c] = static_cast<int16_t>(
                                std::clamp(value * 32768.0, -32768.0, 32767.0));
                        }
                    }
                    audio.push(pcm.data(), static_cast<int>(pcm.size()));
                }
            }
            av_frame_unref(frm);
        }
    };

    while (running.load()) {
        const double seek_to = pending_seek.exchange(-1.0);
        if (seek_to >= 0.0) {
            const int64_t ts =
                static_cast<int64_t>(seek_to * static_cast<double>(AV_TIME_BASE));
            const int rc = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (rc >= 0) {
                avcodec_flush_buffers(dec);
                if (audio_dec) avcodec_flush_buffers(audio_dec);
                audio.clear();
                anchored = false;
                LOG_INFO << "HlsPlayer seek position=" << seek_to << 's';
            } else {
                char errbuf[128] = {0};
                av_strerror(rc, errbuf, sizeof(errbuf));
                LOG_WARN << "HlsPlayer seek failed position=" << seek_to
                         << "s: " << errbuf;
            }
        }
        while (running.load() && rate.load() <= 0.0) {
            audio.pause(true);
            const auto before = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            wall_anchor += std::chrono::steady_clock::now() - before;
            if (pending_seek.load() >= 0.0) break;
        }
        if (!running.load()) break;
        if (pending_seek.load() >= 0.0) continue;
        audio.pause(false);

        const int read_rc = av_read_frame(fmt, pkt);
        if (read_rc == AVERROR_EOF) {
            LOG_INFO << "HlsPlayer: EOF";
            avcodec_send_packet(dec, nullptr);
            receive_frames(dec, video_idx);
            if (audio_dec) {
                avcodec_send_packet(audio_dec, nullptr);
                receive_frames(audio_dec, audio_idx);
            }
            eof = true;
            break;
        }
        if (read_rc < 0) {
            char errbuf[128] = {0};
            av_strerror(read_rc, errbuf, sizeof(errbuf));
            LOG_WARN << "HlsPlayer av_read_frame: " << errbuf;
            break;
        }
        const int index = pkt->stream_index;
        AVCodecContext* decoder = index == video_idx ? dec : (index == audio_idx ? audio_dec : nullptr);
        if (!decoder) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(decoder, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        receive_frames(decoder, index);
    }

    while (eof && running.load() && audio.queued_bytes() > 0) {
        if (!wait_until(media_anchor)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (eof && running.load()) notify_end();
    running.store(false);
    audio.stop();
    avcodec_free_context(&audio_dec);
    sws_freeContext(scaler);
    av_frame_free(&converted);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    LOG_INFO << "HlsPlayer stopped";
}

} // namespace ap::video
