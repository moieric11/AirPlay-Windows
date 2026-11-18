#pragma once
#include "video/receiver_ui.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ap::video {

// UI state is independent of inference and transport. New controls belong here;
// the renderer owns engine actions and persistence.
struct FaceSwapSettingsUi {
    std::array<char,4096> source{}, models{}, python{}, directml_python{}, cpu_python{};
    int backend=0, directml_device=0;
    std::vector<std::string> adapters;
    bool enabled=false, gpu_resident=true, tensorrt=true, swapper_tensorrt=true;
    int model=0, quality=1, restoration=60, swapper_builder=5;
    int detector=0;
    int download_choice=0, accepted_model=-1;
    bool download_terms=false;
    int parser=0;
    int animation=0, animation_strength=100; // Face swap or LivePortrait reference animation.
    int landmarker=0; // Off or HRFFA 68 points, GPU-resident only.
    int shape_transfer=0; // Reserved legacy wire field; geometry calibration is retired.
    int shape_reset=0; // Reserved legacy wire field; ignored by active engines.
    int alignment=0; // Legacy or FaceFusion-style five-point alignment.
    int face_scale=100, face_size=100;
    int mask_expansion=0; // Final mask dilation, 0..12 percent of aligned crop.
    int mask_shape=0, mask_blur=18, mask_preview=0;
    std::array<int,4> mask_padding{};
    bool occlusion=false;
};
struct FaceSwapSettingsStatus {
    std::string message, log, preparation_stage;
    bool downloading=false;
    float download_fraction=-1;
    std::string download_message, download_directory, scrfd_directory;
    bool preparing=false;
    bool python_missing=false, python_setup_active=false, python_setup_failed=false, python_setup_ready=false;
    float python_setup_fraction=-1;
    std::string python_setup_message, python_setup_log;
    double preparation_seconds=0;
    bool failed=false, has_output=false, mirror_connected=false, gpu_decoding=false;
};
struct FaceSwapSettingsActions {
    bool start=false, stop=false, choose_photo=false, choose_models=false, mirror=false;
    int download_swapper=0, swapper_terms=0;
    bool cancel_download=false, download_occluder=false, download_parser=false;
    bool download_detector=false, detector_terms=false, download_landmarker=false, download_portrait=false;
    bool download_arcface=false, arcface_terms=false;
    bool mask_changed=false;
    bool prepare_python=false, cancel_python=false, open_python_log=false;
    bool show_preview=false;
};

