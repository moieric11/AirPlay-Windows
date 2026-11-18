#pragma once

#include <string_view>
#include <unordered_map>
#include <array>

namespace ap::i18n {
enum class Language { English, French, ChineseSimplified, Spanish, PortugueseBrazil, German, Japanese, Italian, Russian, Korean, Count };
inline constexpr const char* codes[] = {"en", "fr", "zh-CN", "es", "pt-BR", "de", "ja", "it", "ru", "ko"};
inline constexpr const char* names[] = {"English", "Français", "简体中文", "Español", "Português (Brasil)", "Deutsch", "日本語", "Italiano", "Русский", "한국어"};
inline constexpr auto language_count = static_cast<size_t>(Language::Count);
static_assert(sizeof(codes)/sizeof(*codes)==language_count && sizeof(names)/sizeof(*names)==language_count);
inline Language current_language = Language::English;

inline bool supported(std::string_view code) {
    for (const auto* candidate : codes) if (code == candidate) return true;
    return false;
}
inline Language language_from_code(std::string_view code) {
    for (size_t i=0;i<language_count;++i)
        if (code == codes[i]) return static_cast<Language>(i);
    return Language::English;
}
inline void set_language(std::string_view code) { current_language = language_from_code(code); }
struct Translation {
    const char* english;
    std::array<const char*, language_count-1> localized;
};
inline constexpr Translation translations[] = {
#include "translations.inc"
};

// Returned pointers remain valid across language switches. Unknown text falls
// back to the supplied English string, including runtime diagnostic messages.
inline const char* tr(const char* english) {
    if (!english) return "";
    if (current_language == Language::English) return english;
    static const auto index = [] {
        std::unordered_map<std::string_view, const Translation*> result;
        for (const auto& entry : translations) result.emplace(entry.english, &entry);
        return result;
    }();
    const auto found = index.find(english);
    if (found == index.end()) return english;
    const auto language = static_cast<size_t>(current_language);
    if (language==0 || language>=language_count) return english;
    const auto* translated = found->second->localized[language-1];
    return translated && *translated ? translated : english;
}
} // namespace ap::i18n
