#include "ui/navigation.h"
#include "ui/theme.h"
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
    const float font = ImGui::GetFontSize();
    auto *draw = ImGui::GetWindowDrawList();
    const auto brand = ImGui::GetCursorScreenPos();
    const ImU32 accent = ImGui::GetColorU32(Accent());
    draw->AddRectFilled(brand, ImVec2(brand.x + font * 1.7f, brand.y + font * 1.7f),
                        IM_COL32(17, 42, 59, 255), font * .22f);
    draw->AddLine(ImVec2(brand.x + font * .4f, brand.y + font * .45f),
                  ImVec2(brand.x + font * .85f, brand.y + font * 1.25f), accent, font * .15f);
    draw->AddLine(ImVec2(brand.x + font * .85f, brand.y + font * 1.25f),
                  ImVec2(brand.x + font * 1.3f, brand.y + font * .45f), accent, font * .15f);
    ImGui::Dummy(ImVec2(font * 1.7f, font * 1.7f));
    if (ImGui::GetContentRegionAvail().x - font * 2.35f >= ImGui::CalcTextSize("VULKAN SCOPE").x)
        ImGui::SameLine(0, font * .65f);
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", "VULKAN SCOPE");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", Text(language, "Performance control"));
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::Separator();
    const float bar = ImGui::GetFontSize() * 3.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float height = std::max(1.0f, ImGui::GetContentRegionAvail().y - bar - gap);
    const char *pageId = state.selected == Page::Home ? "home-content" : "plugin-content";
    ImGui::BeginChild(pageId, ImVec2(0, height));
    BeginTouchScroll(state);
    ImGui::TextUnformatted(Text(language, state.selected == Page::Home ? "Overview" : "Plugins"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", Text(language, state.selected == Page::Home
                                                ? "Monitor your device and foreground applications."
                                                : "Manage the features shown on your home page."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
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
                              selected ? ImVec4(.065f, .15f, .21f, 1) : ImVec4(.052f, .068f, .095f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.085f, .19f, .26f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.10f, .24f, .33f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? Accent() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(i == 0 ? "###Home" : "###Plugins", ImVec2(width, bar)))
            state.selected = static_cast<Page>(i);
        state.tabMin[i] = ImGui::GetItemRectMin();
        state.tabMax[i] = ImGui::GetItemRectMax();
        const char *caption = Text(language, i == 0 ? "Home" : "Plugins");
        const float wrapWidth = std::max(1.f, width - ImGui::GetStyle().FramePadding.x * 2);
        const auto captionSize = ImGui::CalcTextSize(caption, nullptr, false, wrapWidth);
        const auto captionPos = ImVec2(state.tabMin[i].x + (width - captionSize.x) * .5f,
                                       state.tabMin[i].y + (bar - captionSize.y) * .5f);
        draw->AddText(ImGui::GetFont(), font, captionPos, ImGui::GetColorU32(ImGuiCol_Text),
                      caption, nullptr, wrapWidth);
        if (selected) {
            const auto a = state.tabMin[i], b = state.tabMax[i];
            const float inset = (b.x - a.x) * .32f;
            draw->AddRectFilled(ImVec2(a.x + inset, a.y), ImVec2(b.x - inset, a.y + std::max(2.f, font * .12f)), accent, 1.f);
        }
        ImGui::PopStyleColor(4);
    }
}
}
