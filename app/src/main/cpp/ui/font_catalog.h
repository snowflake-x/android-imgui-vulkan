#pragma once
#include <string>
#include <vector>
namespace vulkanscope::ui {
struct FontFile {
    std::string path;
    int index = 0;
};
/** @brief 解析 Android familyset，保留系统 sans-serif 与语言回退顺序、TTC 索引。 */
std::vector<FontFile> ParseFontConfig(const std::string &xml, const std::string &directory,
                                      const std::string &locale);
}
