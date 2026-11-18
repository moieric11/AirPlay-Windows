#include "video/face_swap.h"
#include "video/face_swap_cadence.h"
#include <sstream>
#include <map>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include "net/socket.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

int test_cadence_metrics() {
    using C=ap::video::FaceSwapCadence;
    const auto path=std::filesystem::temp_directory_path() / ("airplay-cadence-"+std::to_string(C::Clock::now().time_since_epoch().count())+".csv");
    try {
        C metrics; metrics.open(path);
        const auto now=C::Clock::now();
        metrics.renderer(100,5,false,false,false,1,.5,-1);
        metrics.renderer(104,7,true,true,true,3,1.5,70);
        for (int i=0;i<3;++i) metrics.event(C::Submitted);
        for (int i=0;i<2;++i) {metrics.event(C::Started);metrics.event(C::Completed);}
        metrics.event(C::Taken);metrics.event(C::PendingReplaced);metrics.event(C::OutputReplaced);
        metrics.timing(C::Queue,1);metrics.timing(C::Queue,3);metrics.timing(C::ResultHold,4);
        metrics.flush(false,now+std::chrono::seconds(2));
        // Session source counters may reset during teardown/reconnect.
        metrics.renderer(0,0,false,false,false,0,0,-1);
        metrics.flush(false,now+std::chrono::seconds(4));metrics.close();
        std::ifstream input(path);std::string line;
        std::getline(input,line);std::stringstream header(line);std::vector<std::string> names;
        while(std::getline(header,line,',')) names.push_back(line);
        std::vector<std::map<std::string,double>> rows;
        while(std::getline(input,line)) {
            std::stringstream values(line);std::string value;std::map<std::string,double> row;size_t index=0;
            while(std::getline(values,value,',')) {if(index>=names.size()) throw std::runtime_error("Extra CSV columns");row[names[index++]]=std::stod(value);}
            if(index!=names.size()) throw std::runtime_error("Missing CSV columns");rows.push_back(row);
        }
        if(rows.size()!=2) throw std::runtime_error("Unexpected cadence interval count");
        auto check=[&](size_t row,const char* key,double wanted) {
            if(std::abs(rows[row].at(key)-wanted)>.01) throw std::runtime_error(std::string("Cadence metric mismatch: ")+key);
        };
        check(0,"decoded_fps",2);check(0,"submitted_fps",1.5);check(0,"completed_fps",1);
        check(0,"taken_fps",.5);check(0,"presented_fps",.5);check(0,"decoder_replaced",2);
        check(0,"pending_replaced",1);check(0,"output_replaced",1);check(0,"held",1);
        check(0,"queue_ms",2);check(0,"result_hold_ms",4);check(0,"render_wait_ms",2);
        check(0,"present_wait_ms",1);check(0,"present_max_ms",1.5);check(0,"origin_age_ms",70);
        check(1,"decoded_fps",0);check(1,"completed_fps",0);check(1,"queue_ms",0);check(1,"result_hold_max_ms",0);
        input.close();std::filesystem::remove(path);
        std::cout<<"Cadence rates, counter resets, timings and CSV structure passed\n";return 0;
    } catch(const std::exception& error) {
        std::error_code ignored;std::filesystem::remove(path,ignored);std::cerr<<error.what()<<'\n';return 1;
    }
}

