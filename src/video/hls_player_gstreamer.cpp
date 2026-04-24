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
// Pipeline (via playbin3):
//   - URI ->  demuxer (hlsdemux) -> decoders (avdec_h264/h265, audio)
//   - video-sink = appsink (caps=video/x-raw,format=I420, sync=true,
//                           drop=true, max-buffers=3) -> push_frame()
//   - audio-sink = default system sink (autoaudiosink -> WASAPI on
//                  Windows, pulseaudio on Linux) so YouTube audio
//                  plays natively without any code from our side.
//
// playbin handles HLS variant selection, demuxing, decoding and clock
// synchronisation. max-buffers=3 + drop=true on the video appsink
// keeps playback paced without letting frames pile up if the GUI
// thread is briefly stalled.

namespace ap::video {

struct HlsPlayer::Impl {
    GstElement*        pipeline     = nullptr;
    GstElement*        appsink      = nullptr;
    GMainLoop*         loop         = nullptr;
    std::thread        loop_thread;
    guint              bus_watch_id = 0;
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

    // playbin = full auto demux + decode + sink selection. We supply
    // our own video sink (appsink wired to push_frame); playbin picks
    // a system audio sink by default, which on Windows means WASAPI
    // and on Linux either PulseAudio or PipeWire via autoaudiosink.
    impl_->pipeline = gst_element_factory_make("playbin", "player");
    if (!impl_->pipeline) {
        // "playbin" was renamed to "playbin3" in 1.22+ on some
        // builds; fall back if the default factory isn't there.
        impl_->pipeline = gst_element_factory_make("playbin3", "player");
    }
    if (!impl_->pipeline) {
        LOG_ERROR << "HlsPlayer(gst) neither playbin nor playbin3 available";
        impl_->running = false;
        return false;
    }
    g_object_set(impl_->pipeline, "uri", url.c_str(), NULL);

    // Video sink: appsink emitting YUV420 buffers for push_frame().
    impl_->appsink = gst_element_factory_make("appsink", "vsink");
    g_object_set(impl_->appsink,
        "emit-signals", TRUE,
        "sync",        TRUE,
        "max-buffers", static_cast<guint>(3),
        "drop",        TRUE,
        NULL);
    GstCaps* caps = gst_caps_from_string("video/x-raw,format=I420");
    g_object_set(impl_->appsink, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_object_set(impl_->pipeline, "video-sink", impl_->appsink, NULL);
    g_signal_connect(impl_->appsink, "new-sample",
                     G_CALLBACK(&Impl::on_new_sample), impl_);

    GstBus* bus = gst_element_get_bus(impl_->pipeline);
    impl_->bus_watch_id = gst_bus_add_watch(bus, &Impl::on_bus_message, impl_);
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

    // Drain the pipeline to NULL *before* quitting the main loop so
    // GStreamer has a chance to flush any in-flight buffer/message
    // while its thread is still running. Wait synchronously for the
    // state transition to complete, otherwise we can destroy the bus
    // with callbacks still pending and hit a use-after-free on the
    // next start().
    if (impl_->pipeline) {
        gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
        gst_element_get_state(impl_->pipeline, nullptr, nullptr,
                              GST_CLOCK_TIME_NONE);
    }
    // Remove the bus watch before quitting the loop / destroying the
    // pipeline. Otherwise the watch source stays registered on the
    // default main context and fires against a freed impl_ next time.
    if (impl_->bus_watch_id) {
        g_source_remove(impl_->bus_watch_id);
        impl_->bus_watch_id = 0;
    }
    if (impl_->loop) g_main_loop_quit(impl_->loop);
    if (impl_->loop_thread.joinable()) impl_->loop_thread.join();

    // appsink is owned by the pipeline (g_object_set on "video-sink"
    // sank its floating ref). Don't unref it separately — that's a
    // double-free and crashes on the next start/stop cycle.
    if (impl_->pipeline) {
        gst_object_unref(impl_->pipeline);
        impl_->pipeline = nullptr;
    }
    impl_->appsink = nullptr;

    if (impl_->loop) {
        g_main_loop_unref(impl_->loop);
        impl_->loop = nullptr;
    }
    impl_->renderer = nullptr;
    LOG_INFO << "HlsPlayer(gst) stopped";
}

} // namespace ap::video
