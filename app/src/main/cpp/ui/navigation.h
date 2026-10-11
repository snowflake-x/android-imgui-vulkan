#pragma once
#include "imgui.h"
#include "ui/i18n.h"
#include <functional>
namespace vulkanscope::ui {
enum class Page { Home, Plugins };
/** @brief 页面选择由宿主持有，内容滚动区与固定底栏分离；切页只影响显示。 */
struct Navigation {
    Page selected = Page::Home;
    ImVec2 tabMin[2]{}, tabMax[2]{};
    struct TouchScroll {
        ImGuiID window = 0, owner = 0;
        ImVec2 start{}, previous{};
        bool pending = false, dragging = false;
    } touch;

};
/** @brief 系统取消触摸时丢弃手势，避免抬起触发页面控件。 */
void CancelPageTouch(Navigation &state);
void DrawPages(Navigation &state, Language language, const std::function<void()> &home,
               const std::function<void()> &plugins);
}
