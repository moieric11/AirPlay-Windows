#include "video/face_swap.h"
#include "video/face_swap_gpu.h"
#include "video/face_swap_cadence.h"
#include "net/socket.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace ap::video {
namespace {
constexpr uint32_t kMagic = 0x46535731; // FSW1, network byte order
constexpr uint32_t kGpuMagic = 0x46535732;
constexpr uint32_t kGpuError = 0x46535733;
constexpr uint32_t kMaskConfig = 0x46535734;
using Clock = std::chrono::steady_clock;
}

struct FaceSwap::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    bool error = false;
    bool cpu = false;
    bool directml = false;
    int max_frame_age_ms = 500;
    uint64_t video_generation=0;
    std::string message = "Disabled";
    std::string log;
    std::string gpu_fallback;
    bool preparing=false;
    Clock::time_point preparation_started{}, progress_checked{};
    std::string preparation_stage;
    std::array<uint32_t,14> mask_settings{};
    uint64_t mask_revision=0;
    bool live_mask_allowed=false;
    socket_t socket = INVALID_SOCK;
    AVFrame* pending = nullptr;
    AVFrame* last_input = nullptr; // Original input only; never replay a composited output.
    int64_t last_input_origin = 0;
    AVFrame* result = nullptr;
    int input_w = 0, input_h = 0, result_w = 0, result_h = 0;
    int64_t pending_origin = 0, result_origin = 0;
    Clock::time_point result_time;
    Clock::time_point pending_time;
    Clock::time_point result_ready_time;
    FaceSwapCadence cadence;
    std::function<void()> result_callback;

    void fail(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        error = true;
        message = text;
        av_frame_free(&pending);
        av_frame_free(&last_input);
        av_frame_free(&result);
    }
    bool read(void* data, int length, int timeout_seconds = 10) {
        auto* bytes = static_cast<char*>(data);
        const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
        int offset = 0;
        while (offset < length && !cancelled && Clock::now() < deadline) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(socket, &ready);
            timeval timeout{0, 100000};
            int rc = select(static_cast<int>(socket) + 1, &ready, nullptr, nullptr, &timeout);
            if (rc < 0) return false;
            if (!rc) continue;
            int n = recv(socket, bytes + offset, length - offset, 0);
            if (n <= 0) return false;
            offset += n;
        }
        return offset == length;
    }
    void publish(AVFrame* frame, int width, int height, int64_t origin, Clock::time_point started, bool resident, uint64_t generation) {
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const bool was_preparing=preparing;
            preparing=false;
            if (generation!=video_generation) {
                if (was_preparing) message="Engine ready; waiting for video frames";
            } else if (frame && input_w == width && input_h == height &&
                Clock::now() - started < std::chrono::milliseconds(max_frame_age_ms)) {
                if (result) cadence.event(FaceSwapCadence::OutputReplaced);
                cadence.event(FaceSwapCadence::Completed);
                result_ready_time=Clock::now();
                av_frame_free(&result);
                result = frame; frame = nullptr;
                preparing=false;
                result_w = width; result_h = height;
                result_origin = origin; result_time = started;
                notify=result_callback;
                message = std::string(cpu ? "CPU - " : directml ? "DirectML [CPU pixels] - " : resident ? "CUDA [GPU resident, no image readback] - " : "CUDA [CPU pixels] - ") +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()) + " ms/frame";
                if (!resident && !gpu_fallback.empty()) message += " | " + gpu_fallback;
            } else if (frame) {
                cadence.event(input_w!=width || input_h!=height ? FaceSwapCadence::WrongSize : FaceSwapCadence::Expired);
                message = "Frame exceeded processing deadline; showing live video";
            }
        }
        av_frame_free(&frame);
        // Callback may acquire the renderer mutex or inspect has_result().
        // Never invoke it while holding the face-swap mutex.
        if (notify) notify();
    }
    void run(const std::string& source, const FaceSwapRuntime& runtime) {
        FaceSwapProcess worker;
        uint16_t port = 0;
        std::string startup_error;
        const bool ready = worker.start(runtime, cancelled, port, startup_error);
        {
            std::lock_guard<std::mutex> lock(mutex);
            log = worker.log_path();
            if (ready) message = runtime.cpu ? "Loading source photo on CPU..." : runtime.directml ? "Loading source photo with DirectML..." : "Loading source photo on NVIDIA GPU...";
        }
        if (!ready) { if (!cancelled) fail(startup_error); return; }
        if (!worker.log_path().empty()) {
            auto path=std::filesystem::u8path(worker.log_path()); path.replace_extension(".cadence.csv");
            cadence.open(path);
        }
        socket_t connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection == INVALID_SOCK) { fail("Cannot create worker connection"); return; }
        int no_delay = 1;
        setsockopt(connection, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        {
            std::lock_guard<std::mutex> lock(mutex);
            socket = connection;
        }
#if defined(_WIN32)
        DWORD timeout = 2000;
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{2, 0};
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (cancelled || connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            fail("Cannot connect to face-swap engine. See engine log."); return;
        }
        uint32_t greeting[] = {htonl(kMagic), htonl(static_cast<uint32_t>(source.size()))};
        if (net::send_all(connection, greeting, sizeof(greeting)) != sizeof(greeting) ||
            net::send_all(connection, source.data(), static_cast<int>(source.size())) != static_cast<int>(source.size())) {
            fail("Worker configuration failed"); return;
        }
        uint32_t reply[2];
        if (!read(reply, sizeof(reply), runtime.cpu || runtime.directml ? 90 : runtime.gpu_resident ? 1200 : 10) || ntohl(reply[1]) > 4096) {
            fail("Engine did not respond while loading the photo. See engine log."); return;
        }
        std::string detail(ntohl(reply[1]), '\0');
        if (!read(detail.data(), static_cast<int>(detail.size()))) { fail("Worker disconnected"); return; }
        if (ntohl(reply[0]) != kMagic) { fail(detail); return; }
        {
            std::lock_guard<std::mutex> lock(mutex);
            message = detail;
            if (detail.find("GPU_MODELS_READY")!=std::string::npos) {
                preparing=false;
                message="GPU models ready; waiting for screen mirroring";
            } else if (runtime.cpu || runtime.directml || !runtime.gpu_resident) preparing=false;
        }
        bool gpu_allowed = !runtime.cpu && !runtime.directml && runtime.gpu_resident && detail.find("GPU_TEXTURES") != std::string::npos;
        {std::lock_guard<std::mutex> lock(mutex);live_mask_allowed=gpu_allowed;}
        gpu_fallback = !runtime.cpu && !runtime.directml && runtime.gpu_resident && !gpu_allowed ? "Optional GPU bridge or CuPy unavailable" : "";
        bool gpu_initialized = false;
        auto last_gpu_report = Clock::time_point{};
        auto gpu = std::make_unique<FaceSwapGpu>();
        SwsContext* to_bgr = nullptr;
        uint64_t applied_mask_revision=0;
        while (!cancelled) {
            AVFrame* frame = nullptr;
            int64_t origin = 0;
            Clock::time_point started;
            uint64_t generation=0;
            uint64_t next_mask_revision=0;
            std::array<uint32_t,14> next_mask{};
            const auto idle_started=Clock::now();
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return cancelled || pending || mask_revision!=applied_mask_revision; });
                if (cancelled) break;
                next_mask_revision=mask_revision;
                if(next_mask_revision!=applied_mask_revision) next_mask=mask_settings;
                else {
                    frame = pending;
                    pending = nullptr;
                    origin = pending_origin;
                    started = pending_time;
                    generation=video_generation;
                }
            }
            if(next_mask_revision!=applied_mask_revision) {
                if(!gpu_allowed){applied_mask_revision=next_mask_revision;continue;}
                uint32_t request[18]={htonl(kMaskConfig),0,0,htonl(56)};
                for(size_t i=0;i<next_mask.size();++i)request[i+4]=htonl(next_mask[i]);
                uint32_t mask_reply[4]{};
                const bool ok=net::send_all(connection,request,sizeof(request))==sizeof(request) &&
                    read(mask_reply,sizeof(mask_reply)) && ntohl(mask_reply[0])==kMaskConfig && !mask_reply[1] && !mask_reply[2] && !mask_reply[3];
                if(!ok){if(!cancelled)fail("Live mask update failed. See engine log.");break;}
                applied_mask_revision=next_mask_revision;
                continue;
            }
            cadence.event(FaceSwapCadence::Started);
            cadence.timing(FaceSwapCadence::Idle,std::chrono::duration<double,std::milli>(Clock::now()-idle_started).count());
            cadence.timing(FaceSwapCadence::Queue,std::chrono::duration<double,std::milli>(Clock::now()-started).count());
            const int original_w = frame->width, original_h = frame->height;
            if (gpu_allowed && frame->format == AV_PIX_FMT_D3D11) {
                const auto prepare_started = Clock::now();
                GpuTextureTicket ticket;
                if (gpu->prepare(frame, runtime.quality == 0 && runtime.swapper_model == 0 ? 1280 : 1920, ticket)) {
                    const auto prepared = Clock::now();
                    if (!gpu_initialized) {
                        std::lock_guard<std::mutex> lock(mutex);
                        message = (runtime.tensorrt && runtime.quality > 0) || (runtime.swapper_model > 0 && runtime.swapper_tensorrt)
                            ? "Preparing GPU models / TensorRT cache; first build may take several minutes..."
                            : "Preparing GPU-resident models and kernels...";
                    }
                    av_frame_free(&frame);
                    uint32_t header[] = {htonl(kGpuMagic), htonl(ticket.width), htonl(ticket.height), htonl(24)};
                    uint32_t request[10];
                    std::copy(std::begin(header), std::end(header), request);
                    const uint64_t values[] = {ticket.input, ticket.output, ticket.adapter};
                    for (int index = 0; index < 3; ++index) {
                        request[4 + index * 2] = htonl(static_cast<uint32_t>(values[index] >> 32));
                        request[5 + index * 2] = htonl(static_cast<uint32_t>(values[index]));
                    }
                    bool sent = net::send_all(connection, request, sizeof(request)) == sizeof(request);
                    uint32_t response[4]{};
                    if (!sent || !read(response, sizeof(response), gpu_initialized ? 10 :
                            ((runtime.tensorrt && runtime.quality > 0) || (runtime.swapper_model > 0 && runtime.swapper_tensorrt) ? 1200 : 90))) {
                        if (!cancelled) fail("GPU engine disconnected. See engine log.");
                        break;
                    }
                    if (ntohl(response[0]) == kGpuError && ntohl(response[3]) <= 4096) {
                        std::string reason(ntohl(response[3]), '\0');
                        if (!read(reason.data(), static_cast<int>(reason.size()))) { fail("GPU fallback response interrupted"); break; }
                        gpu_allowed = false;
                        gpu_fallback = reason;
                        gpu.reset();
                        { std::lock_guard<std::mutex> lock(mutex); message = "GPU path unavailable; using CPU pixels: " + reason; }
                        continue;
                    }
                    if (ntohl(response[0]) != kGpuMagic || response[1] != header[1] || response[2] != header[2] || response[3]) {
                        fail("Invalid GPU engine response"); break;
                    }
                    gpu_initialized = true;
                    const auto replied = Clock::now();
                    AVFrame* output = gpu->complete();
                    if (!output) { fail("GPU output texture failed: " + gpu->error()); break; }
                    const auto completed = Clock::now();
                    cadence.timing(FaceSwapCadence::Prepare,std::chrono::duration<double,std::milli>(prepared-prepare_started).count());
                    cadence.timing(FaceSwapCadence::RoundTrip,std::chrono::duration<double,std::milli>(replied-prepared).count());
                    cadence.timing(FaceSwapCadence::Copy,std::chrono::duration<double,std::milli>(completed-replied).count());
                    if (completed - last_gpu_report >= std::chrono::seconds(2) ||
                        completed - prepare_started >= std::chrono::milliseconds(250)) {
                        const auto ms = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
                        LOG_INFO << "Face swap GPU wall ms: queue=" << ms(prepare_started - started)
                                 << " prepare=" << ms(prepared - prepare_started)
                                 << " engine_round_trip=" << ms(replied - prepared)
                                 << " output_copy=" << ms(completed - replied);
                        last_gpu_report = completed;
                    }
                    publish(output, original_w, original_h, origin, started, true, generation);
                    cadence.flush();
                    continue;
                }
                gpu_allowed = false;
                gpu_fallback = gpu->error();
                { std::lock_guard<std::mutex> lock(mutex); message = "GPU texture path unavailable: " + gpu_fallback; }
                gpu.reset();
            }
            if (gpu_allowed && frame->format != AV_PIX_FMT_D3D11) gpu_fallback = "Enable NVIDIA GPU decoding for GPU-resident video";
            AVFrame* downloaded = nullptr;
            AVFrame* pixels = frame;
            bool ok = true;
            if (frame->hw_frames_ctx) {
                downloaded = av_frame_alloc();
                ok = downloaded && av_hwframe_transfer_data(downloaded, frame, 0) >= 0;
                pixels = downloaded;
            }
            const int max_side = runtime.quality == 0 && runtime.swapper_model == 0 ? 1280 : 1920;
            const double scale = std::min(1.0, double(max_side) / std::max(original_w, original_h));
            const int w = std::max(2, static_cast<int>(original_w * scale) & ~1);
            const int h = std::max(2, static_cast<int>(original_h * scale) & ~1);
            const int payload_size = w * h * 3;
            // libswscale SIMD implementations may access padding at plane ends.
            std::vector<uint8_t> bgr(static_cast<size_t>(payload_size) + 64, 0);
            if (ok) {
                to_bgr = sws_getCachedContext(to_bgr, pixels->width, pixels->height,
                    static_cast<AVPixelFormat>(pixels->format), w, h, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                uint8_t* dst[4] = {bgr.data(), nullptr, nullptr, nullptr};
                int stride[4] = {w * 3, 0, 0, 0};
                // Match the D3D11 display path's matrix/range selection. A
                // hardware download does not necessarily copy AVFrame metadata.
                const int matrix = frame->colorspace == AVCOL_SPC_BT470BG ||
                    frame->colorspace == AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 : SWS_CS_ITU709;
                const int* coefficients = sws_getCoefficients(matrix);
                const int full_range = frame->color_range == AVCOL_RANGE_JPEG ||
                    frame->format == AV_PIX_FMT_YUVJ420P;
                if (to_bgr) sws_setColorspaceDetails(to_bgr, coefficients, full_range,
                    coefficients, 1, 0, 1 << 16, 1 << 16);
                ok = to_bgr && sws_scale(to_bgr, pixels->data, pixels->linesize,
                    0, pixels->height, dst, stride) == h;
            }
            av_frame_free(&downloaded);
            av_frame_free(&frame); // release decoder pool before waiting on inference
            uint32_t header[] = {htonl(kMagic), htonl(w), htonl(h), htonl(payload_size)};
            ok = ok && net::send_all(connection, header, sizeof(header)) == sizeof(header)
                && net::send_all(connection, bgr.data(), payload_size) == payload_size;
            uint32_t response[4]{};
            ok = ok && read(response, sizeof(response), runtime.cpu || runtime.directml ? 60 : 10) && !std::memcmp(header, response, sizeof(header))
                && read(bgr.data(), payload_size);
            if (!ok) { if (!cancelled) fail("Face-swap engine failed. See engine log."); break; }
            AVFrame* output = av_frame_alloc();
            if (output) {
                output->format = AV_PIX_FMT_BGR24;
                output->width = w;
                output->height = h;
                ok = av_frame_get_buffer(output, 32) >= 0;
                // Preserve restored RGB detail through presentation instead of
                // subsampling chroma again into an intermediate YUV420 frame.
                if (ok) for (int row = 0; row < h; ++row)
                    std::memcpy(output->data[0] + row * output->linesize[0],
                                bgr.data() + row * w * 3, static_cast<size_t>(w) * 3);
            } else ok = false;
            if (ok) { publish(output, original_w, original_h, origin, started, false, generation); cadence.flush(); }
            else av_frame_free(&output);
            if (!ok) { fail("Face-swap pixel conversion failed"); break; }
        }
        sws_freeContext(to_bgr);
    }
};

