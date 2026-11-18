#include "ui/i18n.h"
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static std::vector<std::string> arguments(const char* text) {
    static const std::regex format(R"(%(?:%|[-+#0]*[0-9]*(?:\.[0-9]+)?(?:hh|ll|h|l|j|z|t|L)?[diuoxXfFeEgGaAcspn]))");
    const std::string value(text);
    std::vector<std::string> result;
    for (auto it=std::sregex_iterator(value.begin(),value.end(),format);it!=std::sregex_iterator();++it)
        result.push_back(it->str());
    return result;
}
int main() {
    try {
        using namespace ap::i18n;
        std::set<std::string> keys;
        for (const auto& entry : translations) {
            require(keys.emplace(entry.english).second,"duplicate translation key");
            for (const auto* text : entry.localized) {
                require(text && *text,"empty translation");
                require(arguments(entry.english)==arguments(text),"translated format arguments changed");
            }
            const std::string key(entry.english);
            if (const auto suffix=key.find("##");suffix!=std::string::npos) {
                const std::string id=key.substr(suffix);
                for (const auto* text : entry.localized) {
                    const std::string translated(text);
                    const auto offset=translated.find("##");
                    require(offset!=std::string::npos && translated.substr(offset)==id,"ImGui ID suffix changed");
                }
            }
            set_language("en");
            require(std::string(tr(entry.english))==entry.english,"English lookup failed");
            for (size_t i=1;i<language_count;++i) {
                set_language(codes[i]);
                require(static_cast<size_t>(current_language)==i,"language code mapping failed");
                require(std::string(tr(entry.english))==entry.localized[i-1],"localized lookup failed");
            }
        }
        set_language("zh-CN");
        const auto* chinese=tr("Settings");
        set_language("fr");
        require(std::string(chinese)=="设置","language switch invalidated text pointer");
        require(std::string(tr("Untranslated diagnostic"))=="Untranslated diagnostic","missing English fallback");
        require(std::string(tr(nullptr)).empty(),"null text fallback failed");
        require(!supported("invalid") && supported("en") && supported("fr") && supported("zh-CN"),"language validation failed");
        for (const auto* code : codes) require(supported(code),"registered language is unsupported");
        set_language("invalid");
        require(current_language==Language::English,"invalid language fallback failed");
        set_language("ko");
        require(current_language==Language::Korean && std::string(tr("Settings"))=="설정","Korean language selection failed");
        std::cout<<keys.size()<<" translation keys in "<<language_count<<" languages: lookups, switching, fallback, format arguments and IDs passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
