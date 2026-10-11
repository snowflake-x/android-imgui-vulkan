#include "ui/navigation.h"
#include "ui/theme.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include "imgui_internal.h"
namespace vulkanscope::ui {
namespace {
/** @brief 丢弃本手势尚未应用的滚动目标，避免切页或重新按下后再滑动一帧。 */
void StopTouchScroll(Navigation &state) {
    auto &touch = state.touch;
    if (auto *window = ImGui::FindWindowByID(touch.window);
        window && touch.hasTarget && window->ScrollTarget.y == touch.target)
        window->ScrollTarget.y = FLT_MAX;
    if (touch.owner && ImGui::GetCurrentContext()->ActiveId == touch.owner)
        ImGui::ClearActiveID();
    touch = {};
}
/** @brief 用最近 100ms 的位置拟合速度；按住不动会淘汰旧运动，释放帧不凭空添加零速度。 */
void RecordMotion(Navigation::TouchScroll &touch, float y) {
    const double now = ImGui::GetTime();
    while (touch.sampleCount &&
           (now - touch.samples[0].time > .1 || touch.sampleCount == static_cast<int>(touch.samples.size()))) {
        for (int i = 1; i < touch.sampleCount; ++i)
            touch.samples[i - 1] = touch.samples[i];
        --touch.sampleCount;
    }
    if (touch.sampleCount && touch.samples[touch.sampleCount - 1].time == now)
        --touch.sampleCount;
    touch.samples[touch.sampleCount++] = {now, y};
    double timeMean = 0, positionMean = 0;
    for (int i = 0; i < touch.sampleCount; ++i) {
        timeMean += touch.samples[i].time - now;
        positionMean += touch.samples[i].y;
    }
    timeMean /= touch.sampleCount;
    positionMean /= touch.sampleCount;
    double covariance = 0, variance = 0;
    for (int i = 0; i < touch.sampleCount; ++i) {
        const double time = touch.samples[i].time - now - timeMean;
        covariance += time * (touch.samples[i].y - positionMean);
        variance += time * time;
    }
    const float limit = ImGui::GetFontSize() * 140.f;
    touch.velocity = variance > 1e-9 ? std::clamp(static_cast<float>(-covariance / variance), -limit, limit) : 0;
}
void ScrollTo(Navigation::TouchScroll &touch, float position) {
    touch.position = touch.target = std::clamp(position, 0.f, ImGui::GetScrollMaxY());
    touch.hasTarget = true;
    ImGui::SetScrollY(touch.target);
}
/** @brief 在控件处理释放前识别纵向拖动；使用独立 ActiveID 阻止拖动末尾误点击。 */
void BeginTouchScroll(Navigation &state, bool catchFling) {
    auto &touch = state.touch;
    auto &g = *ImGui::GetCurrentContext();
    auto &io = ImGui::GetIO();
    auto *window = ImGui::GetCurrentWindow();
    const auto id = window->GetID("##page-touch-scroll");
    const auto viewport = window->InnerRect.GetSize();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || touch.window != window->ID ||
        (touch.window && (touch.viewportSize.x != viewport.x || touch.viewportSize.y != viewport.y)) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        StopTouchScroll(state);
    }
    if (ImGui::IsMouseClicked(0) && io.MouseSource == ImGuiMouseSource_TouchScreen &&
        ImGui::IsWindowHovered() && window->InnerRect.Contains(io.MousePos)) {
        touch.window = window->ID;
        touch.owner = id;
        touch.page = state.selected;
        touch.displaySize = io.DisplaySize;
        touch.viewportSize = viewport;
        touch.start = touch.previous = io.MousePos;
        touch.pending = true;
        touch.dragging = false;
        RecordMotion(touch, io.MousePos.y);
        if (catchFling)
            ImGui::SetActiveID(id, window);
    }
    if (touch.coasting) {
        /** @brief 指数减速的解析积分与刷新率无关；浮点位置保留 ImGui 整像素滚动丢掉的小数。 */
        constexpr float friction = 5.5f;
        const float decay = std::exp(-friction * io.DeltaTime);
        const float next = touch.position + touch.velocity * (1.f - decay) / friction;
        touch.velocity *= decay;
        ScrollTo(touch, next);
        if (next <= 0 || next >= ImGui::GetScrollMaxY() ||
            std::abs(touch.velocity) < ImGui::GetFontSize() * .5f)
            touch = {};
        return;
    }
    if (!touch.pending) return;
    if (!ImGui::IsMousePosValid() || (!io.MouseDown[0] && !ImGui::IsMouseReleased(0))) {
        StopTouchScroll(state);
        return;
    }
    if (g.ActiveId == id) ImGui::KeepAliveID(id);
    if (io.MouseDown[0] || io.MousePos.y != touch.previous.y)
        RecordMotion(touch, io.MousePos.y);
    const float dx = io.MousePos.x - touch.start.x, dy = io.MousePos.y - touch.start.y;
    const float threshold = std::max(io.MouseDragThreshold, ImGui::GetFontSize() * .4f);
    if (!touch.dragging && std::abs(dy) > threshold && std::abs(dy) > std::abs(dx)) {
        touch.dragging = true;
        ImGui::SetActiveID(id, window);
    }
    if (touch.dragging) {
        ImGui::KeepAliveID(id);
        ScrollTo(touch, ImGui::GetScrollY() - (io.MousePos.y - touch.previous.y));
    }
    touch.previous = io.MousePos;
}
/** @brief 滑块、文本编辑、滚动条及弹出窗口保留自身手势；空白区域占用按下避免移动窗口。 */
void EndTouchScroll(Navigation &state) {
    auto &touch = state.touch;
    auto &g = *ImGui::GetCurrentContext();
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) || (touch.coasting && g.ActiveId)) {
        StopTouchScroll(state);
        return;
    }
    if (!touch.pending) return;
    auto *window = ImGui::GetCurrentWindow();
    const auto id = window->GetID("##page-touch-scroll");
    if (!ImGui::GetIO().MouseDown[0]) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        const bool recentMotion = touch.sampleCount &&
                                  ImGui::GetTime() - touch.samples[touch.sampleCount - 1].time <= .1;
        touch.coasting = touch.dragging && recentMotion &&
                         std::abs(touch.velocity) >= ImGui::GetFontSize() * 3.f &&
                         ((touch.velocity > 0 && touch.position < ImGui::GetScrollMaxY()) ||
                          (touch.velocity < 0 && touch.position > 0));
        touch.pending = touch.dragging = false;
        if (!touch.coasting)
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
    StopTouchScroll(state);
    ImGui::ClearActiveID();
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, false);
}
void DrawPages(Navigation &state, Language language, const std::function<void()> &home,
               const std::function<void()> &plugins) {
    auto &io = ImGui::GetIO();
    const bool catchFling = state.touch.coasting && ImGui::IsMouseClicked(0) &&
                            io.MouseSource == ImGuiMouseSource_TouchScreen;
    /** @brief 在 BeginChild 应用上一帧滚动目标之前处理抢停、切页和宿主失效。 */
    if (state.touch.window && (state.touch.page != state.selected || ImGui::IsMouseClicked(0) ||
        io.MouseSource != ImGuiMouseSource_TouchScreen || io.AppFocusLost ||
        io.MouseWheel != 0 || io.MouseWheelH != 0 || !std::isfinite(io.DeltaTime) ||
        io.DeltaTime <= 0 || (state.touch.coasting && io.DeltaTime > .1f) ||
        state.touch.displaySize.x != io.DisplaySize.x || state.touch.displaySize.y != io.DisplaySize.y ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)))
        StopTouchScroll(state);
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
    BeginTouchScroll(state, catchFling);
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
