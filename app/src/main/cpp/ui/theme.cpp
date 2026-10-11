#include "ui/theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "imgui_internal.h"

namespace vulkanscope::ui {
namespace {

ImVec4 Surface() { return ImVec4(.043f, .064f, .085f, 1.0f); }

}

ImVec4 Accent() { return ImVec4(.12f, .68f, .91f, 1.0f); }
ImVec4 Muted() { return ImVec4(.49f, .57f, .64f, 1.0f); }

void ApplyTheme(float scale) {
    auto &style = ImGui::GetStyle();
    style = ImGuiStyle{};
    style.WindowPadding = ImVec2(18, 18);
    style.WindowRounding = 0;
    style.WindowBorderSize = 0;
    style.ChildRounding = 4;
    style.ChildBorderSize = 1;
    style.PopupRounding = 4;
    style.PopupBorderSize = 1;
    style.FramePadding = ImVec2(10, 7);
    style.FrameRounding = 3;
    style.FrameBorderSize = 1;
    style.ItemSpacing = ImVec2(10, 12);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = ImVec2(8, 6);
    style.IndentSpacing = 18;
    style.ScrollbarSize = 5;
    style.ScrollbarRounding = 3;
    style.GrabMinSize = 12;
    style.GrabRounding = 2;
    style.TabRounding = 3;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = ImVec2(0, 6);
    style.DisabledAlpha = .48f;

    auto *colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(.85f, .89f, .93f, 1);
    colors[ImGuiCol_TextDisabled] = Muted();
    colors[ImGuiCol_WindowBg] = ImVec4(.024f, .035f, .047f, 1);
    colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg] = ImVec4(.055f, .080f, .105f, 1);
    colors[ImGuiCol_Border] = ImVec4(.11f, .16f, .20f, 1);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = ImVec4(.030f, .046f, .064f, 1);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(.064f, .108f, .142f, 1);
    colors[ImGuiCol_FrameBgActive] = ImVec4(.075f, .146f, .194f, 1);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = Surface();
    colors[ImGuiCol_TitleBgCollapsed] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_MenuBarBg] = Surface();
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(.14f, .21f, .27f, 1);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.20f, .34f, .42f, 1);
    colors[ImGuiCol_ScrollbarGrabActive] = Accent();
    colors[ImGuiCol_CheckMark] = Accent();
    colors[ImGuiCol_SliderGrab] = Accent();
    colors[ImGuiCol_SliderGrabActive] = ImVec4(.35f, .80f, 1, 1);
    colors[ImGuiCol_Button] = ImVec4(.055f, .135f, .184f, 1);
    colors[ImGuiCol_ButtonHovered] = ImVec4(.067f, .215f, .29f, 1);
    colors[ImGuiCol_ButtonActive] = ImVec4(.077f, .30f, .40f, 1);
    colors[ImGuiCol_Header] = ImVec4(.060f, .090f, .119f, 1);
    colors[ImGuiCol_HeaderHovered] = ImVec4(.068f, .143f, .188f, 1);
    colors[ImGuiCol_HeaderActive] = ImVec4(.075f, .196f, .260f, 1);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_SeparatorHovered] = colors[ImGuiCol_ButtonHovered];
    colors[ImGuiCol_SeparatorActive] = Accent();
    colors[ImGuiCol_ResizeGrip] = ImVec4(.12f, .68f, .91f, .12f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(.12f, .68f, .91f, .50f);
    colors[ImGuiCol_ResizeGripActive] = Accent();
    colors[ImGuiCol_Tab] = Surface();
    colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
    colors[ImGuiCol_TabSelected] = colors[ImGuiCol_Button];
    colors[ImGuiCol_TabSelectedOverline] = Accent();
    colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TabDimmedSelected] = Surface();
    colors[ImGuiCol_TabDimmedSelectedOverline] = Muted();
    colors[ImGuiCol_PlotLines] = Accent();
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(.56f, .88f, 1, 1);
    colors[ImGuiCol_PlotHistogram] = Accent();
    colors[ImGuiCol_PlotHistogramHovered] = colors[ImGuiCol_PlotLinesHovered];
    colors[ImGuiCol_TableHeaderBg] = colors[ImGuiCol_Header];
    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
    colors[ImGuiCol_TableBorderLight] = ImVec4(.10f, .15f, .19f, .5f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(.12f, .18f, .24f, .12f);
    colors[ImGuiCol_TextLink] = Accent();
    colors[ImGuiCol_TextSelectedBg] = ImVec4(.12f, .68f, .91f, .28f);
    colors[ImGuiCol_DragDropTarget] = Accent();
    colors[ImGuiCol_NavCursor] = Accent();
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(.85f, .94f, 1, .7f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(.01f, .025f, .04f, .65f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(.005f, .014f, .022f, .78f);

    style.ScaleAllSizes(std::isfinite(scale) && scale > 0 ? scale : 1.0f);
}

Card::Card(const char *id)
    : window_(ImGui::GetCurrentWindow()), origin_(ImGui::GetCursorScreenPos()),
      workBounds_(window_->WorkRect.Min.x, window_->WorkRect.Max.x),
      contentBounds_(window_->ContentRegionRect.Min.x, window_->ContentRegionRect.Max.x),
      width_(std::max(1.0f, ImGui::GetContentRegionAvail().x)),
      padding_(std::floor(std::min(ImGui::GetFontSize() * .75f, width_ * .2f))) {
    ImGui::PushID(id);
    ImGui::BeginGroup();
    layers_.Split(window_->DrawList, 2);
    layers_.SetCurrentChannel(window_->DrawList, 1);
    window_->WorkRect.Min.x = window_->ContentRegionRect.Min.x = origin_.x + padding_;
    window_->WorkRect.Max.x = window_->ContentRegionRect.Max.x = origin_.x + width_ - padding_;
    /** @brief 整像素内边距避免负屏幕坐标的截断改变卡片高度，使滚动范围保持稳定。 */
    if (padding_ > 0) ImGui::Indent(padding_);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + padding_);
    ImGui::PushItemWidth(-1);
    ImGui::PushTextWrapPos(0);
}

Card::~Card() {
    IM_ASSERT(ImGui::GetCurrentWindow() == window_);
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    const float height = std::max(padding_ * 2, window_->DC.CursorMaxPos.y - origin_.y + padding_);
    window_->WorkRect.Min.x = workBounds_.x;
    window_->WorkRect.Max.x = workBounds_.y;
    window_->ContentRegionRect.Min.x = contentBounds_.x;
    window_->ContentRegionRect.Max.x = contentBounds_.y;
    if (padding_ > 0) ImGui::Unindent(padding_);
    ImGui::SetCursorScreenPos(ImVec2(origin_.x, origin_.y + height));
    ImGui::Dummy(ImVec2(width_, 0));
    ImGui::EndGroup();

    layers_.SetCurrentChannel(window_->DrawList, 0);
    const ImVec2 end(origin_.x + width_, origin_.y + height);
    const auto &style = ImGui::GetStyle();
    window_->DrawList->AddRectFilled(origin_, end, ImGui::GetColorU32(Surface()), style.ChildRounding);
    window_->DrawList->AddRect(origin_, end, ImGui::GetColorU32(ImGuiCol_Border), style.ChildRounding,
                               0, std::max(1.0f, style.ChildBorderSize));
    layers_.Merge(window_->DrawList);
    ImGui::PopID();
}

bool Toggle(const char *label, bool *enabled) {
    auto &g = *ImGui::GetCurrentContext();
    auto *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const auto &style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float inset = std::min(style.FramePadding.x, width * .05f);
    const float trackWidth = std::min(font * 1.85f, width * .32f);
    const float trackHeight = std::min(font * .95f, trackWidth * .54f);
    const float labelWidth = std::max(1.0f, width - trackWidth - style.ItemInnerSpacing.x - inset * 2);
    const char *labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd, false, labelWidth);
    const float height = std::max(font * 1.8f, textSize.y + style.FramePadding.y * 2);
    const auto pos = ImGui::GetCursorScreenPos();
    const ImRect bounds(pos, ImVec2(pos.x + width, pos.y + height));
    const auto id = window->GetID(label);
    ImGui::ItemSize(bounds, style.FramePadding.y);
    if (!ImGui::ItemAdd(bounds, id)) return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bounds, id, &hovered, &held,
                                               ImGuiButtonFlags_PressedOnClickRelease);
    if (pressed) {
        *enabled = !*enabled;
        ImGui::MarkItemEdited(id);
    }
    if (hovered)
        window->DrawList->AddRectFilled(bounds.Min, bounds.Max,
            ImGui::GetColorU32(held ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), style.FrameRounding);
    ImGui::RenderNavCursor(bounds, id);
    const ImVec2 textPos(pos.x + inset, pos.y + (height - textSize.y) * .5f);
    window->DrawList->AddText(ImGui::GetFont(), font, textPos, ImGui::GetColorU32(ImGuiCol_Text),
                              label, labelEnd, labelWidth);

    const ImVec2 trackMin(bounds.Max.x - inset - trackWidth, pos.y + (height - trackHeight) * .5f);
    const ImVec2 trackMax(trackMin.x + trackWidth, trackMin.y + trackHeight);
    const float radius = trackHeight * .5f;
    window->DrawList->AddRectFilled(trackMin, trackMax,
        ImGui::GetColorU32(*enabled ? Accent() : ImVec4(.15f, .21f, .26f, 1)), radius);
    const float knobRadius = radius * .68f;
    const ImVec2 knob(*enabled ? trackMax.x - radius : trackMin.x + radius, trackMin.y + radius);
    window->DrawList->AddCircleFilled(knob, knobRadius,
        ImGui::GetColorU32(*enabled ? ImVec4(.94f, .98f, 1, 1) : ImVec4(.58f, .65f, .71f, 1)));
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags |
        ImGuiItemStatusFlags_Checkable | (*enabled ? ImGuiItemStatusFlags_Checked : 0));
    return pressed;
}

}
