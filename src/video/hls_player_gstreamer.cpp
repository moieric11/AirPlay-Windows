#include "video/hls_player.h"
#include "video/video_renderer.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

// GStreamer-backed HlsPlayer. Same public API as the libavformat
// implementation in hls_player.cpp — exactly one of the two files is
// compiled into the binary (USE_GSTREAMER_HLS CMake option).
//
// Pipeline:
//   uridecodebin uri=<local master.m3u8>
//   -> videoconvert
//   -> caps=video/x-raw,format=I420
//   -> appsink name=vsink emit-signals=true sync=false
//      max-buffers=3 drop=true
//
// uridecodebin auto-selects the HLS demuxer + H.264/HEVC decoders
// through libav (avdec_h264 / avdec_h265 plugins from gst-libav).
// sync=false + drop=true + max-buffers=3 = best-effort low-latency:
// we push the newest frame we can pull and let GStreamer throw away
// anything the renderer doesn't pick up fast enough, rather than
// letting the appsink grow a big queue.

namespace ap::video {

struct HlsPlayer::Impl {
    GstElement*        pipeline     = nullptr;
    GstElement*        appsink      = nullptr;
    GMainLoop*         loop         = nullptr;
    std::thread        loop_thread;
    VideoRenderer*     renderer     = nullptr;
    std::atomic<bool>  running{false};

    static GstFlowReturn on_new_sample(GstElement* sink, gpointer user_data);
    static gboolean      on_bus_message(GstBus* bus, GstMessage* msg,
                                        gpointer user_data);
};

GstFlowReturn HlsPlayer::Impl::on_new_sample(GstElement* sink, gpointer user_data) {
    auto* self = static_cast<HlsPlayer::Impl*>(user_data);
    GstSample* sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_ERROR;

    GstCaps*      caps = gst_sample_get_caps(sample);
    GstBuffer*    buf  = gst_sample_get_buffer(sample);
    GstStructure* s    = caps ? gst_caps_get_structure(caps, 0) : nullptr;

    int w = 0, h = 0;
    if (s) {
        gst_structure_get_int(s, "width",  &w);
        gst_structure_get_int(s, "height", &h);
    }

    GstMapInfo info{};
    if (buf && w > 0 && h > 0 && gst_buffer_map(buf, &info, GST_MAP_READ)) {
        // I420 planar layout. Strides are the image width for Y and
        // width/2 for U/V when coming out of videoconvert.
        const uint8_t* y = info.data;
        const uint8_t* u = y + static_cast<std::size_t>(w) * h;
        const uint8_t* v = u + static_cast<std::size_t>(w / 2) * (h / 2);
        if (self->renderer) {
            self->renderer->push_frame(y, w,
                                       u, w / 2,
                                       v, w / 2,
                                       w, h);
        }
        gst_buffer_unmap(buf, &info);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

gboolean HlsPlayer::Impl::on_bus_message(GstBus*, GstMessage* msg, gpointer user_data) {
    auto* self = static_cast<HlsPlayer::Impl*>(user_data);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg  = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            LOG_ERROR << "HlsPlayer(gst) error: "
                      << (err ? err->message : "?")
                      << " (" << (dbg ? dbg : "") << ')';
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            if (self->loop) g_main_loop_quit(self->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            LOG_INFO << "HlsPlayer(gst) EOS";
            if (self->loop) g_main_loop_quit(self->loop);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline)) {
                GstState old_s, new_s, pending;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                LOG_INFO << "HlsPlayer(gst) state: "
                         << gst_element_state_get_name(old_s) << " -> "
                         << gst_element_state_get_name(new_s);
            }
            break;
        }
        default: break;
    }
    return TRUE;
}

HlsPlayer::HlsPlayer() : impl_(new Impl) {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}
HlsPlayer::~HlsPlayer() { stop(); delete impl_; }

bool HlsPlayer::start(const std::string& url, VideoRenderer* renderer) {
    if (impl_->running.exchange(true)) return false;
    impl_->renderer = renderer;

    std::string desc =
        "uridecodebin uri=" + url + " ! "
        "videoconvert ! "
        "video/x-raw,format=I420 ! "
        "appsink name=vsink emit-signals=true sync=false "
               "max-buffers=3 drop=true";

    GError* err = nullptr;
    impl_->pipeline = gst_parse_launch(desc.c_str(), &err);
    if (!impl_->pipeline) {
        LOG_ERROR << "HlsPlayer(gst) parse_launch failed: "
                  << (err ? err->message : "?");
        if (err) g_error_free(err);
        impl_->running = false;
        return false;
    }

    impl_->appsink = gst_bin_get_by_name(GST_BIN(impl_->pipeline), "vsink");
    g_signal_connect(impl_->appsink, "new-sample",
                     G_CALLBACK(&Impl::on_new_sample), impl_);

    GstBus* bus = gst_element_get_bus(impl_->pipeline);
    gst_bus_add_watch(bus, &Impl::on_bus_message, impl_);
    gst_object_unref(bus);

    LOG_INFO << "HlsPlayer(gst) open " << url;
    const GstStateChangeReturn rc =
        gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (rc == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR << "HlsPlayer(gst) set_state(PLAYING) failed";
        stop();
        return false;
    }

    impl_->loop = g_main_loop_new(nullptr, FALSE);
    impl_->loop_thread = std::thread([this] { g_main_loop_run(impl_->loop); });
    return true;
}

void HlsPlayer::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->loop)      g_main_loop_quit(impl_->loop);
    if (impl_->loop_thread.joinable()) impl_->loop_thread.join();
    if (impl_->pipeline) {
        gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
    }
    if (impl_->appsink) { gst_object_unref(impl_->appsink); impl_->appsink = nullptr; }
    if (impl_->loop)    { g_main_loop_unref(impl_->loop);   impl_->loop    = nullptr; }
    LOG_INFO << "HlsPlayer(gst) stopped";
}

} // namespace ap::video
