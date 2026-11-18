// Optional worker-side bridge. CUDA Driver API keeps this DLL independent of
// the CUDA runtime major version used by CuPy and ONNX Runtime.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cuda.h>
#include <cudaD3D11.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
thread_local char last_error[512]{};
bool checked(CUresult status, const char* operation) {
    if (status == CUDA_SUCCESS) return true;
    const char* message = nullptr;
    cuGetErrorString(status, &message);
    std::snprintf(last_error, sizeof(last_error), "%s: %s", operation, message ? message : "CUDA driver error");
    return false;
}
bool checked_hr(HRESULT status, const char* operation) {
    if (status == S_OK) return true;
    std::snprintf(last_error, sizeof(last_error), "%s: HRESULT 0x%08lx", operation, static_cast<unsigned long>(status));
    return false;
}
struct Bridge {
    CUdevice ordinal = 0;
    CUcontext cuda = nullptr;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> input, output;
    CUgraphicsResource cuda_input = nullptr, cuda_output = nullptr;
    unsigned width = 0, height = 0;
    ~Bridge() {
        if (cuda) {
            cuCtxSetCurrent(cuda);
            if (cuda_input) cuGraphicsUnregisterResource(cuda_input);
            if (cuda_output) cuGraphicsUnregisterResource(cuda_output);
            cuDevicePrimaryCtxRelease(ordinal);
        }
    }
    bool transfer(bool read, CUdeviceptr pointer, CUstream stream) {
        if (!pointer || !checked(cuCtxSetCurrent(cuda), "set CUDA context")) return false;
        CUgraphicsResource resource = read ? cuda_input : cuda_output;
        bool mapped = false;
        bool ok = checked(cuGraphicsMapResources(1, &resource, stream), "map D3D11 texture");
        mapped = ok;
        CUarray array = nullptr;
        if (ok) ok = checked(cuGraphicsSubResourceGetMappedArray(&array, resource, 0, 0), "get CUDA array");
        if (ok) {
            CUDA_MEMCPY2D copy{};
            copy.WidthInBytes = static_cast<size_t>(width) * 4;
            copy.Height = height;
            if (read) {
                copy.srcMemoryType = CU_MEMORYTYPE_ARRAY; copy.srcArray = array;
                copy.dstMemoryType = CU_MEMORYTYPE_DEVICE; copy.dstDevice = pointer;
                copy.dstPitch = copy.WidthInBytes;
            } else {
                copy.srcMemoryType = CU_MEMORYTYPE_DEVICE; copy.srcDevice = pointer;
                copy.srcPitch = copy.WidthInBytes;
                copy.dstMemoryType = CU_MEMORYTYPE_ARRAY; copy.dstArray = array;
            }
            ok = checked(cuMemcpy2DAsync(&copy, stream), "GPU texture copy");
        }
        if (mapped) ok = checked(cuGraphicsUnmapResources(1, &resource, stream), "unmap D3D11 texture") && ok;
        // Only GPU completion is awaited; no image data is copied to the CPU.
        ok = checked(cuStreamSynchronize(stream), "finish GPU texture copy") && ok;
        return ok;
    }
};
}

extern "C" {
__declspec(dllexport) const char* ap_gpu_error() { return last_error; }
__declspec(dllexport) void* ap_gpu_open(uint64_t input, uint64_t output, uint64_t luid,
                                      unsigned width, unsigned height, int* ordinal) {
    if (!input || !output || !width || !height || width > 8192 || height > 8192 || !ordinal) return nullptr;
    auto bridge = std::make_unique<Bridge>();
    bridge->width = width; bridge->height = height;
    if (!checked(cuInit(0), "initialize CUDA driver")) return nullptr;
    Ptr<IDXGIFactory> factory;
    if (!checked_hr(CreateDXGIFactory(IID_PPV_ARGS(&factory)), "create DXGI factory")) return nullptr;
    Ptr<IDXGIAdapter> selected;
    for (UINT index = 0;; ++index) {
        Ptr<IDXGIAdapter> candidate;
        if (factory->EnumAdapters(index, &candidate) != S_OK) break;
        DXGI_ADAPTER_DESC description{};
        candidate->GetDesc(&description);
        const uint64_t candidate_luid = static_cast<uint32_t>(description.AdapterLuid.LowPart) |
            (static_cast<uint64_t>(static_cast<uint32_t>(description.AdapterLuid.HighPart)) << 32);
        if (candidate_luid == luid) { selected = candidate; break; }
    }
    if (!selected) { std::snprintf(last_error, sizeof(last_error), "D3D11 adapter is unavailable"); return nullptr; }
    if (!checked(cuD3D11GetDevice(&bridge->ordinal, selected.Get()), "match NVIDIA adapter")) return nullptr;
    if (!checked(cuDevicePrimaryCtxRetain(&bridge->cuda, bridge->ordinal), "retain CUDA primary context")) return nullptr;
    if (!checked(cuCtxSetCurrent(bridge->cuda), "set CUDA context")) return nullptr;
    HRESULT status = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &bridge->device, nullptr, &bridge->context);
    if (!checked_hr(status, "create worker D3D11 device")) return nullptr;
    status = bridge->device->OpenSharedResource(reinterpret_cast<HANDLE>(input), IID_PPV_ARGS(&bridge->input));
    if (status == S_OK) status = bridge->device->OpenSharedResource(reinterpret_cast<HANDLE>(output), IID_PPV_ARGS(&bridge->output));
    if (!checked_hr(status, "open shared textures")) return nullptr;
    for (auto* texture : {bridge->input.Get(), bridge->output.Get()}) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width != width || description.Height != height ||
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.ArraySize != 1) {
            std::snprintf(last_error, sizeof(last_error), "Shared texture has incompatible dimensions or format");
            return nullptr;
        }
    }
    if (!checked(cuGraphicsD3D11RegisterResource(&bridge->cuda_input, bridge->input.Get(), 0), "register input texture")) return nullptr;
    if (!checked(cuGraphicsD3D11RegisterResource(&bridge->cuda_output, bridge->output.Get(), 0), "register output texture")) return nullptr;
    *ordinal = bridge->ordinal;
    return bridge.release();
}
__declspec(dllexport) int ap_gpu_read(void* object, uint64_t destination, uint64_t stream) {
    return object && static_cast<Bridge*>(object)->transfer(true, destination, reinterpret_cast<CUstream>(stream));
}
__declspec(dllexport) int ap_gpu_write(void* object, uint64_t source, uint64_t stream) {
    return object && static_cast<Bridge*>(object)->transfer(false, source, reinterpret_cast<CUstream>(stream));
}
__declspec(dllexport) void ap_gpu_close(void* object) { delete static_cast<Bridge*>(object); }
}
