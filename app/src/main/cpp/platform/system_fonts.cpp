#include "platform/system_fonts.h"
#include "monitor/collector.h"
#include "ui/font_catalog.h"
#ifdef __ANDROID__
#include <android/font.h>
#include <android/font_matcher.h>
#include <dlfcn.h>
#endif
#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
/** @brief 复用 ImGui 随带的字体解析器，只保留实际提供所需字形的系统字体。 */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

namespace vulkanscope {
namespace {
/** @brief 通过 dlsym 使用 API 29 字体匹配，避免 API 26–28 的动态链接失败。 */
#ifdef __ANDROID__
struct Matcher {
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    AFontMatcher *(*create)() = nullptr;
    void (*destroy)(AFontMatcher *) = nullptr;
    void (*locales)(AFontMatcher *, const char *) = nullptr;
    AFont *(*match)(const AFontMatcher *, const char *, const uint16_t *, uint32_t, uint32_t *) = nullptr;
    const char *(*path)(const AFont *) = nullptr;
    size_t (*index)(const AFont *) = nullptr;
    void (*close)(AFont *) = nullptr;
    AFontMatcher *value = nullptr;
    Matcher() {
        if (!library)
            return;
#define FONT_SYMBOL(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(library, #symbol))
        FONT_SYMBOL(create, AFontMatcher_create);
        FONT_SYMBOL(destroy, AFontMatcher_destroy);
        FONT_SYMBOL(locales, AFontMatcher_setLocales);
        FONT_SYMBOL(match, AFontMatcher_match);
        FONT_SYMBOL(path, AFont_getFontFilePath);
        FONT_SYMBOL(index, AFont_getCollectionIndex);
        FONT_SYMBOL(close, AFont_close);
#undef FONT_SYMBOL
        if (create && destroy && locales && match && path && index && close)
            value = create();
    }
    ~Matcher() {
        if (value)
            destroy(value);
        if (library)
            dlclose(library);
    }
};
#endif
}
bool SystemFonts::load(float pixels, const std::string &locale, const std::string &corpus,
                       const std::vector<ui::FontFile> &fallback) {
    ranges_.clear();
    description_.clear();
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
    builder.AddText(corpus.c_str());
    ImVector<ImWchar> requested;
    builder.BuildRanges(&requested);
    requested_.assign(requested.Data, requested.Data + requested.Size);
    struct Group {
        ui::FontFile file;
        std::vector<ImWchar> glyphs;
    };
    std::vector<Group> groups;
#ifdef __ANDROID__
    Matcher matcher;
    if (matcher.value) {
        matcher.locales(matcher.value, locale.c_str());
        for (int n = 0; n + 1 < requested.Size && requested[n]; n += 2) {
            for (unsigned point = requested[n]; point <= requested[n + 1]; ++point) {
                const uint16_t utf16[] = {
                    static_cast<uint16_t>(point > 0xffff ? 0xd800 + ((point - 0x10000) >> 10) : point),
                    static_cast<uint16_t>(0xdc00 + ((point - 0x10000) & 0x3ff))};
                auto *font =
                    matcher.match(matcher.value, "sans-serif", utf16, point > 0xffff ? 2 : 1, nullptr);
                if (!font)
                    continue;
                ui::FontFile file{matcher.path(font), static_cast<int>(matcher.index(font))};
                matcher.close(font);
                auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
                    return g.file.path == file.path && g.file.index == file.index;
                });
                if (it == groups.end()) {
                    groups.push_back({file, {}});
                    it = groups.end() - 1;
                }
                it->glyphs.push_back(static_cast<ImWchar>(point));
                it->glyphs.push_back(static_cast<ImWchar>(point));
            }
        }
    }
