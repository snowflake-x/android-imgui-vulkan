#include "ui/navigation.h"
#include <algorithm>
namespace vulkanscope::ui {
void DrawPages(Navigation &state, Language language, const std::function<void()> &home,
               const std::function<void()> &plugins) {
    ImGui::TextColored(ImVec4(.59f, .85f, .67f, 1), "VULKAN SCOPE");
    const float bar = ImGui::GetFontSize() * 3.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float height = std::max(1.0f, ImGui::GetContentRegionAvail().y - bar - gap);
    const char *pageId = state.selected == Page::Home ? "home-content" : "plugin-content";
    ImGui::BeginChild(pageId, ImVec2(0, height));
    ImGui::TextUnformatted(Text(language, state.selected == Page::Home ? "Overview" : "Plugins"));
    ImGui::TextWrapped("%s", Text(language, state.selected == Page::Home
                                                ? "Monitor your device and foreground applications."
                                                : "Manage the features shown on your home page."));
    ImGui::Separator();
    if (state.selected == Page::Home)
        home();
    else
        plugins();
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
