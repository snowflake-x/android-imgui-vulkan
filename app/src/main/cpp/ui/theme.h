#pragma once

#include "imgui.h"

struct ImGuiWindow;

namespace vulkanscope::ui {

/** @brief 重置当前 ImGui 上下文的样式并应用深色蓝灰主题；重复调用不会累计缩放。 */
void ApplyTheme(float scale = 1.0f);

/** @brief 返回用于重点操作与当前页面的青蓝色。 */
ImVec4 Accent();

/** @brief 返回次要说明文字的颜色。 */
ImVec4 Muted();

/**
 * @brief 在当前窗口中绘制自动高度、全宽的卡片，不创建额外滚动窗口。
 * @note 允许嵌套；作用域必须在当前 ImGui 窗口结束前关闭。卡片内可正常使用折叠控件。
 */
class Card {
public:
    explicit Card(const char *id);
    ~Card();
    Card(const Card &) = delete;
    Card &operator=(const Card &) = delete;
    Card(Card &&) = delete;
    Card &operator=(Card &&) = delete;

private:
    ImGuiWindow *window_;
    ImVec2 origin_;
    ImVec2 workBounds_;
    ImVec2 contentBounds_;
    float width_;
    float padding_;
    ImDrawListSplitter layers_;
};

/**
 * @brief 整行可点击的开关，标签自动换行，保留 ImGui 的释放触发与手势取消语义。
 * @param label 可包含 ## 或 ### 稳定标识后缀，与标准 ImGui 控件一致。
 * @param enabled 当前值；仅在完成点击或键盘激活时修改。
 * @return 值是否发生改变。
 */
bool Toggle(const char *label, bool *enabled);

}
