#include "video/face_swap_process.h"
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>
#endif

namespace ap::video {
namespace fs = std::filesystem;
namespace {
bool is_file(const fs::path& path) { std::error_code ec; return fs::is_regular_file(path, ec); }
fs::path executable_directory() {
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length && length < path.size()) { path.resize(length); return fs::path(path).parent_path(); }
#endif
    return {};
}
#if defined(_WIN32)
std::wstring quoted(const fs::path& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value.wstring()) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'"') result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#endif
}

FaceSwapRuntime FaceSwapRuntime::discover() {
    FaceSwapRuntime result;
    auto directory = executable_directory();
    if (directory.empty()) return result;
    result.worker = (directory / "face_swap" / "engine" / "face_swap_worker.py").u8string();
    result.gpu_bridge = (directory / "face_swap" / "engine" / "airplay-cuda-bridge.dll").u8string();
    result.logs = (directory / "face_swap" / "logs").u8string();
    result.models = (directory / "face_swap" / "swappers").u8string();
    result.python = (directory / "face_swap" / "python" / "python.exe").u8string();
    result.directml_python = (directory / "face_swap" / "python-directml" / "python.exe").u8string();
    result.cpu_python = (directory / "face_swap" / "python-cpu" / "python.exe").u8string();
    // A local installation may point to already provisioned model weights.
    std::ifstream model_path(directory / "face_swap" / "models-path.txt");
    std::string configured;
    if (!is_file(fs::u8path(result.models) / "det_10g.onnx") && std::getline(model_path, configured) && !configured.empty()) {
        if (configured.back() == '\r') configured.pop_back();
        result.models = configured;
    }
    for (int level = 0; level < 5; ++level) {
        if (!is_file(fs::u8path(result.python)) && is_file(directory / ".venv-face-swap" / "Scripts" / "python.exe"))
            result.python = (directory / ".venv-face-swap" / "Scripts" / "python.exe").u8string();
        if (!is_file(fs::u8path(result.directml_python)) && is_file(directory / ".venv-face-swap-directml" / "Scripts" / "python.exe"))
            result.directml_python = (directory / ".venv-face-swap-directml" / "Scripts" / "python.exe").u8string();
        if (!is_file(fs::u8path(result.worker)) && is_file(directory / "tools" / "face_swap_worker.py"))
            result.worker = (directory / "tools" / "face_swap_worker.py").u8string();
        auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    if (!is_file(fs::u8path(result.cpu_python)) && is_file(fs::u8path(result.python))) result.cpu_python=result.python;
    return result;
}

std::vector<std::string> FaceSwapRuntime::directml_adapters() {
    std::vector<std::string> result;
#if defined(_WIN32)
    IDXGIFactory1* factory=nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter=nullptr;
        for (UINT index=0; ; ++index) {
            if (FAILED(factory->EnumAdapters1(index,&adapter)) || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                char name[512]{};
                WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
                result.emplace_back(std::to_string(index)+": "+name+(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " (software / CPU)" : ""));
            } else result.emplace_back(std::to_string(index)+": Unknown adapter");
            adapter->Release();
        }
        factory->Release();
    }
#endif
    return result;
}

struct FaceSwapProcess::Impl {
    fs::path log, ready;
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    ~Impl() {
        if (job) CloseHandle(job);
        if (process) { WaitForSingleObject(process, 1000); CloseHandle(process); }
        std::error_code ec;
        if (!ready.empty()) {
            fs::remove(ready, ec);
            auto temporary = ready; temporary += ".tmp";
            fs::remove(temporary, ec);
        }
    }
#endif
};
FaceSwapProcess::FaceSwapProcess() : impl_(std::make_unique<Impl>()) {}
FaceSwapProcess::~FaceSwapProcess() = default;
std::string FaceSwapProcess::log_path() const { return impl_->log.u8string(); }

