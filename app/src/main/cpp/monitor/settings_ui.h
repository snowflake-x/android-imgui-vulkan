#pragma once
namespace vulkanscope::monitor {
struct SettingsState {
    bool surfaceKeyboard = false, powerKeyboard = false, shifted = false;
};
/** @brief 在调用线程的当前 ImGui 上下文绘制原生配置与诊断界面。 */
void DrawSettings(SettingsState &state);
}
