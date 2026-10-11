#pragma once
#include "imgui.h"
#include "ui/i18n.h"
namespace vulkanscope::ui {
/** @brief 表单标签在控件上方自动换行，避免长译文或系统大字号被屏幕右缘裁切。 */
inline std::string FieldLabel(Language language, const char *key) {
    ImGui::TextWrapped("%s", Text(language, key));
    ImGui::SetNextItemWidth(-1);
    return std::string("##") + key;
}
}
