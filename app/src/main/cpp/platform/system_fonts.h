#pragma once
#include "imgui.h"
#include "ui/font_catalog.h"
#include <string>
#include <vector>
namespace vulkanscope {
/** @brief 每个 ImGui context 独立拥有字形范围；须存活到 atlas 清理后，不跨 context 复用。 */
class SystemFonts {
  public:
    bool load(float pixels, const std::string &locale, const std::string &corpus,
              const std::vector<ui::FontFile> &fallback = {});
    bool needsGlyphs(const std::string &text) const;
    const std::string &description() const { return description_; }

  private:
    std::vector<std::vector<ImWchar>> ranges_;
    std::string description_;
    std::vector<ImWchar> requested_;
};
/** @brief 从 Android Resources.Configuration 读取当前 BCP-47 语言列表。 */
std::string ReadSystemLocale();
/** @brief 读取 Android 无障碍字号倍率；失败回退 1。 */
float ReadSystemFontScale();
/** @brief 合并字形需求并去重，避免诊断内容随重建次数无限增长。 */
std::string MergeGlyphCorpus(const std::string &existing, const std::string &text);
}
