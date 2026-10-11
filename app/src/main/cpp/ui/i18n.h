#pragma once
#include <string>
#include <string_view>
namespace vulkanscope::ui {
enum class Language { English, Chinese };
/** @brief 按显式偏好或 BCP-47 系统语言选择译文；未知语言回退英文。 */
Language ResolveLanguage(std::string_view preference, std::string_view systemLocale);
const char *Text(Language language, const char *source);
/** @brief 翻译显示文字并保留英文源键作为 ImGui ID，切换语言不丢失控件状态。 */
std::string Label(Language language, const char *source);
/** @brief 字体预热用的完整翻译语料；不依赖可变的全局语言状态。 */
std::string TranslationCorpus();
/** @brief 本地化报告标签，保留原始指标值、路径和诊断内容。 */
std::string TranslateReport(Language language, const std::string &report);
}
