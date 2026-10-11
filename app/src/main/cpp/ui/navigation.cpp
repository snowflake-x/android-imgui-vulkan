#include "ui/navigation.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include "imgui_internal.h"
namespace vulkanscope::ui {
namespace {
/** @brief 在控件处理释放前识别纵向拖动；使用独立 ActiveID 阻止拖动末尾误点击。 */
void BeginTouchScroll(Navigation &state) {
    auto &touch = state.touch;
    auto &g = *ImGui::GetCurrentContext();
    auto &io = ImGui::GetIO();
    auto *window = ImGui::GetCurrentWindow();
    const auto id = window->GetID("##page-touch-scroll");
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || touch.window != window->ID ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        if (touch.owner && g.ActiveId == touch.owner) ImGui::ClearActiveID();
        touch = {};
    }
    if (ImGui::IsMouseClicked(0) && io.MouseSource == ImGuiMouseSource_TouchScreen &&
        ImGui::IsWindowHovered() && window->InnerRect.Contains(io.MousePos)) {
        touch.window = window->ID;
        touch.owner = id;
        touch.start = touch.previous = io.MousePos;
        touch.pending = true;
        touch.dragging = false;
    }
    if (!touch.pending) return;
    if (!ImGui::IsMousePosValid() || (!io.MouseDown[0] && !ImGui::IsMouseReleased(0))) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        touch = {};
        return;
    }
    if (g.ActiveId == id) ImGui::KeepAliveID(id);
    const float dx = io.MousePos.x - touch.start.x, dy = io.MousePos.y - touch.start.y;
    const float threshold = std::max(io.MouseDragThreshold, ImGui::GetFontSize() * .4f);
    if (!touch.dragging && std::abs(dy) > threshold && std::abs(dy) > std::abs(dx)) {
        touch.dragging = true;
        ImGui::SetActiveID(id, window);
    }
    if (touch.dragging) {
        ImGui::KeepAliveID(id);
        ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() - (io.MousePos.y - touch.previous.y),
                                   0.f, ImGui::GetScrollMaxY()));
    }
    touch.previous = io.MousePos;
}
/** @brief 滑块、文本编辑、滚动条及弹出窗口保留自身手势；空白区域占用按下避免移动窗口。 */
void EndTouchScroll(Navigation &state) {
    auto &touch = state.touch;
    if (!touch.pending) return;
    auto &g = *ImGui::GetCurrentContext();
    auto *window = ImGui::GetCurrentWindow();
    const auto id = window->GetID("##page-touch-scroll");
    if (!ImGui::GetIO().MouseDown[0]) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        touch = {};
    } else if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        touch = {};
    } else if (!touch.dragging && g.ActiveId && g.ActiveId != id &&
               (g.ActiveIdWindow != window || g.ActiveIdUsingNavDirMask || g.ActiveIdUsingAllKeyboardKeys)) {
        touch = {};
    } else if (!g.ActiveId) {
        ImGui::SetActiveID(id, window);
    }
}
}
void CancelPageTouch(Navigation &state) {
    state.touch = {};
    ImGui::ClearActiveID();
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, false);
}
void DrawPages(Navigation &state, Language language, const std::function<void()> &home,
               const std::function<void()> &plugins) {
    ImGui::TextColored(ImVec4(.59f, .85f, .67f, 1), "VULKAN SCOPE");
    const float bar = ImGui::GetFontSize() * 3.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float height = std::max(1.0f, ImGui::GetContentRegionAvail().y - bar - gap);
    const char *pageId = state.selected == Page::Home ? "home-content" : "plugin-content";
    ImGui::BeginChild(pageId, ImVec2(0, height));
    BeginTouchScroll(state);
    ImGui::TextUnformatted(Text(language, state.selected == Page::Home ? "Overview" : "Plugins"));
    ImGui::TextWrapped("%s", Text(language, state.selected == Page::Home
                                                ? "Monitor your device and foreground applications."
                                                : "Manage the features shown on your home page."));
    ImGui::Separator();
    if (state.selected == Page::Home)
        home();
    else
        plugins();
    EndTouchScroll(state);
    ImGui::EndChild();
    const float width =
        std::max(1.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f);
    for (int i = 0; i < 2; ++i) {
        if (i)
            ImGui::SameLine();
        const bool selected = static_cast<int>(state.selected) == i;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              selected ? ImVec4(.16f, .32f, .28f, 1) : ImVec4(.12f, .13f, .15f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.20f, .40f, .34f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.24f, .46f, .38f, 1));
        if (ImGui::Button(Label(language, i == 0 ? "Home" : "Plugins").c_str(), ImVec2(width, bar)))
            state.selected = static_cast<Page>(i);
        state.tabMin[i] = ImGui::GetItemRectMin();
        state.tabMax[i] = ImGui::GetItemRectMax();
        ImGui::PopStyleColor(3);
    }
}
}
