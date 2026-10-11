#include "imgui.h"
#include "ui/navigation.h"
#include "ui/theme.h"
#include <cmath>
#include <stdexcept>
#include <iostream>
using namespace vulkanscope::ui;
namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() {
    ImGui::CreateContext();
    ApplyTheme();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(420, 800);
    io.DeltaTime = 1.f / 60;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    Navigation navigation;
    float scroll = 0, maxScroll = 0, slider = .5f;
    ImVec2 button{}, sliderPos{}, togglePos{};
    bool enabled = false;
    int clicks = 0;
    bool resetScroll = false;
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar);
        auto content = [&] {
            scroll = ImGui::GetScrollY(); maxScroll = ImGui::GetScrollMaxY();
            if (resetScroll) ImGui::SetScrollY(0);
            Card card("test-card");
            if (ImGui::Button("Action", ImVec2(180, 60))) ++clicks;
            const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            button = ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Value", &slider, 0, 1);
            const auto c = ImGui::GetItemRectMin(), d = ImGui::GetItemRectMax();
            sliderPos = ImVec2(c.x + 100, (c.y + d.y) / 2);
            Toggle("Enabled", &enabled);
            const auto t = ImGui::GetItemRectMin(), u = ImGui::GetItemRectMax();
            togglePos = ImVec2((t.x + u.x) / 2, (t.y + u.y) / 2);
            for (int i = 0; i < 100; ++i) ImGui::Text("Content row %d", i);
        };
        DrawPages(navigation, Language::English, content, content);
        ImGui::End();
        ImGui::Render();
    };
    auto position = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frame(); };
    auto down = [&](ImVec2 p) { position(p); io.AddMouseButtonEvent(0, true); frame(); };
    auto up = [&] { io.AddMouseButtonEvent(0, false); frame(); frame(); };
    auto top = [&] { resetScroll = true; frame(); resetScroll = false; frame(); };
    try {
        frame(); frame();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        const float tabY = navigation.tabMin[0].y;
        down(ImVec2(200, 500));
        position(ImVec2(200, 380)); up();
        require(scroll >= 100, "finger drag scrolls home content");
        require(navigation.tabMin[0].y == tabY, "bottom navigation stays fixed");
        const float homeScroll = scroll;
        navigation.selected = Page::Plugins;
        frame(); frame();
        require(scroll == 0, "plugin scroll position is independent");
        down(ImVec2(200, 500)); position(ImVec2(200, 360)); up();
        require(scroll >= 120, "finger drag scrolls plugins");
        navigation.selected = Page::Home;
        frame(); frame();
        require(std::abs(scroll - homeScroll) < 1, "switching pages preserves scroll");
        top();
        down(button); up();
        require(clicks == 1, "stationary touch still clicks buttons");
        const auto start = button;
        down(start); position(ImVec2(start.x, start.y - 60)); up();
        require(clicks == 1 && scroll > 40, "vertical drag starting on a button scrolls without clicking");
        top();
        const auto knob = sliderPos;
        down(knob); position(ImVec2(knob.x + 55, knob.y)); up();
        require(slider > .65f && scroll == 0, "slider retains horizontal interaction");
        down(togglePos); up();
        require(enabled, "themed toggle responds to a tap");
        const auto toggleStart = togglePos;
        down(toggleStart); position(ImVec2(toggleStart.x, toggleStart.y - 60)); up();
        require(enabled && scroll > 40, "drag on themed toggle scrolls without changing plugin state");
        top();
        down(button); CancelPageTouch(navigation); frame(); frame();
        require(clicks == 1 && !navigation.touch.pending, "system cancellation does not activate a button");
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        down(ImVec2(200, 400)); position(ImVec2(200, -10000)); up();
        require(scroll == maxScroll, "drag clamps at bottom");
        down(ImVec2(200, 400)); position(ImVec2(200, 10000)); up();
        require(scroll == 0, "drag clamps at top");
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        down(ImVec2(200, 400)); position(ImVec2(200, 200)); up();
        require(scroll == 0, "mouse dragging is not treated as touch scrolling");
        position(ImVec2(200, 400)); io.AddMouseWheelEvent(0, -2); frame(); frame();
        require(scroll > 0, "mouse wheel still scrolls");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; ImGui::DestroyContext(); return 1;
    }
    ImGui::DestroyContext();
}
