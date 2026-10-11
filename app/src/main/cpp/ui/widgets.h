#pragma once
#include "imgui.h"
#include "ui/i18n.h"
namespace vulkanscope::ui {
/** @brief 下一个操作能容纳时才续行，避免窄屏和大字号下按钮被裁切。 */
inline void SameLineIfFits(const char *nextLabel) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(nextLabel, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine();
}
/** @brief 表单标签在控件上方自动换行，避免长译文或系统大字号被屏幕右缘裁切。 */
inline std::string FieldLabel(Language language, const char *key) {
    ImGui::TextWrapped("%s", Text(language, key));
    ImGui::SetNextItemWidth(-1);
    return std::string("##") + key;
}
}
