#pragma once
#include "imgui.h"
#include "ui/i18n.h"
#include <filesystem>

namespace ap::video::ui_fonts {
// Load every translated glyph up front so changing languages never rebuilds
// the font atlas during a frame. The ranges outlive all atlas builds.
inline const ImWchar* glyph_ranges() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        for (const auto& entry : ap::i18n::translations) {
            builder.AddText(entry.english);
            for (const auto* text : entry.localized) if (text) builder.AddText(text);
        }
        for (const auto* name : ap::i18n::names) builder.AddText(name);
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}
inline const char* chinese_font(bool bold = false) {
#if defined(_WIN32)
    if (bold && std::filesystem::exists("C:/Windows/Fonts/msyhbd.ttc")) return "C:/Windows/Fonts/msyhbd.ttc";
    constexpr const char* candidates[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc"};
#else
    constexpr const char* candidates[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"};
    (void)bold;
#endif
    for (const auto* path : candidates) if (std::filesystem::exists(path)) return path;
    return nullptr;
}
inline ImFont* add_font(const char* path, float size, bool bold = false) {
    auto* atlas = ImGui::GetIO().Fonts;
    ImFont* font = atlas->AddFontFromFileTTF(path, size, nullptr, glyph_ranges());
    if (font) {
        auto merge=[&](const char* fallback) {
            if (!fallback) return;
            ImFontConfig config;
            config.MergeMode = true;
            config.PixelSnapH = true;
            config.DstFont = font;
            atlas->AddFontFromFileTTF(fallback, size, &config, glyph_ranges());
        };
        merge(chinese_font(bold));
#if defined(_WIN32)
        // Supply Japanese glyphs absent from the Chinese fallback. Cyrillic
        // and Latin accents are already provided by the primary Segoe font.
        const char* japanese=bold ? "C:/Windows/Fonts/YuGothB.ttc" : "C:/Windows/Fonts/YuGothM.ttc";
        if (std::filesystem::exists(japanese)) merge(japanese);
        else if (std::filesystem::exists("C:/Windows/Fonts/msgothic.ttc")) merge("C:/Windows/Fonts/msgothic.ttc");
        const char* korean=bold ? "C:/Windows/Fonts/malgunbd.ttf" : "C:/Windows/Fonts/malgun.ttf";
        if (std::filesystem::exists(korean)) merge(korean);
        else if (bold && std::filesystem::exists("C:/Windows/Fonts/malgun.ttf")) merge("C:/Windows/Fonts/malgun.ttf");
#endif
    }
    return font;
}
} // namespace ap::video::ui_fonts
