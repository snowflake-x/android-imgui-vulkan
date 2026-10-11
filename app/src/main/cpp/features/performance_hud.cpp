#include "features/performance_hud.h"

#include "features/builtin_plugins.h"
#include "imgui.h"

namespace vulkanscope {
namespace {

/**
 * @brief 每次插件加载独立持有的只读报告缓存，不拥有 Android 采集会话。
 */
struct HudReport {
    std::string text;
    double nextRefresh = 0;
    bool initialized = false;
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
            if (!state->initialized || frame.time >= state->nextRefresh) {
                host.borrow([&](HostPerformanceHud& value) { state->text = value.report(); });
                state->nextRefresh = frame.time + 1.0;
                state->initialized = true;
            }
            if (!ImGui::CollapsingHeader("Foreground performance HUD", ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            ImGui::PushID("performance-hud");
            ImGui::TextWrapped("Start a HUD session, then switch to your game. Stop it here or from its notification.");
            if (ImGui::Button("Configure")) {
                host.borrow([](HostPerformanceHud& value) { value.configure(); });
                state->initialized = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Start HUD")) {
                host.borrow([](HostPerformanceHud& value) { value.start(); });
                state->initialized = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop HUD")) {
                host.borrow([](HostPerformanceHud& value) { value.stop(); });
                state->initialized = false;
            }
            ImGui::TextWrapped("%s", state->text.c_str());
            ImGui::PopID();
        });
    }});
}

}
