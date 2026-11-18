#pragma once

#include "imgui.h"
#include "ui/i18n.h"

namespace ap::video { using ap::i18n::tr; }
#include <algorithm>
#include <cmath>

// Vector controls use the same renderer as the video; no browser or bitmap icons.
namespace ap::video::receiver_ui {
inline bool dark = false;
inline ImU32 ink, muted, blue, line, paper, panel;
inline ImU32 themed(ImU32 light, ImU32 night) { return dark ? night : light; }
inline ImVec4 rgba(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }
inline void apply_theme(bool use_dark = false) {
    dark = use_dark;
    ink = themed(IM_COL32(32,38,41,255), IM_COL32(232,237,242,255));
    muted = themed(IM_COL32(115,123,128,255), IM_COL32(160,171,183,255));
    blue = themed(IM_COL32(36,103,236,255), IM_COL32(115,163,255,255));
    line = themed(IM_COL32(230,232,231,255), IM_COL32(53,63,76,255));
    paper = themed(IM_COL32(245,246,244,255), IM_COL32(22,27,34,255));
    panel = themed(IM_COL32(255,255,255,255), IM_COL32(30,37,47,255));
    if (dark) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.GrabRounding = 8;
    s.WindowPadding = ImVec2(20, 20);
    s.FrameBorderSize = 1;
    s.ScrollbarSize = 10;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 12);
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Text] = ImVec4(.125f, .149f, .161f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.45f, .48f, .50f, 1);
    c[ImGuiCol_Border] = ImVec4(.90f, .91f, .90f, 1);
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_FrameBg] = ImVec4(.988f, .992f, .984f, 1);
    c[ImGuiCol_Button] = ImVec4(.988f, .992f, .984f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.87f, .91f, .97f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.80f, .86f, .96f, 1);
    c[ImGuiCol_Header] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_PlotHistogram] = ImVec4(.14f, .40f, .92f, 1);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = rgba(panel);
    c[ImGuiCol_Text] = rgba(ink);
    c[ImGuiCol_TextDisabled] = rgba(muted);
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = rgba(line);
    if (dark) {
        c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = ImVec4(.15f,.19f,.24f,1);
        c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = ImVec4(.21f,.28f,.37f,1);
        c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = ImVec4(.26f,.35f,.48f,1);
        c[ImGuiCol_Header] = rgba(panel);
        c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
        c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
        c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = rgba(blue);
    }
}

enum class Icon { Cast, Home, Music, Screen, Face, Settings, Phone, Previous, Next, Play, Pause, Close, Minimize, Maximize, Fullscreen, Record, Moon, Sun };

inline void icon(ImDrawList* d, Icon kind, ImVec2 p, float size, ImU32 color) {
    auto v = [&](float x, float y) { return ImVec2(p.x + x * size / 24, p.y + y * size / 24); };
    auto stroke = [&](float x, float y, float xx, float yy) { d->AddLine(v(x,y), v(xx,yy), color, 1.6f); };
    switch (kind) {
    case Icon::Moon: {
        // Two intersecting circles give a crescent symmetric about y=12.
        // Its bounds (6..18) center it on the same axis as the sun.
        constexpr float pi = 3.14159265f;
        const float tip = std::acos(1.f/3.f);
        ImVec2 outline[64];
        for (int i=0;i<=32;++i) {
            const float a=-tip-(2*pi-2*tip)*i/32;
            outline[i]=v(15+9*std::cos(a),12+9*std::sin(a));
        }
        for (int i=1;i<32;++i) {
            const float a=pi-tip+2*tip*i/32;
            outline[32+i]=v(21+9*std::cos(a),12+9*std::sin(a));
        }
        d->AddPolyline(outline,64,color,ImDrawFlags_Closed,1.6f);
        break;
    }
    case Icon::Sun:
        d->AddCircle(v(12,12),size*.20f,color,24,1.6f);
        for (int i=0;i<8;++i) {
            const float a=i*3.14159265f/4;
            stroke(12+8*std::cos(a),12+8*std::sin(a),12+11*std::cos(a),12+11*std::sin(a));
        }
        break;
    case Icon::Cast:
        stroke(3,15,3,5); stroke(3,5,21,5); stroke(21,5,21,15); stroke(3,15,8,15); stroke(16,15,21,15);
        stroke(12,12,6,20); stroke(6,20,18,20); stroke(18,20,12,12); break;
    case Icon::Home:
        stroke(3,10,12,3); stroke(12,3,21,10); stroke(5,9,5,21); stroke(5,21,10,21);
        stroke(10,21,10,15); stroke(10,15,14,15); stroke(14,15,14,21); stroke(14,21,19,21); stroke(19,21,19,9); break;
    case Icon::Music:
        stroke(9,18,9,5); stroke(9,5,20,3); stroke(20,3,20,16); stroke(9,8,20,6);
        d->AddEllipse(v(6,18), ImVec2(size/8,size/10), color, 0, 20, 1.6f);
        d->AddEllipse(v(17,16), ImVec2(size/8,size/10), color, 0, 20, 1.6f); break;
    case Icon::Screen:
        d->AddRect(v(2,3),v(22,17),color,2,0,1.6f); stroke(12,17,12,21); stroke(8,21,16,21); break;
    case Icon::Face:
        d->AddCircle(v(12,12),size*.40f,color,32,1.6f);
        d->AddCircleFilled(v(9,10),size*.045f,color); d->AddCircleFilled(v(15,10),size*.045f,color);
        d->AddBezierCubic(v(8,14),v(9,18),v(15,18),v(16,14),color,1.6f,16); break;
    case Icon::Settings: {
        // A single closed outline avoids the overlaps and gaps produced by
        // drawing separate teeth on top of a circle at small icon sizes.
        ImVec2 outline[40];
        constexpr float angles[]={-22.5f,-15.f,-11.f,11.f,15.f};
        constexpr float radii[]={7.5f,7.5f,10.f,10.f,7.5f};
        for (int i=0; i<8; ++i) {
            for (int j=0;j<5;++j) {
                const float a=(i*45+angles[j])*3.14159265f/180;
                outline[i*5+j]=v(12+radii[j]*std::cos(a),12+radii[j]*std::sin(a));
            }
        }
        d->AddPolyline(outline,40,color,ImDrawFlags_Closed,1.5f);
        d->AddCircle(v(12,12),size*.13f,color,24,1.5f);
        break;
    }
    case Icon::Phone:
        d->AddRect(v(6,2),v(18,22),color,3,0,1.6f); stroke(10,5,14,5); stroke(11,19,13,19); break;
    case Icon::Previous:
        d->AddTriangleFilled(v(18,5),v(8,12),v(18,19),color); stroke(5,5,5,19); break;
    case Icon::Next:
        d->AddTriangleFilled(v(6,5),v(16,12),v(6,19),color); stroke(19,5,19,19); break;
    case Icon::Play: d->AddTriangleFilled(v(9,5),v(20,12),v(9,19),color); break;
    case Icon::Pause: d->AddRectFilled(v(7,5),v(10,19),color,1); d->AddRectFilled(v(14,5),v(17,19),color,1); break;
    case Icon::Close: stroke(7,7,17,17); stroke(17,7,7,17); break;
    case Icon::Minimize: stroke(7,12,17,12); break;
    case Icon::Maximize: d->AddRect(v(7,7),v(17,17),color,0,0,1.2f); break;
    case Icon::Fullscreen:
        stroke(3,9,3,3); stroke(3,3,9,3); stroke(15,3,21,3); stroke(21,3,21,9);
        stroke(3,15,3,21); stroke(3,21,9,21); stroke(15,21,21,21); stroke(21,21,21,15); break;
    case Icon::Record: d->AddCircleFilled(v(12,12),size*.18f,IM_COL32(205,92,92,255)); break;
    }
}