inline FaceSwapSettingsActions draw_face_swap_settings(FaceSwapSettingsUi& ui,
        const FaceSwapSettingsStatus& status, ImFont* heading, ImFont* bold, ImFont* small) {
    namespace paint=receiver_ui;
    FaceSwapSettingsActions actions;
    auto hint=[&](const char* text) {
        ImGui::PushFont(small);
        ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(paint::muted));
        ImGui::TextWrapped("%s",text);
        ImGui::PopStyleColor(); ImGui::PopFont();
    };
    auto title=[&](const char* text) {
        ImGui::PushFont(bold); ImGui::TextUnformatted(text); ImGui::PopFont();
    };
    auto card_begin=[&](const char* id,const char* label) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,paint::rgba(paint::panel));
        ImGui::PushStyleColor(ImGuiCol_Border,ImGui::ColorConvertU32ToFloat4(paint::line));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,1);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(22,20));
        ImGui::BeginChild(id,ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|
            ImGuiChildFlags_AlwaysAutoResize|ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        title(label); ImGui::Dummy(ImVec2(0,8));
    };
    auto card_end=[&]() { ImGui::EndChild(); ImGui::PopStyleVar(3); ImGui::PopStyleColor(2); };
    auto field=[&](const char* label,const char* id,char* value,size_t size) {
        ImGui::TextUnformatted(label); ImGui::SetNextItemWidth(-1);
        ImGui::InputText(id,value,size);
    };
    const bool locked=(ui.enabled&&!status.failed)||status.downloading||status.python_setup_active;
    const float width=ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(12,10));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6);
    ImGui::SetCursorPosY(32);
    ImGui::PushFont(heading); ImGui::TextUnformatted(tr("Faceswap Settings")); ImGui::PopFont();
    const float back_width=std::max(164.f,ImGui::CalcTextSize(tr("Screen mirroring")).x+24);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-back_width);
    if (paint::text_button(tr("Screen mirroring"),ImVec2(back_width,34))) actions.mirror=true;
    hint(tr("Your source face, model and processing preferences in one place."));
    ImGui::Dummy(ImVec2(0,12));

    card_begin("##faceswap_status",tr("Faceswap engine"));
    if (ImGui::BeginTable("##faceswap_activation",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("State"),ImGuiTableColumnFlags_WidthStretch);
        const char* action_label=ui.enabled&&!status.failed ?
            (status.preparing ? tr("Cancel preparation") : tr("Stop faceswap")) :
            (status.failed ? tr("Retry faceswap") : tr("Enable faceswap"));
        const auto* preview_label=tr("Preview Settings");
        const auto* source_label=tr("Select a source photo");
        const bool source_missing=!ui.source[0];
        float action_width=std::max({158.f,ImGui::CalcTextSize(action_label).x+24,
            ImGui::CalcTextSize(preview_label).x+24});
        if (source_missing) action_width=std::max(action_width,ImGui::CalcTextSize(source_label).x+24);
        ImGui::TableSetupColumn(tr("Actions"),ImGuiTableColumnFlags_WidthFixed,action_width);
        ImGui::TableNextColumn();
        const char* state=status.failed ? tr("Engine needs attention") : !ui.enabled ? tr("Faceswap is off") :
            !status.mirror_connected ? tr("Waiting for screen mirroring") : status.has_output ? tr("Faceswap is running") : tr("Starting the engine...");
        ImGui::PushStyleColor(ImGuiCol_Text, paint::dark ?
            (status.failed ? ImVec4(1,.57f,.53f,1) : ui.enabled ? ImVec4(.48f,.82f,.63f,1) : ImVec4(.58f,.73f,.96f,1)) :
            (status.failed ? ImVec4(.72f,.27f,.22f,1) : ui.enabled ? ImVec4(.18f,.48f,.32f,1) : ImVec4(.23f,.36f,.55f,1)));
        ImGui::TextWrapped("%s",state); ImGui::PopStyleColor();
        if ((ui.enabled || status.failed) && !status.message.empty()) hint(status.message.c_str());
        else hint(tr("Applies to the largest detected face. Recordings keep the original video."));
        ImGui::TableNextColumn();
        if (ui.enabled&&!status.failed) {
            if (paint::text_button(status.preparing ? tr("Cancel preparation") : tr("Stop faceswap"),ImVec2(-1,38))) {ui.enabled=false;actions.stop=true;}
        } else {
            ImGui::BeginDisabled(source_missing || status.downloading || status.python_setup_active || (ui.animation==1 && (ui.backend!=0 || !ui.gpu_resident)));
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.14f,.40f,.92f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.12f,.34f,.82f,1));
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
            if (ImGui::Button(status.failed ? tr("Retry faceswap") : tr("Enable faceswap"),ImVec2(-1,38))) {
                ui.enabled=true; actions.start=true;
            }
            ImGui::PopStyleColor(3); ImGui::EndDisabled();
            if (source_missing) {
#if defined(_WIN32)
                ImGui::BeginDisabled(locked);
                if (paint::text_button(source_label,ImVec2(-1,34))) actions.choose_photo=true;
                ImGui::EndDisabled();
#else
                hint(source_label);
#endif
            }
            if (ui.enabled&&paint::text_button(tr("Turn off"),ImVec2(-1,30))) {ui.enabled=false;actions.stop=true;}
        }
        if (paint::text_button(preview_label,ImVec2(-1,34))) actions.show_preview=true;
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0,4));
    ImGui::BeginDisabled((ui.enabled&&!status.failed)||status.downloading);
    if (status.python_setup_active) {
        hint(tr(status.python_setup_message.c_str()));
        ImGui::ProgressBar(status.python_setup_fraction<0 ? -static_cast<float>(ImGui::GetTime()) : status.python_setup_fraction,
            ImVec2(-1,12),"");
        if (paint::text_button(tr("Cancel Python preparation"))) actions.cancel_python=true;
    } else {
        if (status.python_missing) hint(tr("Python environment is missing for the selected backend."));
        if (paint::text_button(tr("Prepare Python Environment"))) actions.prepare_python=true;
        hint(tr("Downloads Python and dependencies for the selected backend. NVIDIA may require several GB. Models are installed separately."));
        if (status.python_setup_failed) {
            ImGui::TextWrapped("%s",tr("Python preparation failed. Open the preparation log for details."));
            hint(tr(status.python_setup_message.c_str()));
        } else if (status.python_setup_ready) hint(tr("Python environment ready. You can retry the engine."));
    }
    if (!status.python_setup_log.empty() && paint::text_button(tr("Open Python preparation log"))) actions.open_python_log=true;
    ImGui::EndDisabled();
    if (ui.enabled && status.preparing && !status.failed) {
        ImGui::TextWrapped("%s",status.preparation_stage.c_str());
        if (status.preparation_stage.rfind("TensorRT: building engine",0)==0) {
            const ImVec4 warning=paint::dark ? ImVec4(1.f,.57f,.53f,1.f) : ImVec4(.72f,.20f,.17f,1.f);
            ImGui::PushStyleColor(ImGuiCol_Text,warning);
            ImGui::TextWrapped("%s",tr("Compiling TensorRT engines. This one-time step may take 5 to 20 minutes."));
            ImGui::PopStyleColor();
        }
        // Unknown work count: animated activity, never a fabricated percentage.
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()),ImVec2(-1,10),"");
        ImGui::Text(tr("Elapsed: %d:%02d"),static_cast<int>(status.preparation_seconds)/60,
                    static_cast<int>(status.preparation_seconds)%60);
        hint(tr("Preparing models and cache. TensorRT does not expose a percentage through this runtime. Cancel stops the engine."));
    }
    if (locked && !status.python_setup_active) hint(status.downloading ? tr("Finish or cancel the download to edit faceswap options.") : tr("Mask controls remain live. Stop faceswap to change the source, models or device."));
    card_end(); ImGui::Dummy(ImVec2(0,4));

    const char* detectors[]={tr("SCRFD 10G - 320px (default)"),"YOLOFace 8n - 640px","RetinaFace 10G - 320px","YuNet - 640px"};
    const char* swappers[]={"InSwapper 128", "AlphaFace 256", tr("HyperSwap 1A 256 (Recommended)"), tr("HyperSwap 1B 256 (Recommended)"), tr("HyperSwap 1C 256 (Recommended)")};
    card_begin("##faceswap_download",tr("Model management"));
    ImGui::BeginDisabled(locked);
    struct DownloadOption { const char* label; int choice; };
    enum { Detector=0, Landmarker=5, Portrait, Occluder, Parser, ArcFace };
    std::vector<DownloadOption> options{{detectors[ui.detector],Detector},{"ArcFace (w600k_r50.onnx)",ArcFace}};
    for(int i=1;i<IM_ARRAYSIZE(swappers);++i) options.push_back({swappers[i],i});
    if(ui.backend==0 && ui.gpu_resident) {
        options.push_back({tr("HRFFA - 68 points"),Landmarker});
        options.push_back({"LivePortrait",Portrait});
        options.push_back({"XSeg 1",Occluder});
        options.push_back({"BiSeNet",Parser});
    }
    const auto selected_option=std::find_if(options.begin(),options.end(),[&](const auto& option){return option.choice==ui.download_choice;});
    if(selected_option==options.end()) ui.download_choice=Detector;
    ImGui::TextUnformatted(tr("Model to download"));
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##faceswap_download_choice",selected_option==options.end() ? options.front().label : selected_option->label)) {
        for(const auto& option:options) {
            ImGui::PushID(option.choice);
            if(ImGui::Selectable(option.label,ui.download_choice==option.choice)) ui.download_choice=option.choice;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const bool detector=ui.download_choice==Detector;
    const bool arcface=ui.download_choice==ArcFace;
    const bool swapper=ui.download_choice>Detector && ui.download_choice<Landmarker;
    const int terms_key=detector ? ui.detector : swapper ? 4+ui.download_choice : arcface ? ArcFace : -1;
    if(ui.accepted_model!=terms_key) {
        ui.accepted_model=terms_key;
        ui.download_terms=false;
    }
    if(detector) {
        const char* notes[]={
            "Uses det_10g.onnx from the models folder. InsightFace weights are restricted to non-commercial research unless separately licensed.",
            "YOLOFace: GPL-3.0 per publisher metadata.",
            "RetinaFace: non-commercial research weights (InsightFace).",
            "YuNet: MIT per publisher metadata."
        };
        hint(tr(notes[ui.detector]));
        if(paint::text_button(tr("Detector terms"))) actions.detector_terms=true;
    } else if(arcface) {
        if(paint::text_button(tr("Model terms"))) actions.arcface_terms=true;
    } else if(swapper) {
        hint(ui.download_choice==1 ? tr("AlphaFace 256: Non-Commercial (FaceFusion ONNX export).") : tr("HyperSwap A/B/C: ResearchRAIL-MS. Review the publisher's terms before use."));
        if(paint::text_button(tr("Model terms"))) actions.swapper_terms=ui.download_choice;
    } else if(ui.download_choice==Landmarker) {
        hint(tr("HRFFA: MIT per FaceFusion publisher metadata. Source photo identity stays unchanged for this first comparison."));
    } else if(ui.download_choice==Portrait) {
        hint(tr("Four ONNX models, about 338 MB. LivePortrait: MIT per publisher metadata. Use a clear photo with open eyes and a neutral expression."));
    } else {
        hint(tr("XSeg 1: GPL-3.0. BiSeNet: MIT. Per publisher metadata; official FaceFusion model downloads."));
    }
    if(terms_key>=0) ImGui::Checkbox(tr("I have reviewed the model terms"),&ui.download_terms);
    ImGui::BeginDisabled((terms_key>=0 && !ui.download_terms) || status.python_missing);
    if(paint::text_button(tr("Download / Verify"))) {
        actions.download_detector=detector;
        actions.download_arcface=arcface;
        if(swapper) actions.download_swapper=ui.download_choice;
        actions.download_landmarker=ui.download_choice==Landmarker;
        actions.download_portrait=ui.download_choice==Portrait;
        actions.download_occluder=ui.download_choice==Occluder;
        actions.download_parser=ui.download_choice==Parser;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    const auto& destination=arcface || (detector && ui.detector==0) ? status.scrfd_directory : status.download_directory;
    if(!destination.empty()) ImGui::TextWrapped(tr("Destination: %s"),destination.c_str());
    if(!status.download_message.empty()) hint(status.download_message.c_str());
    if(status.downloading) {
        ImGui::ProgressBar(status.download_fraction<0 ? -static_cast<float>(ImGui::GetTime()) : status.download_fraction,ImVec2(-1,14));
        if(paint::text_button(tr("Cancel download"))) actions.cancel_download=true;
    }
    card_end(); ImGui::Dummy(ImVec2(0,4));

    const int columns=width>=820 ? 2 : 1;
    if (ImGui::BeginTable("##faceswap_configuration",columns,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        card_begin("##faceswap_source",tr("Source face"));
        hint(tr("Choose a photo with a clear face. The largest face in the photo is used."));
        ImGui::BeginDisabled(locked);
        field(tr("Source photo"),"##faceswap_photo",ui.source.data(),ui.source.size());
#if defined(_WIN32)
        if (paint::text_button(tr("Choose photo..."))) actions.choose_photo=true;
#endif
        ImGui::EndDisabled(); card_end();

        ImGui::TableNextColumn();
        card_begin("##faceswap_model",tr("Model & quality"));
        ImGui::BeginDisabled(locked);
        const char* engines[]={tr("Face swap"),tr("Reference animation - LivePortrait (experimental)")};
        ImGui::TextUnformatted(tr("Rendering engine"));ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##faceswap_rendering_engine",&ui.animation,engines,2);
        if(ui.animation==1) {
            hint(tr("Animates the selected photo from the current face pose and expression. Photo appearance and 3D reference points are cached; output is generated at 512px."));
            if(ui.backend!=0 || !ui.gpu_resident)hint(tr("Requires NVIDIA CUDA and Keep video on GPU."));
            hint(tr("Four ONNX models, about 338 MB. LivePortrait: MIT per publisher metadata. Use a clear photo with open eyes and a neutral expression."));
        } else {
            ImGui::TextUnformatted(tr("Swapper model")); ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##faceswap_model_choice",&ui.model,swappers,IM_ARRAYSIZE(swappers));
            if (ui.model==0) {
                const char* qualities[]={tr("Fast - no restoration"), tr("Balanced - GPEN 256"), tr("Detail - GPEN 512")};
                ImGui::TextUnformatted(tr("Quality")); ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##faceswap_quality",&ui.quality,qualities,3);
                hint(tr("Balanced adds detail with GPEN 256. Detail uses the larger GPEN 512 model."));
            } else hint(tr("Native 256-pixel face output without GPEN. Compare identity and expressions on your live stream."));
        }
        ImGui::EndDisabled(); card_end();
        ImGui::EndTable();
    }
    card_begin("##faceswap_detection",tr("Face detection"));
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##faceswap_detector",&ui.detector,detectors,IM_ARRAYSIZE(detectors));
    hint(tr("Finds faces and the five alignment points in the source photo and live video. Compare stability using the mask/landmark preview. Models use different input resolutions."));
    if(ui.backend==0 && ui.gpu_resident) {
        ImGui::Dummy(ImVec2(0,6));ImGui::TextUnformatted(tr("Face landmarker (experimental)"));
        const char* landmarkers[]={tr("Off - detector's 5 points"),tr("HRFFA - 68 points")};
        ImGui::SetNextItemWidth(-1);ImGui::Combo("##faceswap_landmarker",&ui.landmarker,landmarkers,2);
        hint(tr("Refines alignment from 68 landmarks. In LivePortrait, also drives head pose from rigid facial anchors. Follows the selected TensorRT level. Stop faceswap to change this model."));
    }
    ImGui::EndDisabled();
    card_end(); ImGui::Dummy(ImVec2(0,4));
    const bool restoration=ui.model==0 && ui.quality>0;
    if (ImGui::BeginTable("##faceswap_processing",restoration ? columns : 1,ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        card_begin("##faceswap_acceleration",tr("Processing device"));
        ImGui::BeginDisabled(locked);
        const char* backends[]={"NVIDIA CUDA","DirectML (AMD / Intel / NVIDIA)","CPU"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##faceswap_backend",&ui.backend,backends,3);
        if (ui.backend==1) {
            hint(tr("Uses DirectX 12 for model inference. Video pixels pass through RAM; CUDA and TensorRT are bypassed."));
            ImGui::TextUnformatted(tr("DirectML graphics adapter")); ImGui::SetNextItemWidth(-1);
            const char* preview=ui.directml_device>=0 && ui.directml_device<static_cast<int>(ui.adapters.size()) ? ui.adapters[ui.directml_device].c_str() : tr("Choose an available adapter");
            if (ImGui::BeginCombo("##faceswap_dml_adapter",preview)) {
                for (int i=0;i<static_cast<int>(ui.adapters.size());++i)
                    if (ImGui::Selectable(ui.adapters[i].c_str(),ui.directml_device==i)) ui.directml_device=i;
                ImGui::EndCombo();
            }
        }
        if (ui.backend==2) hint(tr("Runs face detection, swapping and restoration on the CPU. Frame rate may be low; start without restoration."));
        if (ui.backend==0) {
        ImGui::Checkbox(tr("Keep video on GPU"),&ui.gpu_resident);
        hint(tr("Keeps video pixels on the graphics card through processing and display."));
        if (ui.gpu_resident) {
            ImGui::EndDisabled();
            ImGui::BeginDisabled(status.downloading);
            const std::array<int,14> before={ui.animation_strength,ui.shape_transfer,ui.shape_reset,ui.mask_expansion,ui.alignment,ui.face_size,ui.face_scale,ui.mask_shape,ui.mask_blur,ui.mask_preview,
                ui.mask_padding[0],ui.mask_padding[1],ui.mask_padding[2],ui.mask_padding[3]};
            title(tr("Blending mask"));
            hint(tr("Changes apply live and refresh the preview even on a still image."));
            const char* alignments[]={tr("Current (InsightFace)"),tr("FaceFusion-style (5 alignment points)")};
            ImGui::TextUnformatted(tr("Face alignment"));ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(ui.animation==1);
            ImGui::Combo("##faceswap_alignment",&ui.alignment,alignments,2);ImGui::EndDisabled();
            if(ui.animation==1)hint(tr("LivePortrait uses its own FFHQ crop alignment."));
            else hint(tr("Compares crop estimation and image borders. Uses refined points when HRFFA is enabled; this is not the full FaceFusion pipeline."));
            if(ui.animation==1) {
                ImGui::TextUnformatted(tr("Animation strength"));ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##portrait_strength",&ui.animation_strength,0,100,"%d%%");
                hint(tr("Direct tracking: HRFFA drives head pose when enabled; LivePortrait follows the current expression. No calibration is required. 0% freezes the reference expression."));
            }
            ImGui::TextUnformatted(tr("Mask expansion"));ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##faceswap_mask_expansion",&ui.mask_expansion,0,12,"%d%%");
            hint(tr("Extends the final mask without resizing facial features. Try 3-5%. Preserved eyes/mouth stay protected; high values also reduce XSeg protection at object edges."));
            ImGui::TextUnformatted(tr("Face size (global)"));ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##faceswap_face_size",&ui.face_size,80,120,"%d%%");
            hint(tr("Resizes the whole generated face and its blending mask together around the aligned face centre."));
            if(paint::text_button(tr("Reset face size")))ui.face_size=100;
            ImGui::TextUnformatted(tr("Face scale"));ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##faceswap_face_scale",&ui.face_scale,80,120,"%d%%");
            hint(tr("100% keeps the original size. Try 102-105% to enlarge the generated face inside the same mask. Eyes and mouth also move."));
            if(paint::text_button(tr("Reset face scale")))ui.face_scale=100;
            const char* shapes[]={tr("Ellipse"),tr("Box")};
            ImGui::TextUnformatted(tr("Mask shape"));ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##faceswap_mask_shape",&ui.mask_shape,shapes,2);
            const char* edges[]={tr("Top"),tr("Right"),tr("Bottom"),tr("Left")};
            for(int i=0;i<4;++i) {
                ImGui::PushID(i);ImGui::TextUnformatted(edges[i]);ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##mask_padding",&ui.mask_padding[i],0,45,"%d%%");ImGui::PopID();
            }
            hint(tr("Higher margins reduce the replaced area on that side; they do not resize generated facial features."));
            ImGui::TextUnformatted(tr("Edge softness"));ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##faceswap_mask_blur",&ui.mask_blur,0,50,"%d%%");
            const char* previews[]={tr("Off"),tr("Mask overlay + alignment points"),tr("Mask only + alignment points")};
            ImGui::TextUnformatted(tr("Preview in screen mirroring"));ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##faceswap_mask_preview",&ui.mask_preview,previews,3);
            hint(tr("Green overlay shows the effective swap mask after XSeg/BiSeNet. Red points mark the five alignment points; cyan points show HRFFA landmarks when accepted. Preview changes the displayed video."));
            if(paint::text_button(tr("Reset blending mask"))) {ui.mask_shape=0;ui.mask_padding={};ui.mask_blur=18;ui.mask_expansion=0;ui.mask_preview=0;}
            const std::array<int,14> after={ui.animation_strength,ui.shape_transfer,ui.shape_reset,ui.mask_expansion,ui.alignment,ui.face_size,ui.face_scale,ui.mask_shape,ui.mask_blur,ui.mask_preview,
                ui.mask_padding[0],ui.mask_padding[1],ui.mask_padding[2],ui.mask_padding[3]};
            actions.mask_changed=before!=after;
            ImGui::EndDisabled();
            ImGui::BeginDisabled(locked);
            ImGui::Dummy(ImVec2(0,4));
            ImGui::TextUnformatted(tr("Face regions - BiSeNet ResNet18 (experimental)"));ImGui::SetNextItemWidth(-1);
            const char* regions[]={tr("Off"),tr("Face regions"),tr("Preserve original eyes"),tr("Preserve original mouth"),tr("Preserve original eyes and mouth")};
            ImGui::Combo("##faceswap_regions",&ui.parser,regions,5);
            hint(tr("Uses the original face's regions to control blending. Preserved regions retain the original person's appearance. Adds GPU processing."));
            ImGui::Checkbox(tr("Occlusion protection - XSeg (experimental)"),&ui.occlusion);
            hint(tr("Preserves hands and objects in front of the face. Adds GPU processing; disabled by default."));
            if (ui.animation==1)hint(tr("LivePortrait models follow the shared TensorRT compilation level."));
            else if (ui.model>0) ImGui::Checkbox(tr("TensorRT swapper"),&ui.swapper_tensorrt);
            else if (ui.quality>0) ImGui::Checkbox(tr("TensorRT restoration"),&ui.tensorrt);
            {
                ImGui::TextUnformatted(tr("TensorRT compilation (all models)")); ImGui::SetNextItemWidth(-1);
                const char* levels[]={tr("Level 5 (slower first build, faster processing)"),tr("Level 3 (faster first build, slower processing)")};
                int selected=ui.swapper_builder==3 ? 1 : 0;
                if (ImGui::Combo("##faceswap_swapper_builder",&selected,levels,2))
                    ui.swapper_builder=selected==1 ? 3 : 5;
                hint(tr("Level 5 is the default. Actual speed depends on the selected model and GPU."));
                hint(tr("Applies to every active TensorRT model: swapper, face detector, XSeg, BiSeNet and GPEN. Each model and level has its own cache."));
            }
            if (ui.model==0 && ui.quality>0 && ui.tensorrt)
                hint(tr("Uses cached NVIDIA engines when available. The first build can take several minutes."));
        }
        }
        ImGui::EndDisabled();
        if (ui.backend==0 && !status.gpu_decoding)
            hint(tr("Enable GPU video decoding in Settings, then reconnect your device, to keep video on GPU."));
        else if (ui.backend==0) hint(tr("GPU video decoding is enabled."));
        card_end();

        if (restoration) {
            ImGui::TableNextColumn();
            card_begin("##faceswap_restoration",tr("Face restoration"));
            ImGui::BeginDisabled(locked);
            ImGui::TextUnformatted(tr("Restoration strength")); ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##faceswap_strength",&ui.restoration,0,100,"%d%%");
            ImGui::EndDisabled();
            hint(tr("Blends restored details into the swapped face. Higher strength can change fine facial features."));
            hint(tr("Strength changes the blend, not the model's processing cost."));
            card_end();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0,4));
    card_begin("##faceswap_runtime",tr("Engine & diagnostics"));
    ImGui::TextWrapped("%s",status.message.c_str());
    if (!status.log.empty() && paint::text_button(tr("Copy engine log path"))) ImGui::SetClipboardText(status.log.c_str());
    ImGui::Dummy(ImVec2(0,4)); ImGui::Separator(); ImGui::Dummy(ImVec2(0,4));
    ImGui::BeginDisabled(locked);
    field(tr("Models folder"),"##faceswap_models_folder",ui.models.data(),ui.models.size());
#if defined(_WIN32)
    if (paint::text_button(tr("Choose models folder..."))) actions.choose_models=true;
#endif
    if (ui.backend==1) field(tr("DirectML Python executable"),"##faceswap_dml_python",ui.directml_python.data(),ui.directml_python.size());
    else if (ui.backend==2) field(tr("CPU Python executable"),"##faceswap_cpu_python",ui.cpu_python.data(),ui.cpu_python.size());
    else field(tr("Python executable"),"##faceswap_python",ui.python.data(),ui.python.size());
    ImGui::EndDisabled();
    hint(tr("The app starts and stops the selected engine automatically. Preferences are saved automatically."));
    card_end(); ImGui::Dummy(ImVec2(0,28));
    ImGui::PopStyleVar(2);
    return actions;
}
} // namespace ap::video
