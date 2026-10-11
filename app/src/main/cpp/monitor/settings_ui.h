#pragma once
#include "ui/i18n.h"
struct ImFont;
namespace vulkanscope::monitor {
struct SettingsState {
    bool surfaceKeyboard = false, powerKeyboard = false, shifted = false;
};
/** @brief 在调用线程的当前 ImGui 上下文绘制原生配置与诊断界面。 */
void DrawSettings(SettingsState &state, ui::Language language = ui::Language::English, ImFont *hudFont = nullptr);
}
