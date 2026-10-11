#include "monitor/settings_ui.h"
#include "imgui.h"
#include "monitor/session.h"
#include "monitor/hud_view.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
namespace vulkanscope::monitor {
namespace {
/** @brief ImGui 软键盘用于 Android 原生文本配置，不依赖 EditText 或输入法回调。 */
void keyboard(char *buffer, std::size_t capacity, bool &open, bool &shifted) {
    if (!open)
        return;
    ImGui::TextUnformatted("Touch keyboard (appends at end)");
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", "/._- :[]()"};
    ImGui::PushID(buffer);
    for (const char *row : rows) {
        for (std::size_t i = 0; row[i]; ++i) {
            if (i)
                ImGui::SameLine(0, 2);
            char ch = row[i];
            if (shifted && ch >= 'a' && ch <= 'z')
                ch -= 'a' - 'A';
            char label[] = {ch, 0};
            ImGui::PushID(static_cast<int>(row[i]));
            if (ImGui::Button(ch == ' ' ? "Space" : label) && std::strlen(buffer) + 1 < capacity) {
                auto n = std::strlen(buffer);
                buffer[n] = ch;
                buffer[n + 1] = 0;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button("Shift"))
        shifted = !shifted;
    ImGui::SameLine();
    if (ImGui::Button("Backspace")) {
        auto n = std::strlen(buffer);
        if (n)
            buffer[n - 1] = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        buffer[0] = 0;
    ImGui::SameLine();
    if (ImGui::Button("Done"))
        open = false;
    ImGui::PopID();
}
}
void DrawSettings(SettingsState &state) {
    auto &session = NativeSession();
    auto config = session.config();
    bool changed = false;
    ImGui::SeparatorText("Classic Metal HUD");
    const auto size = MeasureHud(config, ImGui::GetFontSize() * config.textSize / 18.0f, ImGui::GetContentRegionAvail().x, 10000);
    DrawClassicHud(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, config, session.hud());
    ImGui::Dummy(ImVec2(size.width, size.height));
    changed |= ImGui::Checkbox("Frame history graph", &config.hudGraph);
    changed |= ImGui::Checkbox("Additional sensor rows", &config.hudExtras);
    changed |= ImGui::Checkbox("Enable read-only root sampling", &config.root);
    ImGui::TextWrapped("Root is optional. FPS and engine inspection need an existing su provider. Enabling "
                       "this option authorizes read-only queries while the HUD runs. To retry a denied "
                       "authorization, disable it for one sample, then re-enable it.");
    const char *intervals[] = {"500 ms", "1000 ms", "2000 ms"};
    int interval = config.interval == 500 ? 0 : config.interval == 2000 ? 2 : 1;
    if (ImGui::Combo("Sample interval", &interval, intervals, 3)) {
        config.interval = interval == 0 ? 500 : interval == 2 ? 2000 : 1000;
        changed = true;
    }
    changed |= ImGui::SliderInt("Text size", &config.textSize, 10, 24);
    changed |= ImGui::SliderInt("Background opacity", &config.opacity, 20, 100, "%d%%");
    const char *corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    changed |= ImGui::Combo("Corner", &config.corner, corners, 4);
    ImGui::TextWrapped(
        "Changes are saved in native storage. The overlay is touch-through; configure it here.");
    if (ImGui::TreeNode("Fields and display order")) {
        ImGui::TextWrapped("Core rows keep their classic positions. Disabled values show --. Enable Additional sensor rows to show other fields in the order below.");
        for (std::size_t i = 0; i < config.order.size(); ++i) {
            const auto id = config.order[i];
            auto metric =
                std::find_if(Metrics().begin(), Metrics().end(), [&](const auto &m) { return id == m.id; });
            if (metric == Metrics().end())
                continue;
            ImGui::PushID(id.c_str());
            changed |= ImGui::Checkbox(metric->label, &config.enabled[id]);
            ImGui::SameLine();
            if (ImGui::SmallButton("Up") && i > 0) {
                std::swap(config.order[i], config.order[i - 1]);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Down") && i + 1 < config.order.size()) {
                std::swap(config.order[i], config.order[i + 1]);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Advanced sensors and Surface selection")) {
        std::array<char, 513> surface{}, power{};
        std::snprintf(surface.data(), surface.size(), "%s", config.surface.c_str());
        std::snprintf(power.data(), power.size(), "%s", config.powerPath.c_str());
        ImGui::InputText("Exact foreground Surface", surface.data(), surface.size());
        if (ImGui::Button("Edit Surface name"))
            state.surfaceKeyboard = !state.surfaceKeyboard;
        ImGui::SameLine();
        if (ImGui::Button("Paste Surface")) {
            const char *text = ImGui::GetClipboardText();
            if (text)
                std::snprintf(surface.data(), surface.size(), "%s", text);
        }
        keyboard(surface.data(), surface.size(), state.surfaceKeyboard, state.shifted);
        ImGui::InputText("CPU power sensor (/sys/...)", power.data(), power.size());
        if (ImGui::Button("Edit sensor path"))
            state.powerKeyboard = !state.powerKeyboard;
        ImGui::SameLine();
        if (ImGui::Button("Paste path")) {
            const char *text = ImGui::GetClipboardText();
            if (text)
                std::snprintf(power.data(), power.size(), "%s", text);
        }
        keyboard(power.data(), power.size(), state.powerKeyboard, state.shifted);
        if (config.surface != surface.data() || config.powerPath != power.data()) {
            config.surface = surface.data();
            config.powerPath = power.data();
            changed = true;
        }
        changed |=
            ImGui::InputDouble("Sensor to watts multiplier", &config.powerScale, 0.000001, 0.001, "%.9f");
        if (ImGui::Button("Watts")) {
            config.powerScale = 1;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Milliwatts")) {
            config.powerScale = .001;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Microwatts")) {
            config.powerScale = .000001;
            changed = true;
        }
        ImGui::TextWrapped("Battery power is a whole-device estimate, never a CPU power substitute. Leave "
                           "the Surface override empty for automatic selection.");
        ImGui::TreePop();
    }
    if (ImGui::Button("Game fields")) {
        for (auto &item : config.enabled)
            item.second = false;
        for (const char *id : {"foreground", "fps", "frame_time", "frame_p95", "fps_source",
                               "engine_resolution", "engine_source", "battery_temp"})
            config.enabled[id] = true;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Hardware fields")) {
        for (auto &item : config.enabled)
            item.second = false;
        for (const char *id : {"gpu_model", "cpu_freq", "cpu_load", "cpu_power", "gpu_freq", "gpu_load",
                               "memory", "battery_power", "battery_temp", "thermal"})
            config.enabled[id] = true;
        changed = true;
    }
    if (ImGui::Button("Restore defaults")) {
        config = Config{};
        changed = true;
    }
    if (changed)
        session.configure(std::move(config));
    if (ImGui::TreeNode("Latest diagnostics")) {
        const auto text = session.diagnostics();
        if (ImGui::Button("Copy latest diagnostics"))
            ImGui::SetClipboardText(text.c_str());
        ImGui::TextWrapped("%s", text.empty() ? "No sample yet" : text.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Last game diagnostics")) {
        const auto text = session.diagnostics(true);
        if (ImGui::Button("Copy game diagnostics"))
            ImGui::SetClipboardText(text.c_str());
        ImGui::TextWrapped("%s", text.empty() ? "No game sample yet" : text.c_str());
        ImGui::TreePop();
    }
}
}
