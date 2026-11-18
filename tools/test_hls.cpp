#include "airplay/hls_local_server.h"
#include "airplay/hls_session.h"
#include "video/hls_player.h"
#include "video/video_renderer.h"
#include "video/video_recorder.h"
#include "net/socket.h"
#include "log.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
std::atomic<int> frames{0};
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class P> void wait_for(P predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "playback deadline");
        std::this_thread::sleep_for(5ms);
    }
}
std::string get(uint16_t port, const std::string& path, const std::string& extra = "", bool slow = false) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (slow) {
        int buffer_size = 4096;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra + "\r\n";
    ap::net::send_all(fd, request.data(), static_cast<int>(request.size()));
    if (slow) std::this_thread::sleep_for(3s);
    std::string result;
    char buf[4096];
    int n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) result.append(buf, n);
    ap::net::close_socket(fd);
    return result;
}
// A two-second raw BGR + PCM fixture requires no optional encoders.
void make_media(const char* filename) {
    AVFormatContext* fmt = nullptr;
    require(avformat_alloc_output_context2(&fmt, nullptr, "avi", filename) >= 0, "AVI context");
    AVStream* v = avformat_new_stream(fmt, nullptr);
    v->time_base = AVRational{1, 10};
    v->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    v->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    v->codecpar->format = AV_PIX_FMT_BGR24;
    v->codecpar->bits_per_coded_sample = 24;
    v->codecpar->width = 32;
    v->codecpar->height = 32;
    AVStream* a = avformat_new_stream(fmt, nullptr);
    a->time_base = AVRational{1, 44100};
    a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    a->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    a->codecpar->sample_rate = 44100;
    a->codecpar->bits_per_coded_sample = 16;
    a->codecpar->block_align = 4;
    a->codecpar->bit_rate = 44100 * 32;
    av_channel_layout_default(&a->codecpar->ch_layout, 2);
    require(avio_open(&fmt->pb, filename, AVIO_FLAG_WRITE) >= 0, "fixture open");
    require(avformat_write_header(fmt, nullptr) >= 0, "fixture header");
    AVPacket* pkt = av_packet_alloc();
    for (int i = 0; i < 20; ++i) {
        require(av_new_packet(pkt, 32 * 32 * 3) >= 0, "video packet");
        std::fill(pkt->data, pkt->data + pkt->size, static_cast<uint8_t>(i * 10));
        pkt->stream_index = v->index;
        pkt->pts = pkt->dts = i;
        pkt->duration = 1;
        require(av_interleaved_write_frame(fmt, pkt) >= 0, "write video");
        require(av_new_packet(pkt, 4410 * 4) >= 0, "audio packet");
        std::fill(pkt->data, pkt->data + pkt->size, uint8_t{0});
        pkt->stream_index = a->index;
        pkt->pts = pkt->dts = i * 4410;
        pkt->duration = 4410;
        require(av_interleaved_write_frame(fmt, pkt) >= 0, "write audio");
    }
    av_packet_free(&pkt);
    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
}
void make_ts(const char* filename) {
    AVFormatContext* fmt = nullptr;
    require(avformat_alloc_output_context2(&fmt, nullptr, "mpegts", filename) >= 0, "TS context");
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    require(codec != nullptr, "MPEG2 test encoder");
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->width = enc->height = 32;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, 10};
    enc->framerate = AVRational{10, 1};
    enc->gop_size = 10;
    require(avcodec_open2(enc, codec, nullptr) >= 0, "TS encoder open");
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    stream->time_base = enc->time_base;
    avcodec_parameters_from_context(stream->codecpar, enc);
    require(avio_open(&fmt->pb, filename, AVIO_FLAG_WRITE) >= 0 &&
            avformat_write_header(fmt, nullptr) >= 0, "TS header");
    AVFrame* frame = av_frame_alloc();
    frame->width = frame->height = 32;
    frame->format = enc->pix_fmt;
    require(av_frame_get_buffer(frame, 32) >= 0, "TS frame");
    AVPacket* packet = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(enc, packet) >= 0) {
            av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
            packet->stream_index = stream->index;
            require(av_interleaved_write_frame(fmt, packet) >= 0, "TS packet");
        }
    };
    for (int i = 0; i < 20; ++i) {
        av_frame_make_writable(frame);
        for (int plane = 0; plane < 3; ++plane)
            for (int row = 0; row < (plane ? 16 : 32); ++row)
                std::fill_n(frame->data[plane] + row * frame->linesize[plane], plane ? 16 : 32,
                            static_cast<uint8_t>(plane ? 128 : 16 + i * 8));
        frame->pts = i;
        require(avcodec_send_frame(enc, frame) >= 0, "TS encode");
        drain();
    }
    avcodec_send_frame(enc, nullptr);
    drain();
    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
}
}
// Headless renderer sink: validate actual decoded planes without creating a GUI.
namespace ap::video {
VideoRecorder::~VideoRecorder() = default;
DecodedFrame::~DecodedFrame() { av_frame_free(&frame); }
VideoRenderer::VideoRenderer() = default;
VideoRenderer::~VideoRenderer() = default;
void VideoRenderer::push_frame(const uint8_t* y, int ys, const uint8_t* u, int us,
                              const uint8_t* v, int vs, int w, int h, int64_t) {
    require(y && u && v && ys >= w && us >= w / 2 && vs >= w / 2 && w == 32 && h == 32,
            "converted YUV420 planes");
    ++frames;
}
}
int main() {
    ap::set_log_enabled(true);
    require(ap::net::global_init(), "Winsock init");
    const std::string master = "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"first\",URI=\"mlhls://localhost/a.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"default\",DEFAULT=YES,URI=\"mlhls://localhost/b.m3u8?token=1\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000,AUDIO=\"a\"\nmlhls://localhost/v.m3u8?token=2\n";
    const auto filtered = ap::airplay::filter_master_to_single_variant(master);
    require(filtered.find("NAME=\"default\"") != std::string::npos &&
            filtered.find("NAME=\"first\"") == std::string::npos, "default audio selection");
    const auto uris = ap::airplay::extract_media_uris(filtered);
    require(uris.size() == 2 && uris[0] == "mlhls://localhost/b.m3u8?token=1" &&
            uris[1] == "mlhls://localhost/v.m3u8?token=2", "child query tokens preserved");
    const std::string low = "#EXT-X-STREAM-INF:BANDWIDTH=100000,RESOLUTION=256x144,AUDIO=\"low\"\nlow.m3u8\n";
    const std::string hd = "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028,mp4a.40.2\",AUDIO=\"hd\"\nhd.m3u8\n";
    const std::string ultra = "#EXT-X-STREAM-INF:BANDWIDTH=20000000,RESOLUTION=3840x2160\nultra.m3u8\n";
    const std::string audio_groups = "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"low\",NAME=\"low-audio\",URI=\"low-audio.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"hd\",NAME=\"hd-audio\",URI=\"hd-audio.m3u8\"\n";
    for (const auto& variants : {low + hd + ultra, ultra + hd + low, hd + low + ultra}) {
        const auto result = ap::airplay::filter_master_to_single_variant(audio_groups + variants);
        require(result.find("RESOLUTION=1920x1080") != std::string::npos &&
                result.find("RESOLUTION=256x144") == std::string::npos &&
                result.find("RESOLUTION=3840x2160") == std::string::npos &&
                result.find("NAME=\"hd-audio\"") != std::string::npos &&
                result.find("NAME=\"low-audio\"") == std::string::npos,
                "1080p and matching audio selected regardless of playlist order");
    }
    const auto portrait = ap::airplay::filter_master_to_single_variant("#EXTM3U\n" + low +
        "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1080x1920\nportrait.m3u8\n" +
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=720x1280\nportrait-low.m3u8\n");
    require(portrait.find("1080x1920") != std::string::npos && portrait.find("720x1280") == std::string::npos,
            "vertical Full HD Shorts");
    const auto codec_choice = ap::airplay::filter_master_to_single_variant("#EXTM3U\n" +
        std::string("#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080,CODECS=\"vp09.00.40.08\"\nvp9.m3u8\n") + hd);
    require(codec_choice.find("avc1") != std::string::npos && codec_choice.find("vp09") == std::string::npos,
            "prefer H264 at the same resolution");
    require(ap::airplay::filter_master_to_single_variant("#EXTM3U\n" + ultra).find("ultra.m3u8") != std::string::npos,
            "available rendition retained when no Full HD variant exists");
    const auto expanded = ap::airplay::expand_yt_condensed_playlist(
        "#EXTM3U\r\n#YT-EXT-CONDENSED-URL:BASE-URI=\"https://cdn/clip\",PARAMS=\"sq\",PREFIX=\"mlhls://localhost/seg\"\r\n"
        "#EXTINF:2,\r\nmlhls://localhost/seg/4\r\n");
    require(expanded.find("https://cdn/clip/sq/4\n") != std::string::npos,
            "YouTube condensed CRLF segment URL");
    ap::airplay::HlsSession first, second;
    size_t count = 0;
    const auto first_rewrite = ap::airplay::rewrite_segments_to_local(first, 7100, "https://cdn/one\n", count);
    const auto second_rewrite = ap::airplay::rewrite_segments_to_local(second, 7100, "https://cdn/two\n", count);
    require(first_rewrite != second_rewrite, "segment identifiers never alias across sessions");
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    make_ts("test-hls-fixture.ts");
    std::ifstream ts_file("test-hls-fixture.ts", std::ios::binary);
    const std::string ts_data((std::istreambuf_iterator<char>(ts_file)), {});
    ts_file.close();
    ap::net::TcpServer upstream;
    const std::string data = "0123456789abcdefghijklmnopqrstuvwxyz";
    const std::string large(512 * 1024, 'X');
    require(upstream.start(0, [&](ap::net::ClientSocket c) {
        std::string request;
        char ch;
        while (::recv(c.fd, &ch, 1, 0) == 1) {
            request += ch;
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        const std::string& resource = request.find("GET /video.ts ") != std::string::npos ? ts_data :
            (request.find("GET /large ") != std::string::npos ? large : data);
        const auto pos = request.find("Range: bytes=");
        int start = 0, end = static_cast<int>(resource.size()) - 1;
        if (pos != std::string::npos) {
            const std::string range = request.substr(pos + 13);
            start = std::stoi(range);
            const auto dash = range.find('-');
            if (dash + 1 < range.size() && range[dash + 1] >= '0' && range[dash + 1] <= '9')
                end = std::min(end, std::stoi(range.substr(dash + 1)));
        }
        const std::string body = resource.substr(start, end - start + 1);
        std::string wire = pos == std::string::npos ? "HTTP/1.1 200 OK\r\n" :
            "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) +
            "-" + std::to_string(end) + "/" + std::to_string(resource.size()) + "\r\n";
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        ap::net::send_all(c.fd, wire.data(), static_cast<int>(wire.size()));
    }), "upstream start");
    auto& registry = ap::airplay::HlsSessionRegistry::instance();
    auto* s = registry.get_or_create("test");
    registry.set_active_session("test");
    const auto initial_epoch = registry.playback_epoch();
    const int initial_generation = s->playback_generation.load();
    s->master_url = "mlhls://localhost/old.m3u8";
    s->master_playlist = "old video";
    s->seg_url_map["/seg/old"] = "https://old-video/segment";
    registry.get_or_create("replacement");
    registry.set_active_session("replacement");
    std::string stale;
    bool master_flag = false;
    require(!registry.lookup_playlist("mlhls://localhost/old.m3u8", stale, master_flag) &&
            registry.resolve_segment_path("/seg/old").empty(), "no fallback to inactive video caches");
    require(s->playback_generation.load() != initial_generation &&
            registry.playback_epoch() != initial_epoch && !registry.is_active_session("test"),
            "video switch invalidates old requests and URLs");
    registry.set_active_session("test");
    s->seg_url_map["/seg/0"] = "http://127.0.0.1:" + std::to_string(upstream.port()) + "/data";
    s->master_url = "mlhls://localhost/custom.m3u8?token=abc";
    s->master_playlist = "#EXTM3U\n#EXT-X-ENDLIST\n";
    ap::airplay::HlsLocalServer proxy;
    require(proxy.start(0), "proxy start");
    const auto current_prefix = "/hls/" + std::to_string(registry.playback_epoch());
    auto expired = get(proxy.port(), "/hls/" + std::to_string(initial_epoch) + "/custom.m3u8?token=abc");
    require(expired.find("410 Gone") != std::string::npos, "old player URL cannot read the next video");
    auto scoped = get(proxy.port(), current_prefix + "/custom.m3u8?token=abc");
    require(scoped.find("200 OK") != std::string::npos, "current player URL reads current video");
    auto response = get(proxy.port(), "/seg/0", "Range: bytes=5-9\r\n");
    require(response.find("206 Partial Content") != std::string::npos &&
            response.find("Content-Length: 5\r\n") != std::string::npos &&
            response.find("Content-Range: bytes 5-9/36\r\n") != std::string::npos &&
            response.substr(response.find("\r\n\r\n") + 4) == "56789", "bounded range headers and bytes");
    response = get(proxy.port(), "/seg/0", "Range: bytes=30-\r\n");
    require(response.find("Content-Length: 6\r\n") != std::string::npos &&
            response.find("Content-Range: bytes 30-35/36\r\n") != std::string::npos &&
            response.substr(response.find("\r\n\r\n") + 4) == "uvwxyz", "open range");
    response = get(proxy.port(), "/seg/0");
    require(response.substr(response.find("\r\n\r\n") + 4) == data, "complete segment");
    s->seg_url_map["/seg/slow"] = "http://127.0.0.1:" + std::to_string(upstream.port()) + "/large";
    response = get(proxy.port(), "/seg/slow", "", true);
    require(response.substr(response.find("\r\n\r\n") + 4) == large,
            "slow/paused consumer receives the complete segment after three seconds");
    response = get(proxy.port(), "/custom.m3u8?token=abc");
    require(response.find("200 OK") != std::string::npos && response.find("#EXTM3U") != std::string::npos,
            "playlist path with query");
    {
        std::lock_guard<std::mutex> lock(s->seg_mtx);
        s->master_playlist = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:2.0,\nhttp://localhost:" +
            std::to_string(proxy.port()) + "/seg/1\n#EXT-X-ENDLIST\n";
    }
    s->seg_url_map["/seg/1"] = "http://127.0.0.1:" + std::to_string(upstream.port()) + "/video.ts";
    {
        ap::video::VideoRenderer sink;
        ap::video::HlsPlayer hls;
        std::atomic<bool> done{false};
        hls.set_end_callback([&] { done = true; });
        hls.start("http://localhost:" + std::to_string(proxy.port()) + "/custom.m3u8?token=abc", &sink);
        wait_for([&] { return done.load(); });
        hls.stop();
        require(frames.load() == 20, "HLS playlist to HTTP proxy to MPEG-TS to decoded frames");
        frames = 0;
    }
    proxy.stop();
    upstream.stop();
    make_media("test-hls-fixture.avi");
    ap::video::VideoRenderer renderer;
    ap::video::HlsPlayer player;
    std::atomic<bool> ended{false};
    player.set_end_callback([&] { ended = true; });
    const auto began = std::chrono::steady_clock::now();
    require(player.start("test-hls-fixture.avi", &renderer), "player start");
    wait_for([] { return frames.load() >= 3; });
    player.set_rate(0.0);
    std::this_thread::sleep_for(50ms);
    const int paused = frames.load();
    std::this_thread::sleep_for(200ms);
    require(frames.load() == paused && !ended, "pause freezes playback");
    player.set_rate(1.0);
    wait_for([&] { return ended.load(); });
    require(frames.load() == 20, "all video frames including delayed decoder output");
    require(std::chrono::steady_clock::now() - began >= 2100ms, "real-time pacing and audio drain");
    player.stop();
    ended = false;
    frames = 0;
    player.start("test-hls-fixture.avi", &renderer);
    player.seek(1.0);
    wait_for([] { return frames.load() >= 2; });
    player.set_rate(0.0);
    const auto stopping = std::chrono::steady_clock::now();
    player.stop();
    require(std::chrono::steady_clock::now() - stopping < 500ms && !ended, "cancel paused playback without EOS");
    player.start("test-hls-fixture.avi", &renderer);
    wait_for([&] { return ended.load(); });
    player.stop();
    std::remove("test-hls-fixture.avi");
    std::remove("test-hls-fixture.ts");
    SDL_Quit();
    ap::net::global_shutdown();
    std::cout << "PASS: HLS proxy ranges, query playlists, decoded video/audio, pacing, pause, seek, restart, stop\n";
    return 0;
}
