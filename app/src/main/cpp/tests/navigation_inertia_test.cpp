#include "imgui.h"
#include "ui/navigation.h"
#include "ui/theme.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vulkanscope::ui;
namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

/** @brief 通过真实 ImGui 帧及触摸事件观察滚动位置，不依赖惯性内部状态。 */
class PageFixture {
public:
    explicit PageFixture(int refresh = 60) : rate(refresh) {
        ImGui::CreateContext();
        ApplyTheme();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(420, 800);
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        frame(); frame();
    }
    ~PageFixture() { ImGui::DestroyContext(); }
    PageFixture(const PageFixture &) = delete;
    PageFixture &operator=(const PageFixture &) = delete;

    void frame(float elapsed = 0) {
        auto &io = ImGui::GetIO();
        io.DeltaTime = elapsed > 0 ? elapsed : 1.f / static_cast<float>(rate);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar);
        if (openPopup) { ImGui::OpenPopup("Interrupt scrolling"); openPopup = false; }
        auto content = [&] {
            scroll = ImGui::GetScrollY();
            maxScroll = ImGui::GetScrollMaxY();
            float nearest = 10000;
            for (int i = 0; i < 400; ++i) {
                ImGui::PushID(i);
                if (ImGui::Button("Action", ImVec2(180, 32))) ++clicks;
                const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                const ImVec2 center((a.x + b.x) * .5f, (a.y + b.y) * .5f);
                const float distance = std::abs(center.y - 450);
                if (distance < nearest) { nearest = distance; visibleButton = center; }
                ImGui::PopID();
            }
        };
        DrawPages(navigation, Language::English, content, content);
        if (ImGui::BeginPopup("Interrupt scrolling")) {
            ImGui::TextUnformatted("Popup retains its own interaction");
            if (closePopup) { ImGui::CloseCurrentPopup(); closePopup = false; }
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::Render();
    }

    void frames(int count) { for (int i = 0; i < count; ++i) frame(); }
    void position(ImVec2 point) {
        ImGui::GetIO().AddMousePosEvent(point.x, point.y);
        frame();
    }
    void down(ImVec2 point) {
        /** @brief ImGui 对触屏的位置与按下事件分帧处理，先提交位置再观察按下帧。 */
        position(point);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame();
    }
    void up(float elapsed = 0) { ImGui::GetIO().AddMouseButtonEvent(0, false); frame(elapsed); }
    void drag(float speed = 900) {
        down(ImVec2(260, 620));
        const int count = rate / 5;
        for (int i = 1; i <= count; ++i)
            position(ImVec2(260, 620 - speed * static_cast<float>(i) / static_cast<float>(rate)));
    }
    void fling(float speed = 900) { drag(speed); up(); }
    void requireStopped(const char *message) {
        /** @brief 消化 SetScrollY 下一帧生效的请求，再检查持续静止。 */
        frames(2);
        const float stopped = scroll;
        frames(rate / 2);
        require(std::abs(scroll - stopped) <= 1, message);
    }

    Navigation navigation;
    const int rate;
    float scroll = 0, maxScroll = 0;
    int clicks = 0;
    ImVec2 visibleButton{};
    bool openPopup = false, closePopup = false;
};

void releaseContinuesAndDecelerates() {
    PageFixture page;
    page.fling();
    const float released = page.scroll;
    page.frames(6);
    const float first = page.scroll;
    page.frames(12);
    const float later = page.scroll;
    page.frames(6);
    require(first - released > 20, "release continues scrolling without additional touch events");
    require(page.scroll - later < first - released, "coasting decelerates over equal time intervals");
    page.frames(180);
    page.requireStopped("coasting eventually settles");
    require(page.scroll < page.maxScroll, "natural deceleration settles before reaching an edge");
}

void stationaryHoldDoesNotFling() {
    PageFixture page;
    page.drag();
    page.frames(9);
    const float held = page.scroll;
    page.up();
    page.frames(60);
    require(std::abs(page.scroll - held) <= 1, "holding still for 150 ms removes stale fling velocity");
}

void newTouchStopsAndCanReverse() {
    PageFixture page;
    page.fling();
    page.frames(4);
    page.down(page.visibleButton);
    page.frames(2);
    const float caught = page.scroll;
    page.up();
    page.frames(30);
    require(page.clicks == 0, "tap used to catch a fling does not activate the underlying button");
    require(std::abs(page.scroll - caught) <= 1, "new stationary touch stops an active fling");
    page.down(page.visibleButton);
    page.up();
    require(page.clicks == 1, "a subsequent stationary tap activates buttons normally");

    page.fling();
    page.frames(4);
    page.down(ImVec2(260, 350));
    page.frames(2);
    const float beforeReverse = page.scroll;
    for (int i = 1; i <= 8; ++i) page.position(ImVec2(260, 350 + 12.f * static_cast<float>(i)));
    page.up();
    page.frames(6);
    require(page.scroll < beforeReverse - 80, "touch that catches a fling can immediately drag in reverse");
}

