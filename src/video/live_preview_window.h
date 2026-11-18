#pragma once

#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace ap::video {

// SDL textures belong to their renderer. Copy a reduced frame only while the
// preview is open so the main video pipeline can keep its usual GPU path.
class LivePreviewWindow {
public:
    using Clock = std::chrono::steady_clock;

    LivePreviewWindow() = default;
    LivePreviewWindow(const LivePreviewWindow&) = delete;
    LivePreviewWindow& operator=(const LivePreviewWindow&) = delete;
    ~LivePreviewWindow() { close(); }

    bool show(SDL_Window* main_window, const char* title) {
        if (!window_) {
            const int display=std::max(0,SDL_GetWindowDisplayIndex(main_window));
            window_=SDL_CreateWindow(title,SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                SDL_WINDOWPOS_CENTERED_DISPLAY(display),420,480,
                SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window_) return false;
            SDL_SetWindowMinimumSize(window_,180,140);
            renderer_=SDL_CreateRenderer(window_,-1,0);
            if (!renderer_) {
                const std::string error=SDL_GetError();
                close();
                SDL_SetError("%s",error.c_str());
                return false;
            }
            title_=title;
        }
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window_);
        SDL_RaiseWindow(window_);
        return true;
    }

    bool handle_event(const SDL_Event& event) {
        if (!window_) return false;
        Uint32 window_id=0;
        switch (event.type) {
        case SDL_WINDOWEVENT: window_id=event.window.windowID; break;
        case SDL_KEYDOWN: case SDL_KEYUP: window_id=event.key.windowID; break;
        case SDL_TEXTINPUT: window_id=event.text.windowID; break;
        case SDL_TEXTEDITING: window_id=event.edit.windowID; break;
        case SDL_MOUSEMOTION: window_id=event.motion.windowID; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: window_id=event.button.windowID; break;
        case SDL_MOUSEWHEEL: window_id=event.wheel.windowID; break;
        default: return false;
        }
        if (window_id!=SDL_GetWindowID(window_)) return false;
        if ((event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_CLOSE) ||
            (event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE)) close();
        return true;
    }

    void draw(SDL_Renderer* source_renderer, SDL_Texture* source, int width, int height,
              int rotation, Clock::time_point frame_time, const char* title, const char* waiting) {
        if (!window_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return;
        const auto now=Clock::now();
        if (now<next_draw_) return;
        next_draw_=now+std::chrono::milliseconds(33);

        if (!source) frame_ready_=false;
        else if (!frame_ready_ || frame_time!=copied_frame_) {
            frame_ready_=copy_frame(source_renderer,source,width,height);
            if (frame_ready_) copied_frame_=frame_time;
        }
        const std::string caption=frame_ready_ ? title : std::string(title)+" — "+waiting;
        if (caption!=title_) {
            SDL_SetWindowTitle(window_,caption.c_str());
            title_=caption;
        }

        SDL_SetRenderDrawColor(renderer_,0,0,0,255);
        SDL_RenderClear(renderer_);
        if (frame_ready_) {
            int output_w=0, output_h=0;
            SDL_GetRendererOutputSize(renderer_,&output_w,&output_h);
            const bool sideways=rotation%180!=0;
            const float scale=std::min(static_cast<float>(output_w)/(sideways ? height_ : width_),
                static_cast<float>(output_h)/(sideways ? width_ : height_));
            const int draw_w=static_cast<int>(std::lround(width_*scale));
            const int draw_h=static_cast<int>(std::lround(height_*scale));
            const SDL_Rect destination{(output_w-draw_w)/2,(output_h-draw_h)/2,draw_w,draw_h};
            SDL_RenderCopyEx(renderer_,frame_,nullptr,&destination,rotation,nullptr,SDL_FLIP_NONE);
        }
        SDL_RenderPresent(renderer_);
    }

    void close() {
        if (scratch_) SDL_DestroyTexture(scratch_);
        if (renderer_) SDL_DestroyRenderer(renderer_); // Also destroys frame_.
        if (window_) SDL_DestroyWindow(window_);
        scratch_=nullptr;
        frame_=nullptr;
        renderer_=nullptr;
        window_=nullptr;
        frame_ready_=false;
        next_draw_={};
    }

private:
    bool copy_frame(SDL_Renderer* source_renderer,SDL_Texture* source,int width,int height) {
        constexpr int max_edge=960;
        if (width<=0 || height<=0) return false;
        const float scale=std::min(1.f,static_cast<float>(max_edge)/std::max(width,height));
        const int w=std::max(1,static_cast<int>(std::lround(width*scale)));
        const int h=std::max(1,static_cast<int>(std::lround(height*scale)));
        if (!scratch_ || !frame_ || width_!=w || height_!=h) {
            if (scratch_) SDL_DestroyTexture(scratch_);
            if (frame_) SDL_DestroyTexture(frame_);
            scratch_=SDL_CreateTexture(source_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,w,h);
            frame_=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
            width_=w;
            height_=h;
        }
        if (!scratch_ || !frame_) return false;
        auto* previous_target=SDL_GetRenderTarget(source_renderer);
        if (SDL_SetRenderTarget(source_renderer,scratch_)!=0) return false;
        bool copied=false;
        if (SDL_RenderCopy(source_renderer,source,nullptr,nullptr)==0) {
            void* pixels=nullptr;
            int pitch=0;
            if (SDL_LockTexture(frame_,nullptr,&pixels,&pitch)==0) {
                copied=SDL_RenderReadPixels(source_renderer,nullptr,SDL_PIXELFORMAT_ARGB8888,pixels,pitch)==0;
                SDL_UnlockTexture(frame_);
            }
        }
        SDL_SetRenderTarget(source_renderer,previous_target);
        return copied;
    }

    SDL_Window* window_=nullptr;
    SDL_Renderer* renderer_=nullptr;
    SDL_Texture* frame_=nullptr;
    SDL_Texture* scratch_=nullptr;
    int width_=0, height_=0;
    bool frame_ready_=false;
    Clock::time_point next_draw_{}, copied_frame_{};
    std::string title_;
};

} // namespace ap::video
