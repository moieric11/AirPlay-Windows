// Offscreen rendering of the actual UI component; no desktop window or GPU.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdlrenderer2.h"
#include "video/face_swap_settings_ui.h"
#include "video/live_preview_window.h"
#include "video/standard_settings_ui.h"
#include "video/ui_fonts.h"
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <stdexcept>

void verify_external_preview() {
    SDL_SetHint(SDL_HINT_VIDEODRIVER,"dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    if (SDL_Init(SDL_INIT_VIDEO)!=0) throw std::runtime_error(SDL_GetError());
    const auto require=[](bool ok,const char* message) {
        if (!ok) throw std::runtime_error(message);
    };
    auto* main_window=SDL_CreateWindow("Main",0,0,80,80,SDL_WINDOW_HIDDEN);
    require(main_window!=nullptr,"Cannot create the main test window");
    auto* renderer=SDL_CreateRenderer(main_window,-1,SDL_RENDERER_SOFTWARE);
    require(renderer!=nullptr,"Cannot create the main test renderer");
    auto* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STATIC,2,1);
    const Uint32 input[]={0xffff0000u,0xff00ff00u};
    require(texture && SDL_UpdateTexture(texture,nullptr,input,sizeof(input))==0,"Cannot create the test frame");

    ap::video::LivePreviewWindow preview;
    require(preview.show(main_window,"Preview"),SDL_GetError());
    SDL_Event event;
    SDL_Window* preview_window=nullptr;
    while (SDL_PollEvent(&event)) {
        if (event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_SHOWN &&
            event.window.windowID!=SDL_GetWindowID(main_window))
            preview_window=SDL_GetWindowFromID(event.window.windowID);
    }
    require(preview_window!=nullptr,"The preview must have a separate SDL window");
    require(preview.show(main_window,"Preview"),"Cannot raise the existing preview");
    while (SDL_PollEvent(&event))
        require(event.type!=SDL_WINDOWEVENT || event.window.event!=SDL_WINDOWEVENT_SHOWN ||
            event.window.windowID==SDL_GetWindowID(preview_window),"Raising the preview created a second window");

    preview.draw(renderer,texture,2,1,90,ap::video::LivePreviewWindow::Clock::now(),"Preview","Waiting");
    require(SDL_GetRenderTarget(renderer)==nullptr,"Preview did not restore the main render target");
    auto* preview_renderer=SDL_GetRenderer(preview_window);
    int width=0,height=0;
    require(SDL_GetRendererOutputSize(preview_renderer,&width,&height)==0,"Cannot query preview size");
    const SDL_Rect upper{width/2,height/4,1,1};
    const SDL_Rect lower{width/2,3*height/4,1,1};
    Uint32 upper_pixel=0, lower_pixel=0;
    require(SDL_RenderReadPixels(preview_renderer,&upper,SDL_PIXELFORMAT_ARGB8888,&upper_pixel,4)==0 &&
        SDL_RenderReadPixels(preview_renderer,&lower,SDL_PIXELFORMAT_ARGB8888,&lower_pixel,4)==0,
        "Cannot read the preview frame");
    require((upper_pixel&0x00ff0000u)>(upper_pixel&0x0000ff00u)*256 &&
        (lower_pixel&0x0000ff00u)*256>(lower_pixel&0x00ff0000u),"Preview did not display the rotated frame");

    event={};
    event.type=SDL_WINDOWEVENT;
    event.window.event=SDL_WINDOWEVENT_CLOSE;
    event.window.windowID=SDL_GetWindowID(main_window);
    require(!preview.handle_event(event),"The preview intercepted the main window close event");
    event.window.windowID=SDL_GetWindowID(preview_window);
    require(preview.handle_event(event) && !SDL_GetWindowFromID(event.window.windowID),"The preview did not close independently");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(main_window);
    SDL_Quit();
    std::cout<<"External preview window, rotated frame and independent close verified\n";
}