#endif
    if (groups.empty()) {
        auto files = fallback.empty()
                         ? ui::ParseFontConfig(monitor::ReadFile("/system/etc/fonts.xml", 1024 * 1024),
                                               "/system/fonts", locale)
                         : fallback;
        for (const auto &file : files)
            groups.push_back(
                {file, std::vector<ImWchar>(requested.Data, requested.Data + requested.Size - 1)});
    }
    ranges_.reserve(groups.size());
    bool loaded = false;
    std::set<ImWchar> covered;
    for (auto &group : groups) {
        std::ifstream file(group.file.path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() > 64 * 1024 * 1024)
            continue;
        const auto length = static_cast<int>(file.tellg());
        void *buffer = ImGui::MemAlloc(length);
        if (!buffer)
            continue;
        file.seekg(0);
        file.read(static_cast<char *>(buffer), length);
        if (!file) {
            ImGui::MemFree(buffer);
            continue;
        }
        stbtt_fontinfo info{};
        const auto *bytes = static_cast<const unsigned char *>(buffer);
        const int offset = stbtt_GetFontOffsetForIndex(bytes, group.file.index);
        if (offset < 0 || offset >= length || !stbtt_InitFont(&info, bytes, offset)) {
            ImGui::MemFree(buffer);
            continue;
        }
        std::vector<ImWchar> needed;
        for (std::size_t i = 0; i + 1 < group.glyphs.size(); i += 2)
            for (unsigned point = group.glyphs[i]; point <= group.glyphs[i + 1]; ++point)
                if (!covered.count(point) && stbtt_FindGlyphIndex(&info, static_cast<int>(point))) {
                    needed.push_back(point);
                    needed.push_back(point);
                    covered.insert(point);
                }
        if (needed.empty()) {
            ImGui::MemFree(buffer);
            continue;
        }
        group.glyphs = std::move(needed);
        group.glyphs.push_back(0);
        ranges_.push_back(std::move(group.glyphs));
        ImFontConfig config;
        config.FontNo = group.file.index;
        config.MergeMode = loaded;
        config.OversampleH = 2;
        config.OversampleV = 2;
        auto *font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, length, pixels, &config,
                                                                ranges_.back().data());
        if (font) {
            loaded = true;
            if (description_.empty())
                description_ = group.file.path;
        }
    }
    return loaded;
}
std::string MergeGlyphCorpus(const std::string &existing, const std::string &text) {
    ImFontGlyphRangesBuilder builder;
    builder.AddText(existing.c_str());
    builder.AddText(text.c_str());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    std::string result;
    for (int i = 0; ranges[i]; i += 2)
        for (unsigned c = ranges[i]; c <= ranges[i + 1]; ++c) {
            if (c < 0x80)
                result += static_cast<char>(c);
            else if (c < 0x800) {
                result += static_cast<char>(0xc0 | (c >> 6));
                result += static_cast<char>(0x80 | (c & 63));
            } else if (c < 0x10000) {
                result += static_cast<char>(0xe0 | (c >> 12));
                result += static_cast<char>(0x80 | ((c >> 6) & 63));
                result += static_cast<char>(0x80 | (c & 63));
            } else {
                result += static_cast<char>(0xf0 | (c >> 18));
                result += static_cast<char>(0x80 | ((c >> 12) & 63));
                result += static_cast<char>(0x80 | ((c >> 6) & 63));
                result += static_cast<char>(0x80 | (c & 63));
            }
        }
    return result;
}
bool SystemFonts::needsGlyphs(const std::string &text) const {
    ImFontGlyphRangesBuilder builder;
    builder.AddText(text.c_str());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    for (int n = 0; n + 1 < ranges.Size && ranges[n]; n += 2)
        for (unsigned point = ranges[n]; point <= ranges[n + 1]; ++point) {
            bool found = false;
            for (std::size_t i = 0; i + 1 < requested_.size() && requested_[i]; i += 2)
                if (point >= requested_[i] && point <= requested_[i + 1]) {
                    found = true;
                    break;
                }
            if (!found)
                return true;
        }
    return false;
}
}
