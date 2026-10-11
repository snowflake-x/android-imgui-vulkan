#include "features/builtin_plugins.h"
#include "features/performance_hud.h"

#include "imgui.h"

#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using vulkanscope::HostPerformanceHud;
using vulkanscope::RegisterPerformanceHudPlugin;
using vulkanscope::RenderFrame;
using vulkanscope::plugin::Runtime;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/**
 * @brief 区分只读快照访问与会改变 Android 会话的用户动作。
 */
struct HostObservation {
    int reads = 0;
    int configurations = 0;
    int starts = 0;
    int stops = 0;
    bool active = false;
    std::string report = "Foreground sample";
};

std::shared_ptr<HostPerformanceHud> createHost(HostObservation& observation) {
    return std::make_shared<HostPerformanceHud>(
        [&] { ++observation.reads; return observation.report; },
        [&] { ++observation.configurations; },
        [&] { ++observation.starts; observation.active = true; },
        [&] { ++observation.stops; observation.active = false; });
}

/**
 * @brief 只创建 ImGui 核心上下文，验证插件不依赖 Android 或 Vulkan 句柄。
 */
class HeadlessGui {
public:
    HeadlessGui() : context_(ImGui::CreateContext()) {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        if (!io.Fonts->Build()) {
            ImGui::DestroyContext(context_);
            throw std::runtime_error("headless font atlas must build");
        }
    }

    ~HeadlessGui() { ImGui::DestroyContext(context_); }

    HeadlessGui(const HeadlessGui&) = delete;
    HeadlessGui& operator=(const HeadlessGui&) = delete;

    float render(Runtime& runtime, double time) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 680));
        ImGui::Begin("HUD lifecycle test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const float before = ImGui::GetCursorPosY();
        RenderFrame frame;
        frame.time = time;
        runtime.emit(frame);
        const float height = ImGui::GetCursorPosY() - before;
        ImGui::End();
        ImGui::Render();
        require(ImGui::GetDrawData() && ImGui::GetDrawData()->Valid,
                "HUD must generate valid draw data without a platform backend");
        return height;
    }

private:
    ImGuiContext* context_;
};

void registrationAndLifecycleHaveNoHostActions() {
    require(ImGui::GetCurrentContext() == nullptr,
            "registration and lifecycle must not require a GUI context");
    HostObservation observation;
    Runtime runtime;
    RegisterPerformanceHudPlugin(runtime, createHost(observation));
    require(runtime.loadedCount() == 0, "registration must not load the HUD");
    for (int cycle = 0; cycle < 3; ++cycle) {
        runtime.load("performance-hud");
        runtime.load("performance-hud");
        require(runtime.loadedCount() == 2 && runtime.isLoaded("host.performance-hud"),
                "HUD must load its typed host dependency once");
        runtime.unload("performance-hud");
        runtime.emit(RenderFrame{});
        require(runtime.loadedCount() == 0, "HUD unload must release its automatic dependency");
    }
    runtime.load("performance-hud");
    runtime.clear();
    require(observation.reads == 0 && observation.configurations == 0 &&
            observation.starts == 0 && observation.stops == 0,
            "loading, unloading and clearing must not query Android or mutate its session");
}

void snapshotsThrottleAndReloadStartsFresh(const std::string& report) {
    HeadlessGui gui;
    HostObservation observation;
    observation.report = report;
    Runtime runtime;
    RegisterPerformanceHudPlugin(runtime, createHost(observation));
    require(gui.render(runtime, 9.0) == 0, "an unloaded HUD must not render");
    runtime.load("performance-hud");
    require(observation.reads == 0, "loading must not read the Android snapshot");
    require(gui.render(runtime, 10.0) > 0, "an enabled HUD must render its controls");
    gui.render(runtime, 10.25);
    gui.render(runtime, 10.999);
    require(observation.reads == 1, "frames within one second must use the same snapshot");
    gui.render(runtime, 11.0);
    require(observation.reads == 2, "the snapshot must refresh at its one-second deadline");
    runtime.unload("performance-hud");
    require(gui.render(runtime, 100.0) == 0 && observation.reads == 2,
            "unloading must revoke rendering and report reads");
    runtime.load("performance-hud");
    gui.render(runtime, 0.0);
    require(observation.reads == 3,
            "reload must create a fresh cache even after the host clock restarts");
    gui.render(runtime, 0.5);
    require(observation.reads == 3, "the fresh cache must throttle report reads");
    runtime.clear();
    gui.render(runtime, 1000.0);
    require(observation.reads == 3, "clear must leave no report callbacks");
    require(observation.configurations == 0 && observation.starts == 0 && observation.stops == 0,
            "rendering and lifecycle must never start permissions, a sampler, or stop a host session");
}

void nativePreviewHasNoHostActions() {
    HeadlessGui gui;
    Runtime runtime;
    int previews = 0, actions = 0;
    auto host = std::make_shared<HostPerformanceHud>([] { return "sample"; },
        [&] { ++actions; }, [&] { ++actions; }, [&] { ++actions; },
        [&] { ++previews; ImGui::TextUnformatted("native preview"); });
    RegisterPerformanceHudPlugin(runtime, host);
    runtime.load("performance-hud");
    gui.render(runtime, 0);
    require(previews == 1 && actions == 0, "native preview only draws; no permissions or session actions");
    runtime.unload("performance-hud");
    gui.render(runtime, 1);
    require(previews == 1, "unloading revokes preview callback");
}

void graphicsTeardownPreservesUserSession() {
    HostObservation observation;
    auto host = createHost(observation);
    host->start();
    for (int cycle = 0; cycle < 8; ++cycle) {
        HeadlessGui gui;
        Runtime runtime;
        RegisterPerformanceHudPlugin(runtime, host);
        runtime.load("performance-hud");
        gui.render(runtime, 0.0);
        runtime.unload("performance-hud");
        require(observation.active && observation.stops == 0,
                "runtime unload alone must preserve the explicitly started Android session");
        runtime.load("performance-hud");
        gui.render(runtime, 0.5);
    }
    require(observation.active && observation.starts == 1 && observation.stops == 0,
            "surface and runtime destruction must preserve the host-owned game session");
    require(observation.reads == 16 && observation.configurations == 0,
            "each fresh plugin lifetime must own its report cache without opening settings");
    host->stop();
    require(!observation.active && observation.stops == 1,
            "an explicit host stop must end the persistent user session");
}

}

int main() {
    const std::array<std::pair<const char*, void (*)()>, 5> tests{{
        {"registration and lifecycle have no host actions", registrationAndLifecycleHaveNoHostActions},
        {"native preview has no host actions", nativePreviewHasNoHostActions},
        {"HUD snapshots throttle and reset", [] { snapshotsThrottleAndReloadStartsFresh("report"); }},
        {"empty HUD snapshots still throttle", [] { snapshotsThrottleAndReloadStartsFresh(""); }},
        {"graphics teardown preserves the user session", graphicsTeardownPreservesUserSession}
    }};
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
