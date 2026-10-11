#include "imgui_preview.h"
#include "monitor/hud_view.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace vulkanscope::monitor;
namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("Classic HUD regression");
}

}
int main(int argc, char **argv) {
    HudHistory history;
    Values values{{"foreground", "game"}, {"foreground_user", "0"}, {"surface", "layer"}};
    FrameSample frames;
    frames.resetHistory = false;
    frames.fps = 60;
    frames.intervals = {10, 20, 30};
    auto sample = history.update(values, frames, 1);
    require(sample.fps.current && *sample.fps.current == 50 && *sample.present.minimum == 10 &&
            *sample.present.maximum == 30);
    require(sample.presentGraph.size() == 3 && sample.presentGraph[0] == 10 && sample.presentGraph[2] == 30);
    frames.intervals.clear();
    frames.fps = 0;
    sample = history.update(values, frames, 2);
    require(sample.fps.current == 0 && !sample.present.current && sample.presentGraph.size() == 3);
    frames.truncated = true;
    frames.intervals = {40};
    frames.fps = 25;
    sample = history.update(values, frames, 3);
    require(sample.presentGraph.size() == 5 && !sample.presentGraph[3]);
    values["surface"] = "other";
    sample = history.update(values, frames, 4);
    require(sample.presentGraph.size() == 1 && sample.present.minimum == 40);
    frames.resetHistory = true;
    frames.intervals.clear();
    frames.fps.reset();
    sample = history.update(values, frames, 5);
    require(sample.presentGraph.empty() && !sample.fps.current && !sample.gpu.current && !sample.processMb);
    frames.resetHistory = false;
    frames.fps = 60;
    frames.truncated = false;
    frames.intervals.assign(1400, 16.67);
    sample = history.update(values, frames, 6);
    require(sample.presentGraph.size() == 200 && std::abs(*sample.present.current - 16.67) < .001);
    Config config;
    auto captions = HudCaptions(config, sample);
    require(captions[0] == "App: game" && captions[2].find("unavailable") != std::string::npos);
    sample.status = "SurfaceFlinger query timed out";
    captions = HudCaptions(config, sample);
    require(captions[1] == sample.status && captions[2].find("unavailable") != std::string::npos);
    sample.status = "Query failed:\nPermission Denial\r\n";
    require(HudCaptions(config, sample)[1].find_first_of("\r\n\t") == std::string::npos);
    config.enabled["fps"] = config.enabled["frame_time"] = false;
    require(HudCaptions(config, sample)[1] == "FPS / Pre: OFF");
    config = Config{};
    sample.values["foreground"] = "N/A (Root denied)";
    require(HudCaptions(config, sample)[0] == "App: unavailable");
    for (float width : {120.f, 320.f, 1080.f}) {
        auto size = MeasureHud(config, 40, width, 200);
        require(size.width <= width && size.height <= 200 && size.fontSize > 0);
    }
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(960, 600);
    io.DeltaTime = 1.f / 60;
    require(io.Fonts->AddFontFromFileTTF(HUD_FONT_PATH, 40) != nullptr);
    unsigned char *atlas;
    int aw, ah;
    io.Fonts->GetTexDataAsAlpha8(&atlas, &aw, &ah);
    ImGui::NewFrame();
    config.opacity = 100;
    sample.values = {{"gpu_model", "Adreno 750"},
                     {"foreground", "com.example.game"},
                     {"display_resolution", "2940 x 1840 px (display mode)"},
                     {"display_refresh", "60 Hz"}};
    sample.fps = {56.25, 30, 60};
    sample.present = {17.78, 16.67, 33.33};
    sample.processMb = 160.35;
    sample.presentGraph.clear();
    sample.gpuGraph.clear();
    for (int i = 0; i < 200; ++i)
        sample.presentGraph.push_back(i % 28 == 20 ? 33.33 : 16.67);
    sample.status.clear();
    DrawClassicHud(*ImGui::GetForegroundDrawList(), ImVec2(104, 50), MeasureHud(config, 40, 752, 520), config,
                   sample);
    ImGui::Render();
    require(ImGui::GetDrawData()->TotalVtxCount > 0);
    if (argc > 1)
        test_preview::SavePreview(argv[1], *ImGui::GetDrawData(), atlas, aw, ah);
    ImGui::DestroyContext(context);
}