void render(const std::filesystem::path& output,int width,int height,int model,bool active,bool failed,float scroll=0,int backend=0,int builder=5,bool dark=false,const char* language="en",int python_state=0,bool has_source=true) {
    SDL_Surface* surface=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
    if (!surface) throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer=SDL_CreateSoftwareRenderer(surface);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
    io.DisplaySize=ImVec2(static_cast<float>(width),static_cast<float>(height));
    io.DeltaTime=1.f/60;
    ap::video::receiver_ui::apply_theme(dark);
    ap::i18n::set_language(language);
    io.FontDefault=ap::video::ui_fonts::add_font("C:/Windows/Fonts/segoeui.ttf",18);
    auto* heading=ap::video::ui_fonts::add_font("C:/Windows/Fonts/seguisb.ttf",28,true);
    auto* bold=ap::video::ui_fonts::add_font("C:/Windows/Fonts/seguisb.ttf",18,true);
    auto* small=ap::video::ui_fonts::add_font("C:/Windows/Fonts/segoeui.ttf",16);
    ImGui_ImplSDLRenderer2_Init(renderer);
    ap::video::FaceSwapSettingsUi ui;
    ui.model=model;ui.enabled=active;ui.quality=1;ui.backend=backend;ui.swapper_builder=builder;
    ui.adapters={"0: NVIDIA GeForce RTX 3080 Laptop GPU","1: Intel integrated graphics"};
    std::snprintf(ui.directml_python.data(),ui.directml_python.size(),"D:/AirPlay/.venv-face-swap-directml/Scripts/python.exe");
    if (has_source) std::snprintf(ui.source.data(),ui.source.size(),"D:/Photos/source-face.jpg");
    std::snprintf(ui.models.data(),ui.models.size(),"D:/AI/VisoMaster-Fusion/model_assets");
    std::snprintf(ui.python.data(),ui.python.size(),"D:/source/repos/AirPlay-Windows/.venv-face-swap/Scripts/python.exe");
    std::snprintf(ui.cpu_python.data(),ui.cpu_python.size(),"D:/AirPlay/face_swap/python-cpu/python.exe");
    ap::video::FaceSwapSettingsStatus status;
    status.failed=failed;status.has_output=active;status.mirror_connected=active;status.gpu_decoding=true;
    status.message=failed ? "Selected model could not start. Check the model file and runtime, then retry." :
        active ? (backend==2 ? "CPU - 380 ms/frame" : backend==1 ? "DirectML [CPU pixels] - 35 ms/frame" : "CUDA [GPU resident, no image readback] - 14 ms/frame") : "Disabled";
    if (builder==92) {ui.animation=1;ui.swapper_builder=5;}
    if (builder==93) {
        ui.landmarker=1;ui.alignment=1;ui.mask_expansion=4;ui.shape_transfer=40;
        ui.enabled=true;status.mirror_connected=true;status.has_output=true;

    }
    if (builder==94) {ui.detector=1;ui.download_terms=true;ui.accepted_model=1;ui.swapper_builder=5;}
    if (builder==95) {
        ui.mask_shape=1;ui.mask_padding={8,12,5,10};ui.mask_blur=25;ui.mask_preview=1;ui.swapper_builder=5;
    }
    if (builder==96) {ui.parser=2;ui.swapper_builder=5;}
    if (builder==97) {ui.occlusion=true;ui.swapper_builder=5;}
    if (builder==98) {
        ui.swapper_builder=5;ui.download_choice=3;ui.download_terms=true;ui.accepted_model=7;
        status.downloading=true;status.download_fraction=.42f;
        status.download_message="Downloading hyperswap_1b_256";
        status.download_directory="D:/AirPlay/face_swap/swappers";
    }
    if (builder==99) {
        ui.swapper_builder=5;ui.enabled=true;
        status.has_output=false;status.mirror_connected=true;status.preparing=true;
        status.preparation_stage="TensorRT: building engine - hyperswap_1b_256 (level 5)";
        status.preparation_seconds=185;
        status.message="Preparing GPU models / TensorRT cache...";
    }
    status.log="D:/AirPlay-Windows/face-swap-logs/current-engine.log";
    if(python_state) {
        ui.enabled=false;status.has_output=false;
        status.failed=python_state==1;
        status.python_missing=python_state!=4;
        status.message=python_state==1 ? "Face-swap Python runtime missing. Check Runtime settings." : "Disabled";
        status.python_setup_active=python_state==2;
        status.python_setup_failed=python_state==3;
        status.python_setup_ready=python_state==4;
        status.python_setup_message=python_state==2 ? "Installing Python dependencies..." : python_state==3 ? "Python preparation failed. Open the preparation log for details." : "";
        if(python_state>1)status.python_setup_log="D:/AirPlay/face_swap/logs/python-setup.log";
    }
    ap::airplay::LiveSettings live;
    std::array<char,51> receiver{};std::snprintf(receiver.data(),receiver.size(),"AirPlay-Windows");
    std::array<char,1024> directory{};std::snprintf(directory.data(),directory.size(),"D:/Videos/AirPlay recordings");
    ap::video::VideoRecorder::Options options;
    ap::video::VideoRecorder::Status recording;
    recording.recording=active; recording.elapsed_ms=125000; recording.frames=7500;
    if (failed) {recording.error="Could not open output folder"; recording.output_path="D:/Videos/AirPlay recordings/test-recording.mp4";}
    bool diagnostics=false,custom=model==-2;
    if (custom) {live.mirror_width=1352;live.mirror_height=2400;}
    ap::video::StandardSettingsContext settings{&live,receiver,directory,options,recording,diagnostics,custom,
        1080,1920,0,[](std::string&){return false;},[]{},[]{},[](const char* code){ap::i18n::set_language(code);}};
    for (int frame=0;frame<8;++frame) {
        ImGui_ImplSDLRenderer2_NewFrame();ImGui::NewFrame();
        if (frame==0) {
            for (const auto& entry : ap::i18n::translations) {
                const auto* cursor=reinterpret_cast<const unsigned char*>(ap::i18n::tr(entry.english));
                while (*cursor) {
                    unsigned int codepoint=*cursor++;
                    if (codepoint>=0xc0) {
                        int count=codepoint<0xe0 ? 1 : codepoint<0xf0 ? 2 : 3;
                        codepoint&=(1u<<(6-count))-1;
                        while (count-->0) codepoint=(codepoint<<6)|(*cursor++&0x3f);
                    }
                    if (codepoint<32) continue;
                    for (auto* font : {io.FontDefault,heading,bold,small})
                        if (!font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)))
                            throw std::runtime_error(std::string("Missing translated glyph: ")+entry.english);
                }
            }
        }
        ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(42,0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg,ImGui::ColorConvertU32ToFloat4(ap::video::receiver_ui::paper));
        ImGui::Begin("##fixture",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
        if (scroll>0 && frame==5) ImGui::SetScrollY(scroll);
        if (model<0) ap::video::draw_standard_settings(settings,heading,bold,small);
        else ap::video::draw_face_swap_settings(ui,status,heading,bold,small);
        ImGui::End();ImGui::PopStyleColor();ImGui::PopStyleVar();
        ImGui::Render();SDL_SetRenderDrawColor(renderer,245,246,244,255);SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),renderer);SDL_RenderPresent(renderer);
    }
    if (SDL_SaveBMP(surface,output.string().c_str())!=0) throw std::runtime_error(SDL_GetError());
    ImGui_ImplSDLRenderer2_Shutdown();ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);
}
int main(int argc,char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--external-preview") {
            verify_external_preview();
            return 0;
        }
        const auto root=std::filesystem::path(argc>1?argv[1]:"build/faceswap-ui-qa");
        std::filesystem::create_directories(root);
        if (argc>2 && std::string(argv[2])=="--source-required") {
            render(root/"source-required-fr.bmp",770,720,3,false,false,0,0,5,false,"fr",0,false);
            return 0;
        }
        if(argc>2 && std::string(argv[2])=="--python-setup") {
            for(const auto* language:{"en","fr"})
                for(int state=1;state<=4;++state)
                    render(root/(std::string(language)+"-python-"+std::to_string(state)+".bmp"),
                        state==2 ? 770 : 1030,1000,3,false,state==1,0,2,5,state==2,language,state);
            std::cout<<"Python setup: missing runtime, installing, failure and ready states rendered\n";
            return 0;
        }
        if(argc>2 && std::string(argv[2])=="--preview-settings") {
            render(root/"preview-settings-fr.bmp",1030,900,3,true,false,0,0,93,false,"fr");
            return 0;
        }
        if (argc>2 && std::string(argv[2])=="--languages") {
            for (const auto* language : ap::i18n::codes) {
                const auto prefix=std::string(language);
                render(root/(prefix+"-settings.bmp"),1030,2500,-1,false,false,0,0,5,false,language);
                render(root/(prefix+"-settings-narrow.bmp"),770,3300,-1,false,false,0,0,5,true,language);
                render(root/(prefix+"-faceswap.bmp"),1030,2000,3,false,false,0,0,5,true,language);
                render(root/(prefix+"-faceswap-narrow.bmp"),770,3300,3,false,false,0,0,5,false,language);
            }
            std::cout<<ap::i18n::language_count<<" languages rendered in both themes and widths; translated glyphs checked\n";
            return 0;
        }
        render(root/"settings-dark.bmp",1030,2300,-1,false,false,0,0,5,true);
        render(root/"faceswap-dark.bmp",1030,1400,3,true,false,0,0,5,true);
        render(root/"wide-native.bmp",1030,1100,4,false,false);
        render(root/"wide-active.bmp",1030,1100,0,true,false);
        render(root/"narrow.bmp",770,1550,0,false,false);
        render(root/"error.bmp",1030,1100,1,true,true);
        render(root/"cpu.bmp",1030,1100,3,true,false,0,2);
        render(root/"builder-standard.bmp",1030,1400,3,false,false,0,0,3);
        render(root/"builder-optimized.bmp",1030,1400,3,false,false,0,0,5);
        render(root/"live-mask.bmp",1030,3300,3,true,false,0,0,93);
        render(root/"liveportrait.bmp",1030,3300,3,false,false,0,0,92);
        render(root/"liveportrait-narrow.bmp",770,3300,3,false,false,0,0,92);
        render(root/"detector-options.bmp",1030,3300,3,false,false,0,0,94);
        render(root/"mask-options.bmp",1030,3300,3,false,false,0,0,95);
        render(root/"mask-narrow.bmp",770,3200,0,false,false,0,0,95);
        render(root/"parser.bmp",1030,2100,3,false,false,0,0,96);
        render(root/"occlusion.bmp",1030,1900,3,false,false,0,0,97);
        render(root/"downloading.bmp",1030,1700,3,false,false,0,0,98);
        render(root/"preparing.bmp",1030,1400,3,true,false,0,0,99);
        render(root/"builder-running.bmp",1030,1400,3,true,false,0,0,5);
        render(root/"directml.bmp",1030,1250,3,false,false,0,1);
        render(root/"settings-wide.bmp",1030,2300,-1,false,false);
        render(root/"settings-narrow.bmp",770,3200,-2,false,false);
        render(root/"settings-recording.bmp",1030,2500,-1,true,true);
        render(root/"settings-scrolled.bmp",770,675,-1,false,false,5000);
        render(root/"scrolled.bmp",770,675,0,false,false,900);
        std::cout<<"Faceswap page rendered at wide/narrow sizes, active, error and scrolled states\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
