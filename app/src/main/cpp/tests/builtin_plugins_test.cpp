#include "features/builtin_plugins.h"

#include "imgui.h"

#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using vulkanscope::HostReports;
using vulkanscope::RegisterBuiltinPlugins;
using vulkanscope::RenderFrame;
using vulkanscope::plugin::Runtime;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/** @brief Supply only ImGui core state; no Android, Vulkan, or platform backend is needed. */
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

    /** @brief Render a host value snapshot and return the height occupied by feature content. */
    float render(Runtime& runtime, double time) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 680));
        ImGui::Begin("Builtin plugins test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const float before = ImGui::GetCursorPosY();
        runtime.emit(RenderFrame{
            1.0f / 60.0f, 60.0f, time, 1280, 720, "Headless test GPU",
            (1u << 22) | (3u << 12), 0x12345678u, 0x1234u, 0x5678u, 3u
        });
        const float height = ImGui::GetCursorPosY() - before;
        ImGui::End();
        ImGui::Render();
        require(ImGui::GetDrawData() && ImGui::GetDrawData()->Valid,
                "builtins must generate valid headless draw data");
        require(ImGui::GetDrawData()->TotalVtxCount > 0,
                "headless window must produce draw vertices");
        return height;
    }

private:
    ImGuiContext* context_;
};

void registrationAndLifecycleDoNotReadHost() {
    require(ImGui::GetCurrentContext() == nullptr,
            "registration test must run without a graphics context");
    int reads = 0;
    Runtime runtime;
    RegisterBuiltinPlugins(runtime, std::make_shared<HostReports>([&] {
        ++reads;
        return "device report";
    }));
    require(runtime.loadedCount() == 0 && reads == 0,
            "registration must neither activate features nor read the host");

    for (int cycle = 0; cycle != 3; ++cycle) {
        runtime.load("frame-metrics");
        runtime.load("device-info");
        runtime.load("gpu-info");
        runtime.load("device-info");
        require(runtime.loadedCount() == 4 && runtime.isLoaded("host.reports"),
                "device feature must load its host report dependency exactly once");
        require(reads == 0, "plugin activation must not read the host");
        runtime.unload("device-info");
        runtime.unload("frame-metrics");
        runtime.unload("gpu-info");
        runtime.unload("host.reports");
        runtime.emit(RenderFrame{});
        runtime.clear();
        require(runtime.loadedCount() == 0 && reads == 0,
                "unload must revoke all callbacks without touching host state");
    }
}

void reportReadsAreThrottledAndReloadStartsFresh(const std::string& report) {
    HeadlessGui gui;
    int reads = 0;
    Runtime runtime;
    RegisterBuiltinPlugins(runtime, std::make_shared<HostReports>([&] {
        ++reads;
        return report;
    }));
    runtime.load("frame-metrics");
    runtime.load("device-info");
    runtime.load("gpu-info");
    require(reads == 0, "loading builtins must not query the host");
    gui.render(runtime, 10.0);
    require(reads == 1, "first rendered frame must read the device report once");
    gui.render(runtime, 10.25);
    gui.render(runtime, 10.999);
    require(reads == 1, "rendered frames within one second must reuse the report");
    gui.render(runtime, 11.0);
    require(reads == 2, "report must refresh when its one second deadline is reached");

    runtime.unload("device-info");
    require(reads == 2, "disabling device information must not query the host");
    gui.render(runtime, 11.1);
    gui.render(runtime, 11.2);
    require(reads == 2, "disabled device information must stop all report reads");
    runtime.load("device-info");
    require(reads == 2, "reloading device information must not query the host");
    gui.render(runtime, 11.3);
    require(reads == 3, "reloading must discard the prior report and refresh deadline");
    gui.render(runtime, 11.4);
    require(reads == 3, "a reloaded device callback must still throttle host reads");

    runtime.clear();
    gui.render(runtime, 100.0);
    require(reads == 3, "clearing builtins must leave no report callbacks");
}

void independentFeaturesRenderWithoutHostReads() {
    HeadlessGui gui;
    int reads = 0;
    Runtime runtime;
    RegisterBuiltinPlugins(runtime, std::make_shared<HostReports>([&] {
        ++reads;
        return "unused report";
    }));
    require(gui.render(runtime, 0.0) == 0,
            "unloaded plugins must not render feature content");
    for (const auto* id : {"frame-metrics", "gpu-info"}) {
        runtime.load(id);
        require(runtime.loadedCount() == 1 && !runtime.isLoaded("host.reports"),
                "frame and GPU features must not activate the report service");
        require(gui.render(runtime, 1.0) > 0,
                "enabled frame and GPU features must render from value snapshots");
        runtime.unload(id);
        require(gui.render(runtime, 2.0) == 0,
                "unloading a feature must remove its rendered content");
    }
    require(reads == 0, "frame and GPU rendering must never query host reports");
}

void repeatedGraphicsAndRuntimeLifetimesReleaseState() {
    int reads = 0;
    for (int cycle = 0; cycle != 32; ++cycle) {
        std::weak_ptr<HostReports> retiredReports;
        {
            HeadlessGui gui;
            Runtime runtime;
            auto reports = std::make_shared<HostReports>([&] {
                ++reads;
                return "device report";
            });
            retiredReports = reports;
            RegisterBuiltinPlugins(runtime, std::move(reports));
            runtime.load("device-info");
            runtime.load("frame-metrics");
            runtime.load("gpu-info");
            gui.render(runtime, 0.0);
            gui.render(runtime, 0.5);
            require(reads == cycle + 1,
                    "each fresh graphics/runtime lifetime must have one fresh device cache");
        }
        require(ImGui::GetCurrentContext() == nullptr && retiredReports.expired(),
                "destroying runtime before ImGui must release every retained host capability");
        require(reads == cycle + 1, "runtime destruction must not read host reports");
    }
}

}

int main() {
    const std::array<std::pair<const char*, void (*)()>, 5> tests{{
        {"registration and lifecycle have no host reads", registrationAndLifecycleDoNotReadHost},
        {"device reports throttle and reset", [] { reportReadsAreThrottledAndReloadStartsFresh("report"); }},
        {"empty device reports still throttle", [] { reportReadsAreThrottledAndReloadStartsFresh(""); }},
        {"independent features render without host reads", independentFeaturesRenderWithoutHostReads},
        {"repeated runtime and graphics lifetimes", repeatedGraphicsAndRuntimeLifetimesReleaseState}
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