inline bool surface(const char* id, ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
    const bool clicked = ImGui::Button(id, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
}

inline bool icon_button(const char* id, Icon kind, const char* tooltip, float size=35, bool primary=false) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(size,size));
    auto* d = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered();
    const bool disabled = ImGui::GetStyle().Alpha < .9f;
    ImU32 color = disabled ? muted : themed(IM_COL32(83,97,88,255),ink);
    if (primary) {
        d->AddCircleFilled(ImVec2(p.x+size/2,p.y+size/2),size/2, disabled ? IM_COL32(174,183,176,255) : hover ? IM_COL32(59,81,68,255) : IM_COL32(40,56,47,255),48);
        color = IM_COL32(255,255,255,255);
    } else if (hover) d->AddRectFilled(p,ImVec2(p.x+size,p.y+size),themed(IM_COL32(235,239,235,255),IM_COL32(53,67,85,255)),8);
    icon(d,kind,ImVec2(p.x+(size-21)/2,p.y+(size-21)/2),21,color);
    if (hover && tooltip) ImGui::SetTooltip("%s",tooltip);
    return clicked;
}

inline bool nav(const char* id, const char* label, Icon kind, bool selected, float width, ImFont* font) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(width,44));
    auto* d = ImGui::GetWindowDrawList();
    if (selected || ImGui::IsItemHovered())
        d->AddRectFilled(p,ImVec2(p.x+width,p.y+44),selected ? themed(IM_COL32(238,243,255,255),IM_COL32(38,58,91,255)) : themed(IM_COL32(243,245,243,255),IM_COL32(42,51,64,255)),9);
    const ImU32 color = selected ? blue : muted;
    icon(d,kind,ImVec2(p.x+12,p.y+12),20,color);
    const float wrap_width=width-56;
    float font_size=font->FontSize;
    ImVec2 text_size=font->CalcTextSizeA(font_size,10000,wrap_width,label);
    while (text_size.y>36 && font_size>12) {
        font_size-=.5f;
        text_size=font->CalcTextSizeA(font_size,10000,wrap_width,label);
    }
    d->AddText(font,font_size,ImVec2(p.x+44,p.y+(44-text_size.y)/2),color,label,nullptr,wrap_width);
    return clicked;
}

inline bool text_button(const char* label, ImVec2 size=ImVec2(0,34)) {
    if (size.x>0) size.x=std::max(size.x,ImGui::CalcTextSize(label,nullptr,true).x+24);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,7);
    ImGui::PushStyleColor(ImGuiCol_Button,rgba(panel));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,rgba(themed(IM_COL32(250,252,250,255),IM_COL32(53,67,85,255))));
    const bool pressed = ImGui::Button(label,size);
    ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
    return pressed;
}
} // namespace ap::video::receiver_ui
