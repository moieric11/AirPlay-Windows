#pragma once
#include "video/receiver_ui.h"
#include "video/video_recorder.h"
#include "airplay/live_settings.h"
#include "airplay/receiver_name.h"
#include <array>
#include <functional>
#include <cstdio>

namespace ap::video {
struct StandardSettingsContext {
    ap::airplay::LiveSettings* live;
    std::array<char,51>& receiver_name;
    std::array<char,1024>& recording_directory;
    VideoRecorder::Options& recording_options;
    VideoRecorder::Status recording;
    bool& diagnostics;
    bool& custom_resolution;
    int video_width, video_height;
    double reset_notice_until;
    std::function<bool(std::string&)> browse_folder;
    std::function<void()> toggle_recording, reset_all;
    std::function<void(const char*)> set_language;
};

// Same card style and responsive columns as Faceswap Settings.
inline bool draw_standard_settings(StandardSettingsContext& context, ImFont* heading, ImFont* bold, ImFont* small) {
    namespace ui_draw=receiver_ui;
    auto* live_settings_=context.live;
    auto& receiver_name_buf=context.receiver_name;
    auto& recording_dir=context.recording_directory;
    auto& recording_options=context.recording_options;
    auto& custom_resolution=context.custom_resolution;
    const int video_tex_w=context.video_width, video_tex_h=context.video_height;
    const double settings_reset_notice_until=context.reset_notice_until;
    auto& toggle_recording=context.toggle_recording;
    auto& reset_all_settings=context.reset_all;
    const char* kRotationModes[]={tr("Automatic (device)"),tr("Manual: 0 degrees"),tr("Manual: 90 degrees"),tr("Manual: 180 degrees"),tr("Manual: 270 degrees")};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(12,10));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6);
    ImGui::SetCursorPosY(32);
    ImGui::PushFont(heading); ImGui::TextUnformatted(tr("Settings")); ImGui::PopFont();
    const float back_width=std::max(164.f,ImGui::CalcTextSize(tr("Screen mirroring")).x+24);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-back_width);
    const bool back=ui_draw::text_button(tr("Screen mirroring"),ImVec2(back_width,34));
    ImGui::PushFont(small); ImGui::TextWrapped(tr("Receiver, video and recording preferences. Changes are saved automatically.")); ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,12));
    const int columns=ImGui::GetContentRegionAvail().x>=820 ? 2 : 1;
    if (ImGui::BeginTable("##standard_settings_cards",columns,ImGuiTableFlags_SizingStretchSame)) {
        // One table row supplies column widths/clipping. Each column has its
        // own cursor so a tall card does not leave holes in the other column.
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        const float top=ImGui::GetCursorPosY();
        std::array<float,2> next_y{top,top};
        int column=0;
        bool card_open=false;
        auto card_end=[&]() {
            if (!card_open) return;
            ImGui::PopTextWrapPos(); ImGui::PopItemWidth();
            ImGui::EndChild(); ImGui::PopStyleVar(3); ImGui::PopStyleColor(2);
            next_y[column]=ImGui::GetCursorPosY()+6;
            card_open=false;
        };
        auto settings_section=[&](const char* label, ImGuiTreeNodeFlags = 0) {
            card_end();
            column=columns==2 && next_y[1]<next_y[0] ? 1 : 0;
            ImGui::TableSetColumnIndex(column);
            ImGui::SetCursorPosY(next_y[column]);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,ui_draw::rgba(ui_draw::panel));
            ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(ui_draw::line));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,1);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(22,20));
            ImGui::BeginChild(label,ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysAutoResize|ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PushFont(bold); ImGui::TextUnformatted(tr(label)); ImGui::PopFont();
            ImGui::Dummy(ImVec2(0,8)); ImGui::PushTextWrapPos(0); ImGui::PushItemWidth(-1);
            card_open=true; return true;
        };
        if (settings_section("Language")) {
            int selected = static_cast<int>(ap::i18n::current_language);
            ImGui::TextUnformatted(tr("Interface language"));
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!context.set_language);
            if (ImGui::Combo("##interface_language", &selected, ap::i18n::names, IM_ARRAYSIZE(ap::i18n::names)))
                context.set_language(ap::i18n::codes[selected]);
            ImGui::EndDisabled();
            ImGui::PushFont(small);
            ImGui::TextWrapped("%s", tr("Changes apply immediately and are saved automatically."));
            ImGui::PopFont();
        }
        if (live_settings_ && settings_section("Receiver name",
                ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-1.0f);
            const bool enter = ImGui::InputText("##receiver_name",
                receiver_name_buf.data(), receiver_name_buf.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            const auto name = ap::airplay::normalize_receiver_name(receiver_name_buf.data());
            const bool changed = name != live_settings_->receiver_name();
            ImGui::BeginDisabled(name.empty() || !changed);
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.14f,.40f,.92f,1));
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
            const bool apply = ImGui::Button(tr("Apply name"));
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if ((apply || enter) && !name.empty() && changed) {
                live_settings_->set_receiver_name(name);
                std::snprintf(receiver_name_buf.data(), receiver_name_buf.size(), "%s", name.c_str());
            }
            if (name.empty())
                ImGui::TextDisabled(tr("Use a name without dots or backslashes (max. 50 UTF-8 bytes)."));
            else
                ImGui::TextDisabled(tr("Shown in AirPlay on your devices. Saved automatically."));
        }
        if (settings_section("Mirror resolution",
                ImGuiTreeNodeFlags_DefaultOpen)) {
            if (live_settings_) {
                struct Preset { const char* label; int w; int h; };
                const Preset presets[] = {
                    {"1920x1080  (Full HD)",      1920, 1080},
                    {tr("2560x1440  (QHD, default)"), 2560, 1440},
                    {tr("1920x1920  (square limit)"),1920, 1920},
                    {tr("2560x2560  (square limit)"),2560, 2560},
                    {tr("2868x2868  (square limit)"),2868, 2868},
                    {tr("3840x3840  (square limit)"),3840, 3840},
                    {"3840x2160  (4K UHD)",       3840, 2160},
                };
                int cur_w = live_settings_->mirror_width.load();
                int cur_h = live_settings_->mirror_height.load();
                int sel = -1;   // -1 = none of the presets match
                for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                    if (presets[i].w == cur_w && presets[i].h == cur_h) {
                        sel = i; break;
                    }
                }
                // Custom mode is sticky once chosen so the
                // inputs don't disappear the moment the user
                // pauses on a partial value that happens to
                // match a preset (eg typing 1920 then 1080).
                // Reset to false whenever a preset is chosen
                // explicitly, or auto-set when current values
                // don't match any preset (eg --mirror-res CLI).
                if (sel < 0) custom_resolution = true;

                char preview_buf[48];
                const char* preview;
                if (custom_resolution) {
                    std::snprintf(preview_buf, sizeof(preview_buf),
                                  tr("Custom: %dx%d"), cur_w, cur_h);
                    preview = preview_buf;
                } else {
                    preview = presets[sel].label;
                }

                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##mirror_res", preview)) {
                    for (int i = 0; i < (int)IM_ARRAYSIZE(presets); ++i) {
                        const bool is_sel =
                            (!custom_resolution && sel == i);
                        if (ImGui::Selectable(presets[i].label,
                                              is_sel)) {
                            custom_resolution = false;
                            live_settings_->mirror_width.store(presets[i].w);
                            live_settings_->mirror_height.store(presets[i].h);
                        }
                        if (is_sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::Separator();
                    if (ImGui::Selectable(tr("Custom..."), custom_resolution)) {
                        custom_resolution = true;
                    }
                    ImGui::EndCombo();
                }

                if (custom_resolution) {
                    // Width / height inputs that commit live
                    // (no Enter) as soon as the value falls in
                    // the accepted range. While the user is
                    // typing a partial number outside the
                    // range nothing is written — so 2 / 25 /
                    // 256 don't get applied on the way to 2560.
                    static int  width_buf    = 0;
                    static int  height_buf   = 0;
                    static bool width_active = false;
                    static bool height_active= false;
                    if (!width_active)  width_buf  = cur_w;
                    if (!height_active) height_buf = cur_h;

                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt(tr("Width##custom"),
                                    &width_buf, 0, 0);
                    width_active = ImGui::IsItemActive();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt(tr("Height##custom"),
                                    &height_buf, 0, 0);
                    height_active = ImGui::IsItemActive();

                    if (width_buf  >= 320 && width_buf  <= 7680 &&
                        width_buf  != cur_w) {
                        live_settings_->mirror_width.store(width_buf);
                    }
                    if (height_buf >= 240 && height_buf <= 4320 &&
                        height_buf != cur_h) {
                        live_settings_->mirror_height.store(height_buf);
                    }
                    ImGui::TextDisabled(
                        tr("Range: 320-7680 x 240-4320"));
                }
                ImGui::TextDisabled(
                    tr("Maximum video size. Your device chooses the actual stream resolution."));
                if (video_tex_w > 0 && video_tex_h > 0)
                    ImGui::TextDisabled(tr("Received: %d x %d"), video_tex_w, video_tex_h);
                ImGui::TextDisabled(tr("Applies on next device connection"));
            } else {
                ImGui::TextDisabled(tr("(LiveSettings not wired)"));
            }
        }
        if (settings_section("Video decoder",
                ImGuiTreeNodeFlags_DefaultOpen)) {
            if (live_settings_) {
                bool hw = live_settings_->mirror_hwaccel.load();
                if (ImGui::Checkbox(tr("Use GPU acceleration"),
                                    &hw)) {
                    live_settings_->mirror_hwaccel.store(hw);
                }
                ImGui::TextDisabled(
                    hw
                      ? tr("Uses your graphics card when available.")
                      : tr("Uses your processor to decode video."));
                ImGui::TextDisabled(tr("Applies on next device connection"));

                ImGui::Separator();

                bool vsync = live_settings_->vsync_enabled.load();
                if (ImGui::Checkbox(tr("Synchronize with display"), &vsync)) {
                    live_settings_->vsync_enabled.store(vsync);
                }
                ImGui::TextDisabled(
                    vsync
                      ? tr("Sync presents to monitor refresh.")
                      : tr("Present immediately (lower latency,"
                        " visible tearing on fast pans)."));
                ImGui::TextDisabled(tr("Applies live (no reconnect needed)"));
            } else {
                ImGui::TextDisabled(tr("(LiveSettings not wired)"));
            }
        }
        if (live_settings_ && settings_section("Experimental playback", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool hls = live_settings_->hls_playback.load();
            if (ImGui::Checkbox(tr("Direct video playback (HLS)"), &hls))
                live_settings_->hls_playback.store(hls);
            ImGui::TextWrapped(tr("Play compatible videos directly, including YouTube."));
            ImGui::TextDisabled(tr("Disabled by default. Restart the app to apply."));
            if (hls != live_settings_->hls_playback_active)
                ImGui::TextDisabled(live_settings_->hls_playback_active
                    ? tr("Currently enabled until the app restarts.")
                    : tr("Will be enabled after the app restarts."));
        }
        if (settings_section("Recording")) {
            const auto& rec = context.recording;
            ImGui::TextUnformatted(tr("Folder"));
        #if defined(_WIN32)
            const float browse_width=ImGui::CalcTextSize(tr("Browse...")).x+ImGui::GetStyle().FramePadding.x*2;
            ImGui::SetNextItemWidth(-browse_width-ImGui::GetStyle().ItemSpacing.x);
        #endif
            ImGui::InputText("##recording_folder", recording_dir.data(),
                             recording_dir.size());
        #if defined(_WIN32)
            ImGui::SameLine();
            if (ImGui::Button(tr("Browse..."), ImVec2(browse_width, 0.0f))) {
                std::string selected_folder;
                if (context.browse_folder(selected_folder)) {
                    std::snprintf(recording_dir.data(),
                                  recording_dir.size(), "%s",
                                  selected_folder.c_str());
                }
            }
        #endif
            const char* encoder_modes[] = {
                tr("Auto (GPU preferred)"), tr("GPU only"), tr("CPU only")
            };
            const char* video_codecs[] = {
                tr("H.264 (compatible)"), tr("H.265 / HEVC (smaller)")
            };
            int video_codec =
                static_cast<int>(recording_options.codec);
            ImGui::TextUnformatted(tr("Video codec")); ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##recording_codec", &video_codec,
                             video_codecs, 2)) {
                recording_options.codec =
                    static_cast<VideoRecorder::VideoCodec>(video_codec);
            }
            int encoder_mode =
                static_cast<int>(recording_options.encoder);
            ImGui::TextUnformatted(tr("Encoder")); ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##recording_encoder", &encoder_mode,
                             encoder_modes, 3)) {
                recording_options.encoder =
                    static_cast<VideoRecorder::EncoderMode>(encoder_mode);
            }
            ImGui::TextUnformatted(tr("Bitrate (Mbps)")); ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##recording_bitrate",
                             &recording_options.bitrate_mbps, 2, 40);
            ImGui::TextDisabled(
                tr("Lower bitrate = smaller file / more compression"));
            if (rec.recording)
                ImGui::TextDisabled(tr("Settings apply to the next recording"));
            if (rec.recording) {
                const int64_t seconds = rec.elapsed_ms / 1000;
                ImGui::TextColored(ImVec4(1.0f, 0.30f, 0.32f, 1.0f),
                                   tr("REC  %02lld:%02lld"),
                                   static_cast<long long>(seconds / 60),
                                   static_cast<long long>(seconds % 60));
                ImGui::Text(tr("Frames: %llu"),
                    static_cast<unsigned long long>(rec.frames));
                ImGui::Text(tr("Audio: %s"),
                    rec.audio_samples > 0 ? tr("AAC stereo") : tr("waiting for PCM"));
                if (rec.dropped > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f),
                        tr("Queue drops: %llu"),
                        static_cast<unsigned long long>(rec.dropped));
                }
                if (!rec.codec.empty())
                    ImGui::TextWrapped(tr("Codec: %s"), rec.codec.c_str());
                if (ImGui::Button(tr("Stop recording"), ImVec2(-1, 0)))
                    toggle_recording();
            } else {
                ImGui::TextDisabled(tr("MP4 video (decoded AirPlay stream)"));
                if (ImGui::Button(tr("Start recording"), ImVec2(-1, 0)))
                    toggle_recording();
            }
            if (!rec.output_path.empty())
                ImGui::TextWrapped(tr("File: %s"), rec.output_path.c_str());
            if (!rec.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                                   "%s", rec.error.c_str());
        }
        if (settings_section("Diagnostics")) {
            ImGui::Checkbox(tr("Show diagnostics"), &context.diagnostics);
        }
        if (settings_section("Rotation")) {
            if (live_settings_) {
                int selected = live_settings_->video_rotation.load() + 1;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##video_rotation", &selected, kRotationModes, IM_ARRAYSIZE(kRotationModes)))
                    live_settings_->video_rotation.store(selected - 1);
                ImGui::TextDisabled(
                    tr("Auto follows the orientation sent by the device."
                    " Manual angles rotate clockwise from the received image."));
                if (video_tex_w > 0 && video_tex_h > 0)
                    ImGui::TextDisabled(tr("Received: %s (%d x %d)"),
                        video_tex_w > video_tex_h ? tr("Landscape") :
                        video_tex_w < video_tex_h ? tr("Portrait") : tr("Square"),
                        video_tex_w, video_tex_h);
                ImGui::TextDisabled(tr("Applies live to display only; recording is unchanged."));
            }
        }
        if (settings_section("Frame rate")) {
            if (live_settings_) {
                // iOS effectively takes the min of maxFPS and
                // refreshRate, so two separate sliders meant
                // the same thing twice. Drive both atomics
                // from a single "Frame rate cap" knob and
                // keep the underlying fields in lockstep.
                int cap = live_settings_->max_fps.load();
                ImGui::TextUnformatted(tr("Frame rate cap")); ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##frame_rate_cap",
                                     &cap, 1, 60, tr("%d fps"))) {
                    live_settings_->max_fps.store(cap);
                    live_settings_->refresh_rate.store(cap);
                }
                ImGui::TextDisabled(
                    tr("iOS caps mirror at 60 fps regardless of hint;"
                    " lower values force a deterministic cadence."));
                ImGui::TextDisabled(tr("Applies on next device connection"));
            } else {
                ImGui::TextDisabled(tr("(LiveSettings not wired)"));
            }
        }
        if (settings_section("Compression")) {
            if (live_settings_) {
                bool hevc = live_settings_->hevc_enabled.load();
                if (ImGui::Checkbox(tr("Allow HEVC (H.265)"), &hevc)) {
                    live_settings_->hevc_enabled.store(hevc);
                }
                ImGui::TextDisabled(
                    hevc
                      ? tr("The device may pick HEVC at high resolution")
                      : tr("Force H.264 (more CPU, more bandwidth)"));
                ImGui::TextDisabled(tr("Applies on next device connection"));
            } else {
                ImGui::TextDisabled(tr("(LiveSettings not wired)"));
            }
        }
        if (settings_section("Network")) {
            ImGui::TextDisabled("RTSP   :7000");
            ImGui::TextDisabled("HLS    :7100");
            ImGui::TextDisabled(tr("mDNS   advertising"));
        }
        if (settings_section("Keyboard shortcuts")) {
            ImGui::BulletText(tr("F / F11 / double-click  Toggle fullscreen"));
            ImGui::BulletText(tr("H       Hide / show the interface"));
            ImGui::BulletText(tr("R       Start / stop recording"));
            ImGui::BulletText(tr("Esc     Exit fullscreen / show UI / quit"));
        }
        settings_section("Restore defaults");
        ImGui::TextDisabled(tr("Restore all app preferences to their defaults."));
        ImGui::BeginDisabled(!live_settings_);
        if (ui_draw::text_button(tr("Reset all settings"),ImVec2(-1,36))) reset_all_settings();
        ImGui::EndDisabled();
        if (ImGui::GetTime() < settings_reset_notice_until)
            ImGui::TextDisabled(tr("Default settings restored."));
        card_end(); ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0,28)); ImGui::PopStyleVar(2);
    return back;
}
} // namespace ap::video