bool FaceSwapProcess::start(const FaceSwapRuntime& runtime, const std::atomic<bool>& cancelled,
                            uint16_t& port, std::string& error) {
    if (!runtime.launch) { port = runtime.port; return port != 0; }
#if defined(_WIN32)
    const auto python = fs::u8path(runtime.directml ? runtime.directml_python : runtime.python), worker = fs::u8path(runtime.worker);
    const auto models = fs::u8path(runtime.models);
    const auto assets = worker.parent_path().filename()=="engine" ? worker.parent_path().parent_path() : worker.parent_path();
    if (runtime.detector<0 || runtime.detector>3) {error="Invalid face detector";return false;}
    const char* detector_kinds[]={"scrfd","yoloface","retinaface","yunet"};
    const char* detector_files[]={"det_10g","yoloface_8n","retinaface_10g","yunet_2023_mar"};
    const auto detector=runtime.detector==0 ? models/"det_10g.onnx" : assets/"swappers"/(std::string(detector_files[runtime.detector])+".onnx");
    const auto recognizer=models/"w600k_r50.onnx";
    auto swapper = models / "inswapper_128.onnx";
    if (!is_file(swapper)) swapper = models / "inswapper_128.fp16.onnx";
    const char* swapper_names[] = {"inswapper_128", "alphaface_256", "hyperswap_1a_256", "hyperswap_1b_256", "hyperswap_1c_256"};
    if (runtime.swapper_model < 0 || runtime.swapper_model > 4) { error = "Invalid swapper selection"; return false; }
    if (runtime.swapper_builder!=0 && runtime.swapper_builder!=3 && runtime.swapper_builder!=5) { error="Invalid TensorRT compilation level"; return false; }
    if (runtime.swapper_model > 0)
        swapper = assets / "swappers" / (std::string(swapper_names[runtime.swapper_model]) + ".onnx");
    if (!is_file(python)) { error = runtime.directml ? "DirectML Python runtime missing. Check Engine & diagnostics > DirectML Python executable." : "Face-swap Python runtime missing. Check Runtime settings."; return false; }
    if (!is_file(worker)) { error = "Face-swap worker missing from the application folder."; return false; }
    if (!runtime.animation && !is_file(swapper)) { error = "Selected swapper model missing: " + swapper.u8string(); return false; }
    if (!is_file(detector)) {
        error = "Face detector missing: " + detector.filename().u8string() + ". Download the selected detector in Model management.";
        return false;
    }
    if (!runtime.animation && !is_file(recognizer)) {
        error = "ArcFace missing: w600k_r50.onnx. Download ArcFace in Model management.";
        return false;
    }
    if (runtime.animation<0 || runtime.animation>1 || runtime.animation_strength<0 || runtime.animation_strength>100) {error="Invalid reference animation settings";return false;}
    if(runtime.animation && (runtime.cpu || runtime.directml || !runtime.gpu_resident)){error="LivePortrait requires NVIDIA CUDA and Keep video on GPU";return false;}
    if(runtime.animation && !is_file(fs::u8path(runtime.gpu_bridge))){error="LivePortrait requires the CUDA video bridge in face_swap/engine";return false;}
    if(runtime.animation) {
        const char* components[]={"live_portrait_feature_extractor","live_portrait_motion_extractor","live_portrait_generator","live_portrait_stitcher"};
        for(const auto* name:components)if(!is_file(assets/"swappers"/(std::string(name)+".onnx"))){error="LivePortrait models missing. Download the model pack in Faceswap Settings > Model & quality.";return false;}
    }
    if (runtime.shape_transfer<0 || runtime.shape_transfer>100 || runtime.shape_reset<0 || runtime.mask_expansion<0 || runtime.mask_expansion>12 || runtime.alignment<0 || runtime.alignment>1 || runtime.face_size<80 || runtime.face_size>120 || runtime.face_scale<80 || runtime.face_scale>120 || runtime.mask_shape<0 || runtime.mask_shape>1 || runtime.mask_blur<0 || runtime.mask_blur>50 || runtime.mask_preview<0 || runtime.mask_preview>2 ||
        std::any_of(runtime.mask_padding.begin(),runtime.mask_padding.end(),[](int v){return v<0 || v>45;})) {error="Invalid blending mask options";return false;}
    if (runtime.landmarker<0 || runtime.landmarker>1) {error="Invalid face landmarker";return false;}
    fs::path landmarker;
    if(runtime.landmarker==1 && !runtime.cpu && !runtime.directml && runtime.gpu_resident) {
        landmarker=assets/"swappers"/"hrffa.onnx";
        if(!is_file(landmarker)){error="HRFFA model missing. Download HRFFA - 68 points in Model management.";return false;}
    }
    if (runtime.parser<0 || runtime.parser>4) {error="Invalid BiSeNet region mode";return false;}
    fs::path parser;
    if (runtime.parser>0 && !runtime.cpu && !runtime.directml && runtime.gpu_resident) {
        parser=assets/"swappers"/"bisenet_resnet_18.onnx";
        if (!is_file(parser)) {error="BiSeNet model missing. Download it in Faceswap Settings > Processing device.";return false;}
    }
    fs::path occluder;
    if (runtime.occlusion && !runtime.cpu && !runtime.directml && runtime.gpu_resident) {
        occluder=assets/"swappers"/"xseg_1.onnx";
        if (!is_file(occluder)) {error="XSeg 1 model missing. Download it in Faceswap Settings > Processing device.";return false;}
    }
    std::error_code ec;
    auto logs = fs::u8path(runtime.logs);
    fs::path restorer;
    if (!runtime.animation && runtime.quality > 0 && runtime.swapper_model == 0) {
        restorer = models / (runtime.quality == 2 ? "GPEN-BFR-512.onnx" : "GPEN-BFR-256.onnx");
        if (!is_file(restorer)) {
            error = "Restoration model missing: " + restorer.filename().u8string() + ". Choose Fast quality or another models folder.";
            return false;
        }
    }
    fs::create_directories(logs, ec);
    if (ec) { error = "Cannot create face-swap log folder: " + ec.message(); return false; }
    const std::string name = "face-swap-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    impl_->log = logs / (name + ".log");
    impl_->ready = logs / (name + ".port");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(impl_->log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) { error = "Cannot open face-swap log file"; return false; }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, 0, nullptr);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool initialized = InitializeProcThreadAttributeList(list, 1, 0, &size) != FALSE;
    HANDLE inherited[] = {output, input};
    bool ok = input != INVALID_HANDLE_VALUE && initialized &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr);
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ok = ok && impl_->job && SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                                    &limits, sizeof(limits));
    std::wstring command = quoted(python) + L" -u " + quoted(worker) + L" --detector " + quoted(detector)
        + L" --detector-kind " + quoted(fs::u8path(detector_kinds[runtime.detector]))
        + L" --recognizer " + quoted(recognizer) + L" --swapper " + quoted(swapper)
        + L" --cache-root " + quoted(assets / "swappers")
        + L" --port 0 --ready-file " + quoted(impl_->ready) + L" --single-client";
    if (runtime.directml) command += L" --backend directml --device " + std::to_wstring(std::max(0,runtime.directml_device));
    else if (runtime.cpu) command += L" --backend cpu";
    if (!restorer.empty()) command += L" --restorer " + quoted(restorer) + L" --restore-strength "
        + std::to_wstring(std::max(0, std::min(100, runtime.restoration)) / 100.0);
    if (!runtime.cpu && !runtime.directml && runtime.gpu_resident && is_file(fs::u8path(runtime.gpu_bridge)))
        command += L" --gpu-bridge " + quoted(fs::u8path(runtime.gpu_bridge));
    if (!runtime.cpu && !runtime.directml && runtime.gpu_resident && runtime.tensorrt && runtime.quality > 0) command += L" --tensorrt";
    if (!runtime.animation && runtime.swapper_model > 0) {
        command += L" --swap-model " + quoted(fs::u8path(swapper_names[runtime.swapper_model]));
        if (!runtime.cpu && !runtime.directml && runtime.swapper_tensorrt && runtime.gpu_resident)
            command += L" --swapper-backend tensorrt_fp32";
    }
    if (!runtime.cpu && !runtime.directml && runtime.gpu_resident) {
        command+=L" --swapper-builder "+std::to_wstring(runtime.swapper_builder);
        command+=L" --alignment "+std::wstring(runtime.alignment==1 ? L"facefusion" : L"legacy");
        command+=L" --mask-expansion "+std::to_wstring(runtime.mask_expansion);
        command+=L" --shape-transfer "+std::to_wstring(runtime.shape_transfer)+L" --shape-reset "+std::to_wstring(runtime.shape_reset);
        if(runtime.animation)command+=L" --animation";
        command+=L" --animation-strength "+std::to_wstring(runtime.animation_strength);
        command+=L" --mask-shape "+std::wstring(runtime.mask_shape==1 ? L"box" : L"ellipse")+
            L" --face-size "+std::to_wstring(runtime.face_size)+L" --face-scale "+std::to_wstring(runtime.face_scale)+L" --mask-blur "+std::to_wstring(runtime.mask_blur)+L" --mask-preview "+std::to_wstring(runtime.mask_preview)+L" --mask-padding";
        for (int value:runtime.mask_padding) command+=L" "+std::to_wstring(value);
    }
    if (!landmarker.empty()) command+=L" --landmarker "+quoted(landmarker);
    if (!parser.empty()) command+=L" --face-parser "+quoted(parser)+L" --parser-mode "+std::to_wstring(runtime.parser);
    if (!occluder.empty()) command+=L" --occluder "+quoted(occluder);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.StartupInfo.hStdInput = input;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (ok && !cancelled) ok = CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        python.parent_path().c_str(), &startup.StartupInfo, &process) != FALSE;
    else ok = false;
    const DWORD launch_error = GetLastError();
    if (initialized) DeleteProcThreadAttributeList(list);
    CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!ok) { error = "Cannot launch face-swap engine (Windows " + std::to_string(launch_error) + ")"; return false; }
    impl_->process = process.hProcess;
    if (!AssignProcessToJobObject(impl_->job, process.hProcess) || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        error = "Cannot manage face-swap process lifetime"; return false;
    }
    CloseHandle(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!cancelled && std::chrono::steady_clock::now() < deadline) {
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            error = "Face-swap engine stopped during startup. See log: " + log_path(); return false;
        }
        std::ifstream ready(impl_->ready);
        unsigned number = 0;
        if (ready >> number && number > 0 && number <= 65535) {
            port = static_cast<uint16_t>(number);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = cancelled ? "Cancelled" : "Face-swap engine startup timed out. See log: " + log_path();
    return false;
#else
    (void)cancelled; (void)port;
    error = "Automatic CUDA engine launch is currently supported on Windows only";
    return false;
#endif
}
}

