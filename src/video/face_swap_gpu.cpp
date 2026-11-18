#include "video/face_swap_gpu.h"
#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#include <thread>
extern "C" {
#include <libavutil/frame.h>
}
#if defined(_WIN32)
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace ap::video {
struct FaceSwapGpu::Impl {
    std::string error;
#if defined(_WIN32)
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11VideoDevice> video;
    Ptr<ID3D11VideoContext> video_context;
    Ptr<ID3D11VideoProcessorEnumerator> enumerator;
    Ptr<ID3D11VideoProcessor> processor;
    Ptr<ID3D11VideoProcessorOutputView> output_view;
    Ptr<ID3D11Texture2D> input, output, source;
    Ptr<ID3D11Query> completion;
    std::vector<Ptr<ID3D11VideoProcessorInputView>> source_views;
    struct Snapshot { Ptr<ID3D11Texture2D> texture; };
    std::vector<std::shared_ptr<Snapshot>> pool;
    GpuTextureTicket ticket;
    int source_width = 0, source_height = 0;
    bool initialized = false;
    bool check(HRESULT status, const char* operation) {
        if (status == S_OK) return true;
        char text[160];
        std::snprintf(text, sizeof(text), "%s (HRESULT 0x%08lx)", operation, static_cast<unsigned long>(status));
        error = text;
        return false;
    }
    bool texture(Ptr<ID3D11Texture2D>& result, uint64_t* handle = nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = ticket.width; desc.Height = ticket.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (handle) desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (!check(device->CreateTexture2D(&desc, nullptr, &result), "Create GPU face texture")) {
            error += " size=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + " flags=" + std::to_string(desc.MiscFlags);
            return false;
        }
        if (handle) {
            Ptr<IDXGIResource> shared;
            HANDLE value = nullptr;
            if (!check(result.As(&shared), "Get shared texture") ||
                !check(shared->GetSharedHandle(&value), "Get GPU texture handle")) return false;
            *handle = reinterpret_cast<uint64_t>(value);
        }
        return true;
    }
    bool finish() {
        context->End(completion.Get());
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        HRESULT status = S_FALSE;
        while ((status = context->GetData(completion.Get(), nullptr, 0, 0)) == S_FALSE &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return check(status, "Wait for GPU texture handoff");
    }
#endif
};
FaceSwapGpu::FaceSwapGpu() : impl_(std::make_unique<Impl>()) {}
FaceSwapGpu::~FaceSwapGpu() = default;
std::string FaceSwapGpu::error() const { return impl_->error; }

bool FaceSwapGpu::prepare(const AVFrame* frame, int max_side, GpuTextureTicket& ticket) {
#if defined(_WIN32)
    auto& p = *impl_;
    if (!frame || frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) return false;
    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    Impl::Ptr<ID3D11Device> device;
    source->GetDevice(&device);
    const double scale = std::min(1.0, double(max_side) / std::max(frame->width, frame->height));
    const int width = std::max(2, static_cast<int>(frame->width * scale) & ~1);
    const int height = std::max(2, static_cast<int>(frame->height * scale) & ~1);
    if (!p.initialized || p.device.Get() != device.Get() || p.source_width != frame->width || p.source_height != frame->height ||
        p.ticket.width != width || p.ticket.height != height) {
        p.processor.Reset(); p.enumerator.Reset(); p.output_view.Reset();
        p.initialized = false;
        p.input.Reset(); p.output.Reset(); p.source.Reset(); p.source_views.clear(); p.pool.clear();
        p.device = device;
        device->GetImmediateContext(&p.context);
        D3D11_QUERY_DESC event_desc{D3D11_QUERY_EVENT, 0};
        if (!p.check(device->CreateQuery(&event_desc, &p.completion), "Create GPU completion event")) return false;
        Impl::Ptr<ID3D11Multithread> protected_context;
        if (!p.check(p.context.As(&protected_context), "Get multithreaded GPU context")) return false;
        protected_context->SetMultithreadProtected(TRUE);
        Impl::Ptr<IDXGIDevice> dxgi;
        Impl::Ptr<IDXGIAdapter> adapter;
        if (!p.check(device.As(&dxgi), "Get GPU adapter") || !p.check(dxgi->GetAdapter(&adapter), "Find GPU adapter")) return false;
        DXGI_ADAPTER_DESC adapter_desc{};
        adapter->GetDesc(&adapter_desc);
        if (adapter_desc.VendorId != 0x10de) { p.error = "Display decoder is not on the NVIDIA adapter"; return false; }
        p.ticket = {};
        p.ticket.width = width; p.ticket.height = height;
        p.ticket.adapter = static_cast<uint32_t>(adapter_desc.AdapterLuid.LowPart) |
            (static_cast<uint64_t>(static_cast<uint32_t>(adapter_desc.AdapterLuid.HighPart)) << 32);
        p.source_width = frame->width; p.source_height = frame->height;
        if (!p.check(device.As(&p.video), "Get video device") ||
            !p.check(p.context.As(&p.video_context), "Get video conversion context")) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
        description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        description.InputWidth = frame->width; description.InputHeight = frame->height;
        description.OutputWidth = width; description.OutputHeight = height;
        description.InputFrameRate = description.OutputFrameRate = {60, 1};
        description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (!p.check(p.video->CreateVideoProcessorEnumerator(&description, &p.enumerator), "Create GPU color enumerator") ||
            !p.check(p.video->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor), "Create GPU color processor") ||
            !p.texture(p.input, &p.ticket.input) || !p.texture(p.output, &p.ticket.output)) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (!p.check(p.video->CreateVideoProcessorOutputView(p.input.Get(), p.enumerator.Get(), &view, &p.output_view),
                     "Create shared video output view")) return false;
        p.initialized = true;
    }
    if (!p.processor) return false;
    if (p.source.Get() != source) {
        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        p.source = source;
        p.source_views.clear(); p.source_views.resize(description.ArraySize);
    }
    const auto slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
    if (slice >= p.source_views.size()) return false;
    if (!p.source_views[slice]) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
        description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        description.Texture2D.ArraySlice = slice;
        if (!p.check(p.video->CreateVideoProcessorInputView(source, p.enumerator.Get(), &description, &p.source_views[slice]),
                     "Create decoder texture view")) return false;
    }
    RECT input{0, 0, frame->width, frame->height}, output{0, 0, width, height};
    p.video_context->VideoProcessorSetStreamAutoProcessingMode(p.processor.Get(), 0, FALSE);
    p.video_context->VideoProcessorSetStreamFrameFormat(p.processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    p.video_context->VideoProcessorSetStreamSourceRect(p.processor.Get(), 0, TRUE, &input);
    p.video_context->VideoProcessorSetStreamDestRect(p.processor.Get(), 0, TRUE, &output);
    p.video_context->VideoProcessorSetOutputTargetRect(p.processor.Get(), TRUE, &output);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
    in.YCbCr_Matrix = frame->colorspace != AVCOL_SPC_BT470BG && frame->colorspace != AVCOL_SPC_SMPTE170M;
    in.Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
    p.video_context->VideoProcessorSetStreamColorSpace(p.processor.Get(), 0, &in);
    p.video_context->VideoProcessorSetOutputColorSpace(p.processor.Get(), &out);
    p.video_context->VideoProcessorSetOutputAlphaFillMode(p.processor.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE; stream.pInputSurface = p.source_views[slice].Get();
    const HRESULT status = p.video_context->VideoProcessorBlt(p.processor.Get(), p.output_view.Get(), 0, 1, &stream);
    if (!p.check(status, "Convert decoder surface on GPU")) return false;
    // The socket protocol has exactly one frame in flight. GPU completion at
    // each handoff prevents cross-device reuse without keyed-mutex support.
    if (!p.finish()) return false;
    ticket = p.ticket;
    return true;
#else
    (void)frame; (void)max_side; (void)ticket;
    return false;
#endif
}

AVFrame* FaceSwapGpu::complete() {
#if defined(_WIN32)
    auto& p = *impl_;
    if (!p.output) return nullptr;
    std::shared_ptr<Impl::Snapshot> snapshot;
    for (const auto& candidate : p.pool) if (candidate.use_count() == 1) { snapshot = candidate; break; }
    if (!snapshot && p.pool.size() < 4) {
        snapshot = std::make_shared<Impl::Snapshot>();
        if (p.texture(snapshot->texture)) p.pool.push_back(snapshot);
        else snapshot.reset();
    }
    AVFrame* frame = snapshot ? av_frame_alloc() : nullptr;
    if (frame) {
        p.context->CopyResource(snapshot->texture.Get(), p.output.Get());
        if (!p.finish()) { av_frame_free(&frame); return nullptr; }
        auto* owner = new std::shared_ptr<Impl::Snapshot>(snapshot);
        frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(snapshot->texture.Get()), 1,
            [](void* opaque, uint8_t*) { delete static_cast<std::shared_ptr<Impl::Snapshot>*>(opaque); }, owner, 0);
        if (!frame->buf[0]) { delete owner; av_frame_free(&frame); }
        else {
            frame->format = AV_PIX_FMT_D3D11;
            frame->width = p.ticket.width; frame->height = p.ticket.height;
            frame->color_range = AVCOL_RANGE_JPEG;
            frame->colorspace = AVCOL_SPC_RGB;
            frame->data[0] = reinterpret_cast<uint8_t*>(snapshot->texture.Get());
        }
    }
    return frame;
#else
    return nullptr;
#endif
}
}
