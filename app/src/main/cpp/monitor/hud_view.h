#pragma once
#include "imgui.h"
#include "monitor/hud.h"
namespace vulkanscope::monitor {
/** @brief 使用同一 ImGui 绘制路径生成悬浮窗与设置预览；不触发采集或读取平台资源。 */
void DrawHud(ImDrawList &draw, ImVec2 origin, const HudSize &size, const Config &config,
             const HudSnapshot &snapshot, ImFont *font = nullptr);
/** @brief 绘制原有 MetalHUD 样式，供共享调度及布局回归测试使用。 */
void DrawClassicHud(ImDrawList &draw, ImVec2 origin, const HudSize &size, const Config &config,
                    const HudSnapshot &snapshot, ImFont *font = nullptr);
}