int test_download(const char* python,const char* directory) {
    using namespace std::chrono;
    const auto root=std::filesystem::u8path(directory);
    ap::video::FaceSwapRuntime runtime;
    runtime.python=python;runtime.worker=(root/"engine"/"face_swap_worker.py").u8string();runtime.logs=(root/"logs").u8string();
    ap::video::FaceSwapDownload download;
    download.start(runtime,3);
    auto deadline=steady_clock::now()+seconds(10);
    bool progress=false;
    while(download.status().active && steady_clock::now()<deadline) {
        const auto state=download.status();progress|=state.received>0 && state.total>state.received;
        std::this_thread::sleep_for(milliseconds(10));
    }
    auto state=download.status();
    if(state.active || state.failed || state.phase!="complete" || !progress) {std::cerr<<state.message;return 1;}
    const auto target=root/"swappers"/"hyperswap_1b_256.onnx";
    {std::ofstream previous(target,std::ios::binary);previous<<"previous";}
    download.start(runtime,3);
    deadline=steady_clock::now()+seconds(5);
    while(download.status().active && download.status().received==0 && steady_clock::now()<deadline)
        std::this_thread::sleep_for(milliseconds(10));
    if(!download.status().active || download.status().received==0) {std::cerr<<"Cancellation fixture did not start";return 1;}
    const auto begin=steady_clock::now();download.cancel();
    if(steady_clock::now()-begin>seconds(2) || download.status().active || download.status().phase!="cancelled") return 1;
    std::ifstream previous(target,std::ios::binary);std::string contents;std::getline(previous,contents);
    if(contents!="previous") {std::cerr<<"Cancelled transfer replaced existing model";return 1;}
    for(const auto& file:std::filesystem::directory_iterator(root/"swappers"))
        if(file.path().extension()==".part") {std::cerr<<"Cancelled partial left behind";return 1;}
    std::cout<<"Native model download, real progress, cancellation and preservation passed\n";
    return 0;
}
int run_test(int argc, char** argv) {
    if(argc==4 && std::string(argv[1])=="--download-test") return test_download(argv[2],argv[3]);
    if(argc==2 && std::string(argv[1])=="--test-cadence") return test_cadence_metrics();
    if (argc==2 && std::string(argv[1])=="--list-adapters") {
        for (const auto& name : ap::video::FaceSwapRuntime::directml_adapters()) std::cout<<name<<'\n';
        return 0;
    }
    if (!ap::net::global_init()) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    ap::video::FaceSwapRuntime runtime;
    runtime.launch = false;
    if (argc > 1) runtime.port = static_cast<uint16_t>(std::stoi(argv[1]));
    const bool detach_test=argc==3 && std::string(argv[2])=="--detach-test";
    const bool mask_live_test=argc==3 && std::string(argv[2])=="--mask-live-test";
    const bool wake_test=argc==3 && std::string(argv[2])=="--wake-test";
    runtime.cpu = argc == 3 && (std::string(argv[2]) == "--cpu" || wake_test || detach_test);
    const bool managed = argc >= 5;
    if (managed) {
        runtime.swapper_builder=3; // Exercise the global level even with InSwapper + GPEN.
        runtime.launch = true;
        runtime.python = argv[2];
        runtime.worker = argv[3];
        runtime.models = argv[4];
        if (argc>=7) {
            runtime.directml=std::string(argv[6])=="--directml";
            runtime.cpu=std::string(argv[6])=="--cpu";
            runtime.directml_python=runtime.python;
        }
        runtime.logs = (std::filesystem::u8path(argv[4]) / "logs with spaces").u8string();
        if (argc >= 6 && std::string(argv[5]) != "--crash") {
            runtime.logs = (std::filesystem::u8path(argv[0]).parent_path() / "face-swap-test-logs").u8string();
        }
    }
    {
        ap::video::FaceSwap swap;
        AVFrame* input = av_frame_alloc();
        if (!input) return 1;
        input->format = AV_PIX_FMT_YUV420P;
        input->width = 64; input->height = 48;
        if (av_frame_get_buffer(input, 32) < 0) return 1;
        for (int plane = 0; plane < 3; ++plane)
            for (int y = 0; y < (plane ? 24 : 48); ++y)
                std::memset(input->data[plane] + y * input->linesize[plane], plane ? 128 : 80, plane ? 32 : 64);
        if (mask_live_test) {
            swap.start("normal",runtime);
            swap.set_mask(1,20,0,{1,2,3,4}); // Queue even while source/model preparation is in flight.
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(swap.status().find("ready")==std::string::npos && !swap.failed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            swap.set_mask(1,27,2,{4,3,2,1},105,110);
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // No video input yet.
            swap.submit(input,999);
            int64_t origin=0;AVFrame* output=nullptr;
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!output && !swap.failed() && std::chrono::steady_clock::now()<deadline) {
                output=swap.take(64,48,origin);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(output && origin==999,"Live mask update interrupted video processing");
            av_frame_free(&output);
            std::this_thread::sleep_for(std::chrono::milliseconds(650)); // Original frame is now older than the freshness limit.
            swap.set_mask(1,31,2,{4,3,2,1},105,110,1,4,60,1,70); // No new submit: refresh a still image.
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!output && !swap.failed() && std::chrono::steady_clock::now()<deadline) {
                output=swap.take(64,48,origin);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(output && origin==999 && output->data[0][0]==31,"Mask edit did not refresh the last original frame");
            av_frame_free(&output);
            swap.set_mask(1,31,2,{4,3,2,1},105,110,1,4,60,1,70); // Identical controls must not replay continuously.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(!swap.has_result(),"Unchanged mask generated another preview frame");
            swap.detach_video();swap.set_mask(1,32,2,{4,3,2,1},105,110,1,4,60,1,70);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(!swap.has_result(),"Disconnected session replayed an old image");
            swap.stop();av_frame_free(&input);
            ap::net::global_shutdown();return failures ? 1 : 0;
        }
        if (detach_test) {
            swap.start("normal",runtime);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while (swap.status().find("ready")==std::string::npos && !swap.failed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            swap.submit(input,111);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(!swap.failed(),"Fixture failed before disconnect");
            swap.detach_video();
            check(!swap.failed(),"Disconnect stopped engine");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while (swap.preparation().active && !swap.failed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(!swap.preparation().active && !swap.failed(),"Preparation did not finish without mirroring");
            check(!swap.has_result(),"Disconnected video result escaped");
            swap.submit(input,222); // Same dimensions, new session, same worker connection.
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while (!swap.has_result() && !swap.failed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            int64_t origin=0;AVFrame* output=swap.take(64,48,origin);
            check(output && origin==222,"Cannot resume using prepared worker after disconnect");
            av_frame_free(&output);swap.stop();av_frame_free(&input);
            ap::net::global_shutdown();return failures ? 1 : 0;
        }
        if (wake_test) {
            std::mutex wake_mutex;std::condition_variable wake_cv;bool ready=false;
            swap.set_result_callback([&] {
                // Re-entering has_result() also checks callback lock ordering.
                const bool has_output=swap.has_result();
                std::lock_guard<std::mutex> lock(wake_mutex);ready=has_output;wake_cv.notify_one();
            });
            swap.start("normal",runtime);
            swap.submit(input,123456); // Deliberately no further decoded input.
            {
                std::unique_lock<std::mutex> lock(wake_mutex);
                check(wake_cv.wait_for(lock,std::chrono::seconds(4),[&]{return ready;}),"Completed frame did not wake consumer");
            }
            int64_t origin=0;AVFrame* output=swap.take(64,48,origin);
            check(output && origin==123456,"Cannot consume result without new input");
            check(!swap.has_result(),"Ready predicate stayed set after take");
            av_frame_free(&output);swap.stop();av_frame_free(&input);
            ap::net::global_shutdown();return failures ? 1 : 0;
        }
        const std::string source = argc >= 6 ? argv[5] : "normal";
        swap.start(source == "--crash" ? "normal" : source, runtime);
        AVFrame* result = nullptr;
        int64_t origin = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(argc >= 6 ? 60 : 6);
        while (!result && !swap.failed() && std::chrono::steady_clock::now() < deadline) {
            swap.submit(input, 123456);
            result = swap.take(64, 48, origin);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(result && result->width == 64 && result->height == 48 &&
              result->format == AV_PIX_FMT_BGR24 && origin == 123456, "Native frame round trip failed");
        if (result) check(result->data[0][0] > 65 && result->data[0][0] < 85, "Pixel conversion changed gray fixture");
        av_frame_free(&result);
        if (runtime.cpu) {
            check(swap.status().find("CPU - ") == 0, "CPU status missing");
            swap.stop();
            av_frame_free(&input);
            ap::net::global_shutdown();
            return failures ? 1 : 0;
        }
        if (argc >= 6) {
            if (source == "--crash" && !failures) std::_Exit(0);
            std::cout << swap.status() << "\nLog: " << swap.log_path() << '\n';
            swap.stop();
            av_frame_free(&input);
            ap::net::global_shutdown();
            return failures ? 1 : 0;
        }
        check(av_frame_make_writable(input) >= 0, "Cannot prepare color fixture");
        input->colorspace = AVCOL_SPC_BT470BG;
        input->color_range = AVCOL_RANGE_MPEG;
        const int values[] = {81, 90, 240}; // limited-range BT.601 red
        for (int plane = 0; plane < 3; ++plane)
            for (int row = 0; row < (plane ? 24 : 48); ++row)
                std::memset(input->data[plane] + row * input->linesize[plane], values[plane], plane ? 32 : 64);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            swap.submit(input, 654321);
            result = swap.take(64, 48, origin);
            if (result && origin == 654321) break;
            av_frame_free(&result);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(result && result->data[0][0] < 8 && result->data[0][1] < 8 && result->data[0][2] > 245,
              "BT.601 matrix/range or RGB channel order changed");
        av_frame_free(&result);
        // A completed frame must never be delivered to a different resolution.
        swap.submit(input, 123456);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result = swap.take(128, 96, origin);
        check(!result, "Old-resolution frame escaped");
        av_frame_free(&result);
        swap.submit(input, 123456);
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        result = swap.take(64, 48, origin);
        check(!result, "Expired frame escaped");
        av_frame_free(&result);
        swap.stop();
        for (const char* mode : {"reject", "malformed"}) {
            swap.start(mode, runtime);
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!swap.failed() && std::chrono::steady_clock::now() < deadline) {
                swap.submit(input, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(swap.failed(), "Worker error was not reported");
            swap.stop();
        }
        swap.start("stall", runtime);
        check(swap.preparation().active,"Preparation activity missing during loading");
        swap.submit(input, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (managed) {
            const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while (swap.log_path().empty() && !swap.failed() && std::chrono::steady_clock::now()<ready_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto progress=std::filesystem::u8path(swap.log_path());
            check(!progress.empty(),"Managed preparation has no diagnostics path");
            if (!progress.empty()) {
                progress.replace_extension(".progress");
                { std::ofstream stage(progress); stage << "TensorRT: building test engine (level 5)"; }
                const auto preparing=swap.preparation();
                check(preparing.active && preparing.stage=="TensorRT: building test engine (level 5)",
                      "Preparation stage not propagated from worker");
                check(preparing.elapsed_seconds>0,"Preparation elapsed time missing");
            }
        }
        auto begin = std::chrono::steady_clock::now();
        swap.stop();
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1), "Cancellation blocked on worker");
        check(!swap.preparation().active,"Cancelled preparation remained active");
        if (managed) {
            // Cancel while the child is still loading models; the private
            // Windows job must clean it up before readiness is published.
            const auto marker = std::filesystem::u8path(runtime.models) / "stall-startup";
            std::ofstream(marker).put('1');
            swap.start("normal", runtime);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            begin = std::chrono::steady_clock::now();
            swap.stop();
            check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "Cancellation blocked during startup");
            std::filesystem::remove(marker);
        }
        swap.start("", runtime);
        check(swap.failed(), "Empty source accepted");
        av_frame_free(&input);
    }
    ap::net::global_shutdown();
    if (!failures) std::cout << "Native face-swap integration passed\n";
    return failures ? 1 : 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide) {
    std::vector<std::string> values;
    for (int index = 0; index < argc; ++index) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, value.data(), size, nullptr, nullptr);
        value.pop_back();
        values.push_back(std::move(value));
    }
    std::vector<char*> arguments;
    for (auto& value : values) arguments.push_back(value.data());
    return run_test(argc, arguments.data());
}
#else
int main(int argc, char** argv) { return run_test(argc, argv); }
#endif
