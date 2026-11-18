// Pixel upload/readback below are test instrumentation only.
#include "video/face_swap_gpu.h"
#include "video/face_swap.h"
#include "net/socket.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cuda.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <filesystem>
#include <chrono>
#include <thread>
#include <fstream>
extern "C" {
#include <libavutil/frame.h>
}
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int run(int argc, char** argv) {
    try {
        check(argc == 2 || argc >= 6, "Pass bridge DLL path, optionally followed by Python, worker, models and source photo");
        HMODULE module = LoadLibraryW(std::filesystem::u8path(argv[1]).c_str());
        check(module != nullptr, "CUDA bridge DLL unavailable");
        auto open = reinterpret_cast<void*(*)(uint64_t,uint64_t,uint64_t,unsigned,unsigned,int*)>(GetProcAddress(module,"ap_gpu_open"));
        auto read = reinterpret_cast<int(*)(void*,uint64_t,uint64_t)>(GetProcAddress(module,"ap_gpu_read"));
        auto write = reinterpret_cast<int(*)(void*,uint64_t,uint64_t)>(GetProcAddress(module,"ap_gpu_write"));
        auto close = reinterpret_cast<void(*)(void*)>(GetProcAddress(module,"ap_gpu_close"));
        auto error = reinterpret_cast<const char*(*)()>(GetProcAddress(module,"ap_gpu_error"));
        check(open && read && write && close && error, "CUDA bridge API missing");
        Ptr<IDXGIFactory> factory;
        check(CreateDXGIFactory(IID_PPV_ARGS(&factory)) == S_OK, "No DXGI factory");
        Ptr<IDXGIAdapter> adapter;
        for (UINT index=0;;++index) {
            Ptr<IDXGIAdapter> candidate;
            if (factory->EnumAdapters(index,&candidate) != S_OK) break;
            DXGI_ADAPTER_DESC desc{}; candidate->GetDesc(&desc);
            if (desc.VendorId == 0x10de) { adapter=candidate; break; }
        }
        check(adapter != nullptr, "NVIDIA adapter required");
        Ptr<ID3D11Device> device;
        Ptr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
              nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context) == S_OK,"D3D11 device creation failed");
        if (argc >= 6) {
            check(ap::net::global_init(), "Winsock initialization failed");
            ap::video::FaceSwapRuntime runtime = ap::video::FaceSwapRuntime::discover();
            runtime.python = argv[2]; runtime.worker = argv[3]; runtime.models = argv[4];
            runtime.logs = (std::filesystem::absolute(std::filesystem::u8path(argv[0])).parent_path() / "gpu-face-test-logs").u8string();
            runtime.gpu_bridge = std::filesystem::absolute(std::filesystem::u8path(argv[1])).u8string();
            runtime.gpu_resident = true;
            if (argc >= 7) runtime.quality = std::stoi(argv[6]);
            if (argc >= 11) runtime.swapper_model = std::stoi(argv[10]);
            if (argc >= 12) runtime.swapper_tensorrt = std::stoi(argv[11]) != 0;
            if (argc >= 13) runtime.occlusion = std::stoi(argv[12]) != 0;
            if (argc >= 14) runtime.parser = std::stoi(argv[13]);
            if (argc >= 16) runtime.detector=std::stoi(argv[15]);
            if (argc >= 18) runtime.swapper_builder=std::stoi(argv[17]);
            if (argc >= 19) runtime.landmarker=std::stoi(argv[18]);
            if (argc >= 20) runtime.animation=std::stoi(argv[19]);
            if (argc >= 15) {runtime.mask_shape=1;runtime.mask_padding={8,12,5,10};runtime.mask_blur=25;runtime.mask_preview=std::stoi(argv[14]);}
            const bool face_fixture = argc >= 10;
            const int width = face_fixture ? std::stoi(argv[8]) : 160;
            const int height = face_fixture ? std::stoi(argv[9]) : 96;
            std::vector<uint8_t> pixels(width * height * 4);
            for (size_t at = 0; at < pixels.size(); at += 4) { pixels[at] = 17; pixels[at+1] = 42; pixels[at+2] = 199; pixels[at+3] = 255; }
            if (face_fixture) {
                std::ifstream file(std::filesystem::u8path(argv[7]),std::ios::binary);
                check(bool(file.read(reinterpret_cast<char*>(pixels.data()),pixels.size())),"Cannot read BGRA photo fixture");
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width;desc.Height=height;desc.ArraySize=desc.MipLevels=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(),static_cast<UINT>(width*4),0};
            Ptr<ID3D11Texture2D> input;
            check(device->CreateTexture2D(&desc,&data,&input)==S_OK,"Create native GPU pipeline fixture");
            AVFrame* frame=av_frame_alloc();
            frame->format=AV_PIX_FMT_D3D11;frame->width=width;frame->height=height;
            frame->color_range=AVCOL_RANGE_JPEG;frame->colorspace=AVCOL_SPC_RGB;
            input->AddRef();
            frame->buf[0]=av_buffer_create(reinterpret_cast<uint8_t*>(input.Get()),1,
                [](void*,uint8_t* pointer) { reinterpret_cast<ID3D11Texture2D*>(pointer)->Release(); },nullptr,0);
            check(frame->buf[0]!=nullptr,"Create fixture frame buffer");
            frame->data[0]=reinterpret_cast<uint8_t*>(input.Get());
            ap::video::FaceSwap swap;
            swap.start(argv[5],runtime);
            AVFrame* result=nullptr;int64_t origin=0;
            const bool cold_build=runtime.animation || (runtime.tensorrt && runtime.quality>0) || (runtime.swapper_model>0 && runtime.swapper_tensorrt);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(cold_build ? 600 : 90);
            while(swap.preparation().active && !swap.failed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            check(!swap.failed() && !swap.preparation().active,"GPU models require video before preparation can complete");
            check(swap.status().find("GPU models ready")!=std::string::npos,"GPU headless preparation was not acknowledged");
            std::cout<<"GPU model preparation passed without submitting video frames\n";
            while(!result && !swap.failed() && std::chrono::steady_clock::now()<deadline) {
                swap.submit(frame,12345);
                result=swap.take(width,height,origin);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::cout<<"Managed pipeline: "<<swap.status()<<"\nLog: "<<swap.log_path()<<'\n';
            check(result && result->format==AV_PIX_FMT_D3D11 && origin==12345,"Managed worker did not return a GPU-resident frame");
            auto* texture=reinterpret_cast<ID3D11Texture2D*>(result->data[0]);
            texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging)==S_OK,"Allocate validation staging texture");
            context->CopyResource(staging.Get(),texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)==S_OK,"Read GPU result in test only");
            auto* actual=static_cast<uint8_t*>(mapped.pData);
            if (!runtime.mask_preview) for(int channel=0;channel<4;++channel)check(std::abs(int(actual[channel])-pixels[channel])<=2,"Managed GPU pipeline changed no-face fixture pixels");
            if (face_fixture) {
                size_t changed=0,cyan=0;
                std::ofstream saved(std::filesystem::u8path(runtime.logs)/"native-result.bgra",std::ios::binary);
                for(int row=0;row<height;++row) {
                    auto* line=actual+row*mapped.RowPitch;
                    saved.write(reinterpret_cast<char*>(line),width*4);
                    if(runtime.landmarker)for(int x=0;x<width;++x)if(line[x*4]==255 && line[x*4+1]==200 && line[x*4+2]==0)++cyan;
                    for(int index=0;index<width*4;++index)if(line[index]!=pixels[static_cast<size_t>(row)*width*4+index])++changed;
                }
                check(changed>100,"Face fixture returned unchanged pixels");
                if(runtime.landmarker)check(cyan>20,"HRFFA did not produce accepted 68-point diagnostic landmarks");
                std::cout<<"Face fixture changed "<<changed<<" channel values; GPU result saved for visual QA\n";
            }
            context->Unmap(staging.Get(),0);
            av_frame_free(&result);
            if(argc>=17 && std::string(argv[16])=="--live-mask") {
                std::this_thread::sleep_for(std::chrono::milliseconds(650));
                AVFrame* old=swap.take(width,height,origin);av_frame_free(&old);
                swap.set_mask(1,30,2,{12,7,9,5},105,110,1,4,60,1);
                const auto live_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                while(std::chrono::steady_clock::now()<live_deadline && !swap.failed()) {
                    result=swap.take(width,height,origin);
                    if(result && origin==12345)break;
                    av_frame_free(&result);std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                check(result && origin==12345,"Still image did not refresh after live mask edit");
                auto* live_texture=reinterpret_cast<ID3D11Texture2D*>(result->data[0]);
                context->CopyResource(staging.Get(),live_texture);
                check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)==S_OK,"Read live mask fixture");
                const auto* first=static_cast<const uint8_t*>(mapped.pData);
                check(first[0]==0 && first[1]==0 && first[2]==0,"Live mask-only preview was not applied");
                context->Unmap(staging.Get(),0);av_frame_free(&result);
                std::cout<<"Still-image mask refresh passed without new video input on the same managed worker connection\n";
            }
            if (width >= 96 && height >= 64) {
                // Change the visible source box without changing the decoder
                // device, like a pool whose coded texture has extra padding.
                frame->width=96;frame->height=64;
                const auto resize_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                while(!result && !swap.failed() && std::chrono::steady_clock::now()<resize_deadline) {
                    swap.submit(frame,54321);
                    result=swap.take(96,64,origin);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                check(result && result->format==AV_PIX_FMT_D3D11 && origin==54321 && result->width==96 && result->height==64,
                      "GPU-resident resolution change failed");
                av_frame_free(&result);
                std::cout<<"Managed GPU resolution change passed 96x64\n";
            }
            av_frame_free(&frame);
            swap.stop();ap::net::global_shutdown();
            std::cout<<"Managed D3D11 -> CUDA/ORT -> D3D11 pipeline passed\n";
        }
        ap::video::FaceSwapGpu transport;
        for (int width : {64,64,96}) {
            int height=48;
            std::vector<uint8_t> pixels(static_cast<size_t>(width)*height*4);
            for (size_t i=0;i<pixels.size();i+=4) { pixels[i]=17; pixels[i+1]=42; pixels[i+2]=199; pixels[i+3]=255; }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width;desc.Height=height;desc.ArraySize=desc.MipLevels=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(),static_cast<UINT>(width*4),0};
            Ptr<ID3D11Texture2D> input;
            check(device->CreateTexture2D(&desc,&data,&input)==S_OK,"Input texture fixture failed");
            AVFrame* source=av_frame_alloc(); source->format=AV_PIX_FMT_D3D11;
            source->width=width;source->height=height;source->color_range=AVCOL_RANGE_JPEG;source->colorspace=AVCOL_SPC_RGB;
            source->data[0]=reinterpret_cast<uint8_t*>(input.Get());
            ap::video::GpuTextureTicket ticket;
            if (!transport.prepare(source,1920,ticket)) throw std::runtime_error(transport.error());
            av_frame_free(&source);
            int ordinal=-1;
            void* bridge=open(ticket.input,ticket.output,ticket.adapter,width,height,&ordinal);
            check(bridge != nullptr,error());
            CUdeviceptr buffer=0;
            check(cuMemAlloc(&buffer,pixels.size())==CUDA_SUCCESS,"CUDA buffer allocation failed");
            check(read(bridge,buffer,0)!=0,error());
            std::vector<uint8_t> downloaded(pixels.size());
            check(cuMemcpyDtoH(downloaded.data(),buffer,downloaded.size())==CUDA_SUCCESS,"Test CUDA readback failed");
            for (int channel=0;channel<3;++channel)
                check(std::abs(int(downloaded[channel])-int(pixels[channel]))<=2,"Input D3D11/CUDA color mismatch");
            check(write(bridge,buffer,0)!=0,error());
            AVFrame* output=transport.complete();if(!output)throw std::runtime_error(transport.error());
            auto* texture=reinterpret_cast<ID3D11Texture2D*>(output->data[0]);
            texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging)==S_OK,"Test staging allocation failed");
            context->CopyResource(staging.Get(),texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)==S_OK,"Test staging readback failed");
            auto* result=static_cast<uint8_t*>(mapped.pData);
            for (int channel=0;channel<4;++channel)check(result[channel]==downloaded[channel],"CUDA/D3D11 output pixels changed");
            context->Unmap(staging.Get(),0);
            av_frame_free(&output);
            cuMemFree(buffer);close(bridge);
            std::cout<<"GPU interop passed "<<width<<"x"<<height<<", CUDA device "<<ordinal<<'\n';
        }
        FreeLibrary(module);
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}

int wmain(int argc,wchar_t** wide) {
    std::vector<std::string> text;
    for(int index=0;index<argc;++index) {
        int size=WideCharToMultiByte(CP_UTF8,0,wide[index],-1,nullptr,0,nullptr,nullptr);
        std::string value(static_cast<size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,0,wide[index],-1,value.data(),size,nullptr,nullptr);value.pop_back();text.push_back(std::move(value));
    }
    std::vector<char*> pointers;for(auto& value:text)pointers.push_back(value.data());
    return run(argc,pointers.data());
}
