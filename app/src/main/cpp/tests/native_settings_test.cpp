#include "imgui.h"
#include "monitor/session.h"
#include "monitor/settings_ui.h"
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

int main() {
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1080, 6000);
    io.DeltaTime = 1.0f / 60;
    if (!io.Fonts->Build())
        return 1;
    auto directory = "/tmp/vulkanscope-settings-" + std::to_string(getpid());
    vulkanscope::monitor::NativeSession().initialize(directory, {});
    vulkanscope::monitor::SettingsState settings;
    settings.surfaceKeyboard = settings.powerKeyboard = true;
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Native settings");
        for (const char *name : {"Fields and display order", "Advanced sensors and Surface selection",
                                 "Latest diagnostics", "Last game diagnostics"})
            ImGui::GetStateStorage()->SetInt(ImGui::GetID(name), 1);
        vulkanscope::monitor::DrawSettings(settings);
        ImGui::End();
        ImGui::Render();
        if (!ImGui::GetDrawData()->Valid || ImGui::GetDrawData()->TotalVtxCount == 0 ||
            vulkanscope::monitor::NativeSession().running() ||
            vulkanscope::monitor::NativeSession().config().root)
            throw std::runtime_error(
                "Rendering settings must produce geometry without starting collection or requesting root");
    }
    ImGui::DestroyContext(context);
    std::puts("Native ImGui settings and both touch keyboards rendered successfully");
}