void edgesStopCoasting() {
    PageFixture page;
    page.down(ImVec2(260, 600));
    page.position(ImVec2(260, -20000));
    page.up();
    page.frames(2);
    require(page.scroll == page.maxScroll, "touch scrolling clamps to the bottom edge");
    page.requireStopped("bottom edge stops fling movement");
    page.down(ImVec2(260, 300));
    page.position(ImVec2(260, 20000));
    page.up();
    page.frames(2);
    require(page.scroll == 0, "touch scrolling clamps to the top edge");
    page.requireStopped("top edge stops fling movement");
}

void interruptionsStopCoasting() {
    {
        PageFixture page;
        page.fling(); page.frames(3);
        CancelPageTouch(page.navigation);
        page.requireStopped("system touch cancellation clears inertia");
    }
    {
        PageFixture page;
        page.fling(); page.frames(3);
        page.navigation.selected = Page::Plugins;
        page.frames(2);
        require(page.scroll == 0, "fling does not transfer to another page");
        page.navigation.selected = Page::Home;
        page.requireStopped("returning to a page does not resume an old fling");
    }
    {
        PageFixture page;
        page.fling(); page.frames(3);
        ImGui::GetIO().AddFocusEvent(false);
        page.requireStopped("focus loss stops inertia");
        ImGui::GetIO().AddFocusEvent(true);
        page.requireStopped("focus restoration does not resume inertia");
    }
    {
        PageFixture page;
        page.fling(); page.frames(3);
        ImGui::GetIO().DisplaySize = ImVec2(800, 420);
        page.requireStopped("viewport resize cancels inertia");
    }
    {
        PageFixture page;
        page.fling(); page.frames(3);
        page.openPopup = true;
        page.requireStopped("opening a popup stops background page inertia");
        page.closePopup = true;
        page.requireStopped("closing a popup does not resume background inertia");
    }
}

void refreshRateDoesNotChangeTravel() {
    std::vector<float> travels;
    for (const int rate : {30, 60, 120}) {
        PageFixture page(rate);
        page.fling();
        page.frames(rate * 4);
        page.requireStopped("fling settles at each tested refresh rate");
        travels.push_back(page.scroll);
    }
    const auto bounds = std::minmax_element(travels.begin(), travels.end());
    const float tolerance = std::max(12.f, *bounds.second * .06f);
    if (*bounds.second - *bounds.first > tolerance)
        throw std::runtime_error("equal 30/60/120 Hz gestures have inconsistent total travel: " +
                                 std::to_string(travels[0]) + ", " + std::to_string(travels[1]) + ", " +
                                 std::to_string(travels[2]));
}

void slowFramesPreserveTouchButDiscardStaleInertia() {
    {
        PageFixture page(8);
        page.down(ImVec2(260, 620));
        page.position(ImVec2(260, 560));
        page.position(ImVec2(260, 500));
        page.up(); page.frames(2);
        require(page.scroll >= 110, "slow frames do not cancel active touch dragging");
    }
    {
        PageFixture page;
        page.down(ImVec2(260, 620));
        page.position(ImVec2(260, 560));
        ImGui::GetIO().AddMousePosEvent(260, 500);
        page.frame(.15f);
        page.position(ImVec2(260, 440));
        page.up(); page.frames(2);
        require(page.scroll >= 170, "touch dragging continues through a brief frame stall");
    }
    {
        PageFixture page;
        page.drag();
        page.up(.2f);
        page.frames(2);
        const float released = page.scroll;
        page.frames(60);
        require(std::abs(page.scroll - released) <= 1, "a delayed release does not reuse stale velocity");
    }
    {
        PageFixture page;
        page.fling(); page.frames(3);
        const float beforeStall = page.scroll;
        page.frame(.2f);
        require(std::abs(page.scroll - beforeStall) <= 1, "a stalled frame does not jump an active fling ahead");
        page.requireStopped("a frame stall cancels coasting rather than jumping ahead");
    }
}
}

int main() {
    try {
        releaseContinuesAndDecelerates();
        stationaryHoldDoesNotFling();
        newTouchStopsAndCanReverse();
        edgesStopCoasting();
        interruptionsStopCoasting();
        refreshRateDoesNotChangeTravel();
        slowFramesPreserveTouchButDiscardStaleInertia();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
