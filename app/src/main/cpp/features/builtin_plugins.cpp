#include "features/builtin_plugins.h"

#include "imgui.h"

#include <array>
#include <utility>

namespace vulkanscope {
namespace {

struct FrameHistory {
    std::array<float, 180> samples{};
    int index = 0;
};

struct DeviceReport {
    std::string text;
    double nextRefresh = 0;
    bool initialized = false;
};

}

void RegisterBuiltinPlugins(plugin::Runtime& runtime, std::shared_ptr<HostReports> reports) {
    runtime.add({"host.reports", {}, [reports = std::move(reports)](plugin::Context& context) {
        context.provide<HostReports>(reports);
    }});
    runtime.add({"frame-metrics", {}, [](plugin::Context& context) {
        auto state = std::make_shared<FrameHistory>();
        context.on<RenderFrame>([state](const RenderFrame& frame) {
            state->samples[state->index] = 1000.0f * frame.deltaSeconds;
            state->index = (state->index + 1) % state->samples.size();
            ImGui::Text("%.1f FPS  |  %.2f ms", frame.framerate,
                        frame.framerate > 0 ? 1000.0f / frame.framerate : 0);
            ImGui::TextWrapped("App render rate / FIFO VSync. This is not a global game FPS monitor.");
            ImGui::PlotLines("##frame", state->samples.data(), state->samples.size(),
                             state->index, "Frame time (ms)", 0, 50,
                             ImVec2(-1, ImGui::GetFontSize() * 4));
        });
    }});
    runtime.add({"device-info", {"host.reports"}, [](plugin::Context& context) {
        auto reports = context.service<HostReports>();
        auto state = std::make_shared<DeviceReport>();
        context.on<RenderFrame>([reports, state](const RenderFrame& frame) {
            if (!state->initialized || frame.time >= state->nextRefresh) {
                reports.borrow([&](HostReports& host) { state->text = host.systemReport(); });
                state->nextRefresh = frame.time + 1.0;
                state->initialized = true;
            }
            if (ImGui::CollapsingHeader("Screen & device", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("%s", state->text.c_str());
                ImGui::Text("Render surface: %d x %d px", frame.surfaceWidth, frame.surfaceHeight);
            }
        });
    }});
    runtime.add({"gpu-info", {}, [](plugin::Context& context) {
        context.on<RenderFrame>([](const RenderFrame& frame) {
            if (ImGui::CollapsingHeader("Vulkan GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("%s", frame.gpuName.c_str());
                ImGui::Text("API: %u.%u.%u", frame.apiVersion >> 22,
                            (frame.apiVersion >> 12) & 0x3ff, frame.apiVersion & 0xfff);
                ImGui::Text("Driver (raw): 0x%08x", frame.driverVersion);
                ImGui::Text("Vendor / device: %04x / %04x", frame.vendorId, frame.deviceId);
                ImGui::Text("Swapchain: %u images / FIFO", frame.imageCount);
            }
        });
    }});
}

}
