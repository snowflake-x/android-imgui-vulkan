#include "ui/i18n.h"
#include "features/performance_hud.h"

#include "features/builtin_plugins.h"
#include "imgui.h"

namespace vulkanscope {
using ui::Text; using ui::Label;
namespace {

/**
 * @brief 每次插件加载独立持有的只读报告缓存，不拥有 Android 采集会话。
 */
struct HudReport {
    std::string text;
    double nextRefresh = 0;
    bool initialized = false;
    bool settings = false;
};

}

void RegisterPerformanceHudPlugin(plugin::Runtime& runtime,
                                  std::shared_ptr<HostPerformanceHud> host) {
    runtime.add({"host.performance-hud", {}, [host = std::move(host)](plugin::Context& context) {
        context.provide<HostPerformanceHud>(host);
    }});
    runtime.add({"performance-hud", {"host.performance-hud"}, [](plugin::Context& context) {
        auto host = context.service<HostPerformanceHud>();
        auto state = std::make_shared<HudReport>();
        context.on<RenderFrame>([host, state](const RenderFrame& frame) {
            const auto language = frame.language;
            if (!state->initialized || frame.time >= state->nextRefresh) {
                host.borrow([&](HostPerformanceHud& value) { state->text = value.report(); });
                state->nextRefresh = frame.time + 1.0;
                state->initialized = true;
            }
            if (!ImGui::CollapsingHeader(Label(language, "Foreground performance HUD").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            ImGui::PushID("performance-hud");
            ImGui::TextWrapped("%s", Text(language, "Start a HUD session, then switch to your game. Stop it here or from its notification."));
            if (ImGui::Button(Label(language, "Configure").c_str())) {
                state->settings = !state->settings;
                state->initialized = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(Label(language, "Start HUD").c_str())) {
                host.borrow([](HostPerformanceHud& value) { value.start(); });
                state->initialized = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(Label(language, "Stop HUD").c_str())) {
                host.borrow([](HostPerformanceHud& value) { value.stop(); });
                state->initialized = false;
            }
            bool preview = false;
            if (!state->settings) host.borrow([&](HostPerformanceHud& value) { preview = value.preview(); });
            if (!preview && !state->settings) ImGui::TextWrapped(Text(language, "%s"), state->text.c_str());
            if (ImGui::TreeNode(Label(language, "Sample details").c_str())) {
                ImGui::TextWrapped(Text(language, "%s"), state->text.c_str());
                ImGui::TreePop();
            }
            if (state->settings) host.borrow([](HostPerformanceHud& value) { value.configure(); });
            ImGui::PopID();
        });
    }});
}

}