FaceSwap::FaceSwap() : impl_(std::make_unique<Impl>()) {}
FaceSwap::~FaceSwap() { stop(); }
void FaceSwap::start(const std::string& source, FaceSwapRuntime runtime) {
    stop();
    if (source.empty() || source.size() > 32768) { impl_->fail("Choose a source photo first"); return; }
    impl_->cancelled = false;
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->error = false; impl_->cpu = runtime.cpu; impl_->directml = runtime.directml; impl_->max_frame_age_ms = runtime.cpu ? 60000 : runtime.directml ? 5000 : 500; impl_->log.clear(); impl_->preparing=true; impl_->preparation_started=Clock::now(); impl_->progress_checked={}; impl_->preparation_stage.clear(); impl_->message = runtime.cpu ? "Starting CPU engine and loading models..." : runtime.directml ? "Starting DirectML engine and loading models..." : "Starting NVIDIA engine and loading models..."; }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->mask_settings={static_cast<uint32_t>(runtime.mask_shape),static_cast<uint32_t>(runtime.mask_blur),
            static_cast<uint32_t>(runtime.mask_preview),static_cast<uint32_t>(runtime.mask_padding[0]),
            static_cast<uint32_t>(runtime.mask_padding[1]),static_cast<uint32_t>(runtime.mask_padding[2]),
            static_cast<uint32_t>(runtime.mask_padding[3]),static_cast<uint32_t>(runtime.face_scale),static_cast<uint32_t>(runtime.face_size),static_cast<uint32_t>(runtime.alignment),static_cast<uint32_t>(runtime.mask_expansion),static_cast<uint32_t>(runtime.shape_transfer),static_cast<uint32_t>(runtime.shape_reset),static_cast<uint32_t>(runtime.animation_strength)};
        impl_->mask_revision=0;
        impl_->live_mask_allowed=!runtime.cpu && !runtime.directml && runtime.gpu_resident;
    }
    impl_->thread = std::thread([this, source, runtime] {
        try { impl_->run(source, runtime); }
        catch (const std::exception& error) { if (!impl_->cancelled) impl_->fail(error.what()); }
    });
}
void FaceSwap::stop() {
    impl_->cancelled = true;
    impl_->cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->socket != INVALID_SOCK) {
#if defined(_WIN32)
            shutdown(impl_->socket, SD_BOTH);
#else
            shutdown(impl_->socket, SHUT_RDWR);
#endif
        }
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->cadence.close();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    net::close_socket(impl_->socket); impl_->socket = INVALID_SOCK;
    av_frame_free(&impl_->pending); av_frame_free(&impl_->last_input); av_frame_free(&impl_->result);
    impl_->message = "Disabled"; impl_->error = false; impl_->preparing=false;
}
void FaceSwap::detach_video() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->video_generation;
    av_frame_free(&impl_->pending);
    av_frame_free(&impl_->last_input);
    av_frame_free(&impl_->result);
    impl_->input_w=impl_->input_h=impl_->result_w=impl_->result_h=0;
    if (!impl_->preparing && !impl_->error && !impl_->cancelled)
        impl_->message="Engine ready; waiting for video frames";
}
void FaceSwap::set_mask(int shape,int blur,int preview,const std::array<int,4>& padding,int face_scale,int face_size,int alignment,int mask_expansion,int shape_transfer,int shape_reset,int animation_strength) {
    if(animation_strength<0 || animation_strength>100 || shape_transfer<0 || shape_transfer>100 || shape_reset<0 || mask_expansion<0 || mask_expansion>12 || alignment<0 || alignment>1 || face_size<80 || face_size>120 || face_scale<80 || face_scale>120 || shape<0 || shape>1 || blur<0 || blur>50 || preview<0 || preview>2 ||
       std::any_of(padding.begin(),padding.end(),[](int v){return v<0 || v>45;})) return;
    const std::array<uint32_t,14> settings={static_cast<uint32_t>(shape),static_cast<uint32_t>(blur),
        static_cast<uint32_t>(preview),static_cast<uint32_t>(padding[0]),static_cast<uint32_t>(padding[1]),
        static_cast<uint32_t>(padding[2]),static_cast<uint32_t>(padding[3]),static_cast<uint32_t>(face_scale),static_cast<uint32_t>(face_size),static_cast<uint32_t>(alignment),static_cast<uint32_t>(mask_expansion),static_cast<uint32_t>(shape_transfer),static_cast<uint32_t>(shape_reset),static_cast<uint32_t>(animation_strength)};
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->cancelled || impl_->error || !impl_->live_mask_allowed || settings==impl_->mask_settings)return;
        impl_->mask_settings=settings;++impl_->mask_revision;
        // A paused AirPlay sender may produce no new frames. Reprocess the last
        // original input after the configuration ACK, using a fresh work deadline.
        // Keep any newer queued video input; rapid slider edits share one slot.
        if(!impl_->pending && impl_->last_input) {
            impl_->pending=av_frame_clone(impl_->last_input);
            impl_->pending_origin=impl_->last_input_origin;
            impl_->pending_time=Clock::now();
        }
    }
    impl_->cv.notify_one();
}
void FaceSwap::submit(const AVFrame* frame, int64_t origin_ns) {
    if (!frame || frame->width < 2 || frame->height < 2 || frame->width > 8192 || frame->height > 8192) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cancelled || impl_->error) return;
    AVFrame* copy = av_frame_clone(frame);
    if (!copy) return;
    if ((impl_->input_w != frame->width || impl_->input_h != frame->height) && impl_->result) {
        impl_->cadence.event(FaceSwapCadence::WrongSize); av_frame_free(&impl_->result);
    }
    if(impl_->live_mask_allowed) {
        av_frame_free(&impl_->last_input);
        impl_->last_input=av_frame_clone(frame);
        impl_->last_input_origin=origin_ns;
    }
    impl_->input_w = frame->width; impl_->input_h = frame->height;
    impl_->cadence.event(FaceSwapCadence::Submitted);
    if (impl_->pending) impl_->cadence.event(FaceSwapCadence::PendingReplaced);
    av_frame_free(&impl_->pending); impl_->pending = copy;
    impl_->pending_origin = origin_ns;
    impl_->pending_time = Clock::now();
    impl_->cv.notify_one();
}
AVFrame* FaceSwap::take(int width, int height, int64_t& origin_ns) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->result) return nullptr;
    if (width != impl_->result_w || height != impl_->result_h ||
        Clock::now() - impl_->result_time > std::chrono::milliseconds(impl_->max_frame_age_ms)) {
        impl_->cadence.event(width!=impl_->result_w || height!=impl_->result_h ? FaceSwapCadence::WrongSize : FaceSwapCadence::Expired);
        av_frame_free(&impl_->result); return nullptr;
    }
    impl_->cadence.event(FaceSwapCadence::Taken);
    impl_->cadence.timing(FaceSwapCadence::ResultHold,std::chrono::duration<double,std::milli>(Clock::now()-impl_->result_ready_time).count());
    AVFrame* result = impl_->result; impl_->result = nullptr;
    origin_ns = impl_->result_origin;
    return result;
}
std::string FaceSwap::status() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->message; }
FaceSwapPreparation FaceSwap::preparation() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FaceSwapPreparation state;
    state.active=impl_->preparing && !impl_->cancelled && !impl_->error;
    if (!state.active) return state;
    const auto now=Clock::now();
    state.elapsed_seconds=std::chrono::duration<double>(now-impl_->preparation_started).count();
    // Read a tiny atomic sidecar at most four times a second, only while loading.
    if (!impl_->log.empty() && now-impl_->progress_checked>std::chrono::milliseconds(250)) {
        impl_->progress_checked=now;
        auto path=std::filesystem::u8path(impl_->log); path.replace_extension(".progress");
        std::ifstream input(path);
        std::string stage;
        if (std::getline(input,stage) && !stage.empty() && stage.size()<=512)
            impl_->preparation_stage=stage;
    }
    state.stage=impl_->preparation_stage.empty() ? impl_->message : impl_->preparation_stage;
    return state;
}
bool FaceSwap::failed() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }
std::string FaceSwap::log_path() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->log; }
bool FaceSwap::has_result() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->result!=nullptr; }
void FaceSwap::set_result_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->result_callback=std::move(callback);
}
}

void ap::video::FaceSwap::record_cadence(uint64_t decoded,uint64_t replaced,bool input,bool presented,bool held,
                                       double wait_ms,double present_ms,double origin_age_ms) {
    impl_->cadence.renderer(decoded,replaced,input,presented,held,wait_ms,present_ms,origin_age_ms);
}
