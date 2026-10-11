#include "ui/widgets.h"
#include "ui/theme.h"
#include "ui/i18n.h"
#include "monitor/settings_ui.h"
#include "imgui.h"
#include "monitor/session.h"
#include "monitor/hud_view.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
namespace vulkanscope::monitor {
using ui::Text; using ui::Label;
namespace {
/** @brief ImGui 软键盘用于 Android 原生文本配置，不依赖 EditText 或输入法回调。 */
void keyboard(char *buffer, std::size_t capacity, bool &open, bool &shifted, ui::Language language) {
    if (!open)
        return;
    ImGui::TextUnformatted(Text(language, "Touch keyboard (appends at end)"));
    const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", "/._- :[]()"};
    ImGui::PushID(buffer);
    for (const char *row : rows) {
        const float keyWidth = std::max(1.f, (ImGui::GetContentRegionAvail().x - 2.f * (std::strlen(row) - 1)) / std::strlen(row));
        for (std::size_t i = 0; row[i]; ++i) {
            if (i)
                ImGui::SameLine(0, 2);
            char ch = row[i];
            if (shifted && ch >= 'a' && ch <= 'z')
                ch -= 'a' - 'A';
            char label[] = {ch, 0};
            ImGui::PushID(static_cast<int>(row[i]));
            if (ImGui::Button(ch == ' ' ? Text(language, "Space") : label, ImVec2(keyWidth, ImGui::GetFrameHeight())) && std::strlen(buffer) + 1 < capacity) {
                auto n = std::strlen(buffer);
                buffer[n] = ch;
                buffer[n + 1] = 0;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button(Label(language, "Shift").c_str()))
        shifted = !shifted;
    ui::SameLineIfFits(Text(language, "Backspace"));
    if (ImGui::Button(Label(language, "Backspace").c_str())) {
        auto n = std::strlen(buffer);
        if (n) {
            --n;
            while (n && (static_cast<unsigned char>(buffer[n]) & 0xc0) == 0x80) --n;
            buffer[n] = 0;
        }
    }
    ui::SameLineIfFits(Text(language, "Clear"));
    if (ImGui::Button(Label(language, "Clear").c_str()))
        buffer[0] = 0;
    ui::SameLineIfFits(Text(language, "Done"));
    if (ImGui::Button(Label(language, "Done").c_str()))
        open = false;
    ImGui::PopID();
}
}
void DrawSettings(SettingsState &state, ui::Language language, ImFont *hudFont) {
    auto &session = NativeSession();
    auto config = session.config();
    bool changed = false;
    ImGui::SeparatorText(Text(language, "Classic Metal HUD"));
    const auto size = MeasureHud(config, ImGui::GetFontSize() * config.textSize / 18.0f, ImGui::GetContentRegionAvail().x, 10000);
    DrawClassicHud(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, config, session.hud(), hudFont);
    ImGui::Dummy(ImVec2(size.width, size.height));
    changed |= ui::Toggle(Label(language, "Frame history graph").c_str(), &config.hudGraph);
    changed |= ui::Toggle(Label(language, "Additional sensor rows").c_str(), &config.hudExtras);
    changed |= ui::Toggle(Label(language, "Enable read-only root sampling").c_str(), &config.root);
    ImGui::TextWrapped("%s", Text(language, "Root is optional. FPS and engine inspection need an existing su provider. Enabling this option authorizes read-only queries while the HUD runs. To retry a denied authorization, disable it for one sample, then re-enable it."));
    const char *intervals[] = {"500 ms", "1000 ms", "2000 ms"};
    int interval = config.interval == 500 ? 0 : config.interval == 2000 ? 2 : 1;
    if (ImGui::Combo(vulkanscope::ui::FieldLabel(language, "Sample interval").c_str(), &interval, intervals, 3)) {
        config.interval = interval == 0 ? 500 : interval == 2 ? 2000 : 1000;
        changed = true;
    }
    changed |= ImGui::SliderInt(vulkanscope::ui::FieldLabel(language, "Text size").c_str(), &config.textSize, 10, 24);
    changed |= ImGui::SliderInt(vulkanscope::ui::FieldLabel(language, "Background opacity").c_str(), &config.opacity, 20, 100, "%d%%");
    const char *corners[] = {Text(language, "Top left"), Text(language, "Top right"), Text(language, "Bottom left"), Text(language, "Bottom right")};
    changed |= ImGui::Combo(vulkanscope::ui::FieldLabel(language, "Corner").c_str(), &config.corner, corners, 4);
    ImGui::TextWrapped("%s", Text(language, "Changes are saved in native storage. The overlay is touch-through; configure it here."));
    if (ImGui::TreeNode(Label(language, "Fields and display order").c_str())) {
        ImGui::TextWrapped("%s", Text(language, "Core rows keep their classic positions. Disabled values show OFF; unavailable values show --. Enable Additional sensor rows to show other fields in the order below."));
        for (std::size_t i = 0; i < config.order.size(); ++i) {
            const auto id = config.order[i];
            auto metric =
                std::find_if(Metrics().begin(), Metrics().end(), [&](const auto &m) { return id == m.id; });
            if (metric == Metrics().end())
                continue;
            ImGui::PushID(id.c_str());
            changed |= ui::Toggle(Label(language, metric->label).c_str(), &config.enabled[id]);
            if (ImGui::SmallButton(Label(language, "Up").c_str()) && i > 0) {
                std::swap(config.order[i], config.order[i - 1]);
                changed = true;
            }
            ui::SameLineIfFits(Text(language, "Down"));
            if (ImGui::SmallButton(Label(language, "Down").c_str()) && i + 1 < config.order.size()) {
                std::swap(config.order[i], config.order[i + 1]);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode(Label(language, "Advanced sensors and Surface selection").c_str())) {
        std::array<char, 513> surface{}, power{};
        std::snprintf(surface.data(), surface.size(), "%s", config.surface.c_str());
        std::snprintf(power.data(), power.size(), "%s", config.powerPath.c_str());
        ImGui::InputText(vulkanscope::ui::FieldLabel(language, "Exact foreground Surface").c_str(), surface.data(), surface.size());
        if (ImGui::Button(Label(language, "Edit Surface name").c_str()))
            state.surfaceKeyboard = !state.surfaceKeyboard;
        ui::SameLineIfFits(Text(language, "Paste Surface"));
        if (ImGui::Button(Label(language, "Paste Surface").c_str())) {
            const char *text = ImGui::GetClipboardText();
            if (text)
                std::snprintf(surface.data(), surface.size(), "%s", text);
        }
        keyboard(surface.data(), surface.size(), state.surfaceKeyboard, state.shifted, language);
        ImGui::InputText(vulkanscope::ui::FieldLabel(language, "CPU power sensor (/sys/...)").c_str(), power.data(), power.size());
        if (ImGui::Button(Label(language, "Edit sensor path").c_str()))
            state.powerKeyboard = !state.powerKeyboard;
        ui::SameLineIfFits(Text(language, "Paste path"));
        if (ImGui::Button(Label(language, "Paste path").c_str())) {
            const char *text = ImGui::GetClipboardText();
            if (text)
                std::snprintf(power.data(), power.size(), "%s", text);
        }
        keyboard(power.data(), power.size(), state.powerKeyboard, state.shifted, language);
        if (config.surface != surface.data() || config.powerPath != power.data()) {
            config.surface = surface.data();
            config.powerPath = power.data();
            changed = true;
        }
        changed |=
            ImGui::InputDouble(vulkanscope::ui::FieldLabel(language, "Sensor to watts multiplier").c_str(), &config.powerScale, 0.000001, 0.001, "%.9f");
        if (ImGui::Button(Label(language, "Watts").c_str())) {
            config.powerScale = 1;
            changed = true;
        }
        ui::SameLineIfFits(Text(language, "Milliwatts"));
        if (ImGui::Button(Label(language, "Milliwatts").c_str())) {
            config.powerScale = .001;
            changed = true;
        }
        ui::SameLineIfFits(Text(language, "Microwatts"));
        if (ImGui::Button(Label(language, "Microwatts").c_str())) {
            config.powerScale = .000001;
            changed = true;
        }
        ImGui::TextWrapped("%s", Text(language, "Battery power is a whole-device estimate, never a CPU power substitute. Leave the Surface override empty for automatic selection."));
        ImGui::TreePop();
    }
    ImGui::TextWrapped("%s", Text(language, "Graphics tracing samples calls for 200 ms. Requires root and kernel uprobes; adds tracing overhead."));
    if (ImGui::Button(Label(language, "Game fields").c_str())) {
        ApplyMetricPreset(config, MetricPreset::Game);
        changed = true;
    }
    ui::SameLineIfFits(Text(language, "Hardware fields"));
    if (ImGui::Button(Label(language, "Hardware fields").c_str())) {
        ApplyMetricPreset(config, MetricPreset::Hardware);
        changed = true;
    }
    if (ImGui::Button(Label(language, "Restore defaults").c_str())) {
        config = Config{};
        changed = true;
    }
    if (changed)
        session.configure(std::move(config));
    if (ImGui::TreeNode(Label(language, "Latest diagnostics").c_str())) {
        const auto text = session.diagnostics();
        if (ImGui::Button(Label(language, "Copy latest diagnostics").c_str()))
            ImGui::SetClipboardText(text.c_str());
        ImGui::TextWrapped(Text(language, "%s"), text.empty() ? Text(language, "No sample yet") : text.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNode(Label(language, "Last game diagnostics").c_str())) {
        const auto text = session.diagnostics(true);
        if (ImGui::Button(Label(language, "Copy game diagnostics").c_str()))
            ImGui::SetClipboardText(text.c_str());
        ImGui::TextWrapped(Text(language, "%s"), text.empty() ? Text(language, "No game sample yet") : text.c_str());
        ImGui::TreePop();
    }
}
}
