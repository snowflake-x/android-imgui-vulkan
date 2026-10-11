#include "ui/font_catalog.h"
#include "vendor/tinyxml2/tinyxml2.h"
#include <algorithm>
#include <cmath>
#include <string_view>
namespace vulkanscope::ui {
std::vector<FontFile> ParseFontConfig(const std::string &xml, const std::string &directory,
                                      const std::string &locale) {
    tinyxml2::XMLDocument doc;
    if (doc.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS)
        return {};
    const auto *root = doc.FirstChildElement("familyset");
    if (!root)
        return {};
    std::string primary = "sans-serif";
    for (int depth = 0; depth < 8; ++depth) {
        bool found = false;
        for (auto *alias = root->FirstChildElement("alias"); alias;
             alias = alias->NextSiblingElement("alias")) {
            const char *name = alias->Attribute("name"), *to = alias->Attribute("to");
            if (name && to && primary == name) {
                primary = to;
                found = true;
                break;
            }
        }
        if (!found)
            break;
    }
    struct Ranked {
        FontFile file;
        int priority;
    };
    std::vector<Ranked> ranked;
    const auto language = locale.substr(0, locale.find_first_of("-_,"));
    for (auto *family = root->FirstChildElement("family"); family;
         family = family->NextSiblingElement("family")) {
        const char *name = family->Attribute("name"), *lang = family->Attribute("lang");
        if (name && primary != name && std::string_view(name) != "sans-serif")
            continue;
        const int priority =
            name ? 0
            : lang && !language.empty() && std::string_view(lang).find(language) != std::string_view::npos
                ? 1
                : 2;
        const tinyxml2::XMLElement *best = nullptr;
        int score = 10000;
        for (auto *font = family->FirstChildElement("font"); font; font = font->NextSiblingElement("font")) {
            const char *style = font->Attribute("style");
            int candidate = std::abs(font->IntAttribute("weight", 400) - 400) +
                            (style && std::string_view(style) == "italic" ? 1000 : 0);
            if (candidate < score && font->GetText()) {
                score = candidate;
                best = font;
            }
        }
        if (!best)
            continue;
        std::string path = best->GetText();
        auto first = path.find_first_not_of(" \r\n\t"), last = path.find_last_not_of(" \r\n\t");
        if (first == std::string::npos)
            continue;
        path = path.substr(first, last - first + 1);
        if (path.find("..") != std::string::npos)
            continue;
        if (path.front() != '/')
            path = directory + "/" + path;
        int index = best->IntAttribute("index", 0);
        if (index < 0 || index > 256)
            continue;
        ranked.push_back({{path, index}, priority});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.priority < b.priority; });
    std::vector<FontFile> result;
    for (const auto &font : ranked)
        if (std::none_of(result.begin(), result.end(), [&](const auto &f) {
                return f.path == font.file.path && f.index == font.file.index;
            }))
            result.push_back(font.file);
    return result;
}
}