namespace ap::video {
struct FaceSwapDownload::Impl {
    mutable std::mutex mutex;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    FaceSwapDownloadStatus state;
    void run(const FaceSwapRuntime& runtime,int model) {
#if defined(_WIN32)
        if (model<1 || model>FaceSwapDownload::arcface_model) throw std::runtime_error("Invalid model download selection");
        const auto worker=fs::u8path(runtime.worker);
        const auto assets=worker.parent_path().filename()=="engine" ? worker.parent_path().parent_path() : worker.parent_path();
        const auto helper=worker.parent_path()/"download_face_swap_models.py";
        const auto python=fs::u8path(runtime.directml ? runtime.directml_python : runtime.python);
        if (!is_file(python)) throw std::runtime_error("Choose an installed Python executable in Engine & diagnostics first");
        if (!is_file(helper)) throw std::runtime_error("Model downloader is missing from the engine folder");
        const bool uses_models_folder=model==FaceSwapDownload::scrfd_model || model==FaceSwapDownload::arcface_model;
        const auto destination=uses_models_folder && !runtime.models.empty() ?
            fs::absolute(fs::u8path(runtime.models)) : assets/"swappers";
        const auto logs=fs::u8path(runtime.logs);
        fs::create_directories(destination);fs::create_directories(logs);
        const auto id=std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
        const auto progress=logs/("download-"+id+".progress");
        const auto partial=destination/(".download-"+id+".part");
        struct Cleanup {
            fs::path progress,partial;
            ~Cleanup() {
                std::error_code ec;
                // A venv launcher and its Python child may release handles on different ticks.
                for (int attempt=0;attempt<20;++attempt) {
                    ec.clear();fs::remove(partial,ec);
                    if (!ec) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                fs::remove(progress,ec);auto temp=progress;temp.replace_extension(".tmp");fs::remove(temp,ec);
            }
        } cleanup{progress,partial};
        struct Child {
            HANDLE job=nullptr,process=nullptr;
            ~Child() {
                if(job) {
                    TerminateJobObject(job,1);
                    for (int attempt=0;attempt<100;++attempt) {
                        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                        if (!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr) || accounting.ActiveProcesses==0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    CloseHandle(job);
                }
                if(process){WaitForSingleObject(process,1000);CloseHandle(process);}
            }
        } child;
        child.job=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!child.job || !SetInformationJobObject(child.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            throw std::runtime_error("Cannot manage model downloader lifetime");
        const char* names[]={"", "alphaface_256","hyperswap_1a_256","hyperswap_1b_256","hyperswap_1c_256","xseg_1","bisenet_resnet_18","yoloface_8n","retinaface_10g","yunet_2023_mar","hrffa","LivePortrait","det_10g","w600k_r50"};
        const auto selected=model==11 ? std::wstring(L"live_portrait_feature_extractor live_portrait_motion_extractor live_portrait_generator live_portrait_stitcher") : quoted(fs::u8path(names[model]));
        auto command=quoted(python)+L" -u "+quoted(helper)+L" --directory "+quoted(destination)+
            L" --models "+selected+L" --progress-file "+quoted(progress)+L" --partial-file "+quoted(partial);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        PROCESS_INFORMATION process{};
        if (cancelled) return;
        if (!CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,
                nullptr,python.parent_path().c_str(),&startup,&process)) throw std::runtime_error("Cannot launch model downloader");
        child.process=process.hProcess;
        const bool assigned=AssignProcessToJobObject(child.job,process.hProcess)!=FALSE;
        const bool resumed=assigned && ResumeThread(process.hThread)!=static_cast<DWORD>(-1);
        CloseHandle(process.hThread);
        if (!resumed) {TerminateProcess(process.hProcess,1);throw std::runtime_error("Cannot manage model download process");}
        auto exit_grace=std::chrono::steady_clock::time_point{};
        while (!cancelled) {
            const auto result=WaitForSingleObject(child.process,100);
            {
                // Permit atomic progress replacement while the GUI reads it.
                std::string contents;
                HANDLE file=CreateFileW(progress.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if (file!=INVALID_HANDLE_VALUE) {
                    char buffer[1024];DWORD length=0;
                    if (ReadFile(file,buffer,sizeof(buffer),&length,nullptr)) contents.assign(buffer,length);
                    CloseHandle(file);
                }
                std::istringstream input(contents);
                std::string phase,message;
                uint64_t received=0,total=0;
                if (std::getline(input,phase) && (input>>received>>total)) {
                    if (!phase.empty() && phase.back()=='\r') phase.pop_back();
                    input.ignore(4096,'\n');std::getline(input,message);
                    if (message.size()<=512) {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.phase=phase;state.received=received;state.total=total;state.message=message;
                    }
                }
            }
            if (result==WAIT_OBJECT_0) {
                DWORD code=1;GetExitCodeProcess(child.process,&code);
                bool terminal=false;
                {std::lock_guard<std::mutex> lock(mutex);terminal=state.phase=="complete" || state.phase=="failed";}
                if(code==0 && !terminal) {
                    if(exit_grace==std::chrono::steady_clock::time_point{})exit_grace=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
                    if(std::chrono::steady_clock::now()<exit_grace) {std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
                }
                std::lock_guard<std::mutex> lock(mutex);
                state.failed=code!=0;
                if (state.failed && state.phase!="failed") state.message="Download failed. Check your connection and Python runtime, then retry.";
                if (!state.failed && state.phase!="complete") {state.failed=true;state.message="Download ended without a verified model";}
                return;
            }
            if (result==WAIT_FAILED) throw std::runtime_error("Cannot monitor model downloader");
        }
#else
        (void)runtime;(void)model;
        throw std::runtime_error("Model download from the app is currently available on Windows only");
#endif
    }
};
FaceSwapDownload::FaceSwapDownload():impl_(std::make_unique<Impl>()) {}
FaceSwapDownload::~FaceSwapDownload(){cancel();}
void FaceSwapDownload::start(FaceSwapRuntime runtime,int model) {
    cancel();impl_->cancelled=false;
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->state={};impl_->state.active=true;impl_->state.message="Starting model download...";}
    impl_->thread=std::thread([this,runtime,model] {
        try {impl_->run(runtime,model);}
        catch(const std::exception& error) {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->state.failed=true;impl_->state.message=error.what();}
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->cancelled){impl_->state.failed=false;impl_->state.message="Download cancelled";impl_->state.phase="cancelled";}
        impl_->state.active=false;
    });
}
void FaceSwapDownload::cancel(){impl_->cancelled=true;if(impl_->thread.joinable())impl_->thread.join();}
FaceSwapDownloadStatus FaceSwapDownload::status() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state;}
}

namespace ap::video {
struct FaceSwapPythonSetup::Impl {
    mutable std::mutex mutex;
    std::thread thread;
    std::atomic<bool> cancelled{false};
    FaceSwapPythonStatus state;
    void run(const FaceSwapRuntime& runtime,int backend) {
#if defined(_WIN32)
        if (backend<0 || backend>2) throw std::runtime_error("Invalid Python backend");
        const auto engine=fs::u8path(runtime.worker).parent_path();
        const auto assets=engine.filename()=="engine" ? engine.parent_path() : engine.parent_path()/"face_swap";
        const auto helper=engine/"prepare_face_swap_python.ps1";
        if (!is_file(helper)) throw std::runtime_error("Python preparation helper is missing from the release");
        const auto logs=fs::u8path(runtime.logs);
        fs::create_directories(logs);
        const auto id=std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
        const auto progress=logs/("python-setup-"+id+".progress");
        const auto log=logs/("python-setup-"+id+".log");
        const auto workspace=fs::absolute(assets/(".python-setup-"+id));
        fs::create_directories(assets);
        if(!fs::create_directory(workspace))throw std::runtime_error("Python preparation workspace already exists");
        {std::lock_guard<std::mutex> lock(mutex);state.log=log.u8string();}
        struct Workspace {
            fs::path path,assets,progress;
            ~Workspace() {
                std::error_code ec;
                const auto target=fs::weakly_canonical(path,ec);
                if (!ec && target.parent_path()==fs::weakly_canonical(assets,ec) && !ec &&
                    target.filename().u8string().rfind(".python-setup-",0)==0) fs::remove_all(target,ec);
                fs::remove(progress,ec);auto temporary=progress;temporary+=L".tmp";fs::remove(temporary,ec);
            }
        } cleanup{workspace,assets,progress};
        // Closing the job kills only this installer and its pip/Python children.
        struct Child {
            HANDLE job=nullptr,process=nullptr,output=INVALID_HANDLE_VALUE,input=INVALID_HANDLE_VALUE;
            ~Child() {
                if(job) {
                    TerminateJobObject(job,1);
                    for(int i=0;i<300;++i) {
                        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                        if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr) || accounting.ActiveProcesses==0)break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    CloseHandle(job);
                }
                if(process)CloseHandle(process);
                if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
                if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
            }
        } child;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        child.output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        child.input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        if(child.output==INVALID_HANDLE_VALUE || child.input==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open Python preparation log");
        child.job=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!child.job || !SetInformationJobObject(child.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            throw std::runtime_error("Cannot manage Python preparation process");
        std::wstring system(32768,L'\0');
        const UINT length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
        if(!length || length>=system.size())throw std::runtime_error("Cannot locate Windows PowerShell");
        system.resize(length);
        const auto powershell=fs::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
        const wchar_t* backends[]={L"cuda",L"directml",L"cpu"};
        auto command=quoted(powershell)+L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quoted(helper)+
            L" -Backend "+backends[backend]+L" -ProgressFile "+quoted(progress)+L" -Workspace "+quoted(workspace);
        if(backend==0 && (runtime.tensorrt || runtime.swapper_tensorrt))command+=L" -TensorRT";
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<unsigned char> memory(size);
        auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        if(!InitializeProcThreadAttributeList(attributes,1,0,&size))throw std::runtime_error("Cannot initialize Python preparation handles");
        struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} attribute_cleanup{attributes};
        HANDLE handles[]={child.output,child.input};
        if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))
            throw std::runtime_error("Cannot configure Python preparation handles");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=child.output;
        startup.StartupInfo.hStdInput=child.input;
        PROCESS_INFORMATION process{};
        if(cancelled)return;
        if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,engine.c_str(),&startup.StartupInfo,&process))
            throw std::runtime_error("Cannot launch Python preparation");
        child.process=process.hProcess;
        const bool assigned=AssignProcessToJobObject(child.job,process.hProcess)!=FALSE;
        const bool resumed=assigned && ResumeThread(process.hThread)!=static_cast<DWORD>(-1);
        CloseHandle(process.hThread);
        if(!resumed){TerminateProcess(child.process,1);throw std::runtime_error("Cannot manage Python preparation lifetime");}
        while(!cancelled) {
            const DWORD wait=WaitForSingleObject(child.process,100);
            HANDLE file=CreateFileW(progress.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) {
                char buffer[16384];DWORD count=0;std::string contents;
                if(ReadFile(file,buffer,sizeof(buffer),&count,nullptr))contents.assign(buffer,count);
                CloseHandle(file);
                std::istringstream input(contents);std::string phase,message,python;uint64_t received=0,total=0;
                if(std::getline(input,phase) && (input>>received>>total)) {
                    input.ignore(4096,'\n');std::getline(input,message);std::getline(input,python);
                    for(auto* line:{&phase,&message,&python})if(!line->empty() && line->back()=='\r')line->pop_back();
                    if(message.size()<=2048 && python.size()<=8192) {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.phase=phase;state.message=message;state.received=received;state.total=total;state.python=python;
                    }
                }
            }
            if(wait==WAIT_OBJECT_0) {
                DWORD code=1;GetExitCodeProcess(child.process,&code);
                std::lock_guard<std::mutex> lock(mutex);
                state.complete=code==0 && state.phase=="complete" && is_file(fs::u8path(state.python));
                state.failed=!state.complete;
                if(state.failed && state.phase!="failed")state.message="Python preparation failed. Open the preparation log for details.";
                return;
            }
            if(wait==WAIT_FAILED)throw std::runtime_error("Cannot monitor Python preparation");
        }
#else
        (void)runtime;(void)backend;
        throw std::runtime_error("Python preparation is available on Windows only");
#endif
    }
};
FaceSwapPythonSetup::FaceSwapPythonSetup():impl_(std::make_unique<Impl>()){}
FaceSwapPythonSetup::~FaceSwapPythonSetup(){cancel();}
void FaceSwapPythonSetup::start(FaceSwapRuntime runtime,int backend) {
    cancel();impl_->cancelled=false;
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->state={};impl_->state.active=true;impl_->state.message="Preparing Python environment...";}
    impl_->thread=std::thread([this,runtime,backend] {
        try {impl_->run(runtime,backend);}
        catch(const std::exception& error){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->state.failed=true;impl_->state.message=error.what();}
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->cancelled){impl_->state.failed=impl_->state.complete=false;impl_->state.phase="cancelled";impl_->state.message="Python preparation cancelled.";}
        impl_->state.active=false;
    });
}
void FaceSwapPythonSetup::cancel(){impl_->cancelled=true;if(impl_->thread.joinable())impl_->thread.join();}
FaceSwapPythonStatus FaceSwapPythonSetup::status()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->state;}
}
