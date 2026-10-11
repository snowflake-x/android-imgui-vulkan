#include "imgui_preview.h"
#include "monitor/hud_view.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace vulkanscope::monitor;
namespace {
void require(bool value, const char *message = "Classic HUD regression") {
    if (!value)
        throw std::runtime_error(message);
}
/** @brief 驱动窗口与命令批次不能冒充帧历史，GPU 指标也不能依赖 SurfaceFlinger 成功。 */
void gpuMetrics() {
    Config config;
    require(!config.has("gpu_time") && config.has("gpu_memory"), "safe GPU sampling defaults");
    for (auto preset : {MetricPreset::Game, MetricPreset::Hardware}) {
        config.enabled["gpu_time"] = true;
        config.enabled["gpu_memory"] = false;
        ApplyMetricPreset(config, preset);
        require(!config.has("gpu_time") && config.has("gpu_memory"), "presets require explicit trace opt-in");
    }
    config.enabled["gpu_time"] = true;
    HudHistory history;
    Values values{{"foreground", "com.example.game"}, {"foreground_user", "0"},
                  {"gpu_identity", "com.example.game:10001:123:456"},
                  {"gpu_time", "0.44 ms/batch"}, {"gpu_time_unit", "ms/batch"},
                  {"gpu_time_ms", "0.44"}, {"gpu_time_min_ms", "0.40"}, {"gpu_time_max_ms", "0.51"},
                  {"gpu_time_source", "KGSL hardware active time"},
                  {"gpu_memory", "100.25 MB"}, {"gpu_memory_mb", "100.25"},
                  {"gpu_memory_source", "KGSL allocation and proportional DMA-BUF accounting"}};
    FrameSample frames;
    frames.status = "Surface latency unavailable";
    auto sample = history.update(values, frames, 1);
    require(sample.gpu.current == .44 && sample.gpu.minimum == .40 && sample.gpu.maximum == .51 &&
            sample.gpuMb == 100.25 && !sample.fps.current, "GPU metrics survive unavailable Surface latency");
    require(sample.gpuGraph.empty(), "GPU batches must not become invented frame timings");
    auto captions = HudCaptions(config, sample);
    require(captions[1] == frames.status && captions[2] == "GPU: ms/batch | []: KGSL shared MB",
            "GPU scope and memory sharing are explicit");
    values["gpu_time_unit"] = "ms/window";
    sample = history.update(values, frames, 2);
    require(sample.gpu.current == .44 &&
            HudCaptions(config, sample)[2] == "GPU: ms/window (UID) | []: KGSL shared MB",
            "UID driver windows are distinguished from hardware batches");
    values["gpu_time_window_ms"] = "900.000000";
    sample = history.update(values, frames, 2);
    require(HudCaptions(config, sample)[2] == "GPU: ms/900ms (UID) | []: KGSL shared MB",
            "HUD exposes the measured window length instead of implying per-frame time");
    frames.fps = 60;
    frames.intervals.assign(120, 16.67);
    sample = history.update(values, frames, 3);
    require(sample.presentGraph.size() == 120 && sample.gpuGraph.empty(), "GPU samples do not fill SF history");
    config.enabled["gpu_time"] = config.enabled["gpu_memory"] = false;
    require(HudCaptions(config, sample)[2] == "GPU: OFF | []: OFF", "disabled metrics are marked OFF");
    config.enabled["gpu_time"] = config.enabled["gpu_memory"] = true;
    values["gpu_time"] = "N/A (trace unavailable)";
    values["gpu_memory"] = "N/A (process exited)";
    sample = history.update(values, frames, 4);
    require(!sample.gpu.current && !sample.gpu.minimum && !sample.gpu.maximum && !sample.gpuMb,
            "invalid status rejects stale numeric payloads");
    values["gpu_time"] = "0.44 ms/window";
    values["gpu_memory"] = "100.25 MB";
    for (const auto &invalid : {Values{{"gpu_time_unit", "ms/frame"}},
                               Values{{"gpu_time_ms", "nan"}},
                               Values{{"gpu_time_min_ms", "0.50"}},
                               Values{{"gpu_time_max_ms", "0.40"}},
                               Values{{"gpu_time_min_ms", "-1"}}}) {
        auto broken = values;
        for (const auto &item : invalid)
            broken[item.first] = item.second;
        sample = history.update(broken, frames, 5);
        require(!sample.gpu.current && !sample.gpu.minimum && !sample.gpu.maximum,
                "invalid units, numbers and ranges must be unavailable");
    }
    for (const auto *invalid : {"-1", "inf", "nan", "100 MB"}) {
        values["gpu_memory_mb"] = invalid;
        require(!history.update(values, frames, 6).gpuMb, "invalid GPU memory rejected");
    }
    values["gpu_memory_mb"] = "0";
    require(history.update(values, frames, 7).gpuMb == 0, "zero allocated GPU memory is a valid sample");
    values.erase("gpu_identity");
    sample = history.update(values, frames, 8);
    require(!sample.gpu.current && !sample.gpuMb, "GPU metrics require a verified process identity");
    values = {{"foreground", "com.example.other"}, {"foreground_user", "10"}};
    sample = history.update(values, FrameSample{}, 9);
    require(!sample.gpu.current && !sample.gpu.minimum && !sample.gpu.maximum && !sample.gpuMb &&
            sample.gpuGraph.empty(), "switching targets never retains earlier GPU samples");
    history.reset();
    sample = history.update({}, FrameSample{}, 10);
    require(!sample.gpu.current && !sample.gpuMb, "reset clears GPU values");
}
}
int main(int argc, char **argv) {
    gpuMetrics();
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
    config.enabled["gpu_time"] = true;
    sample.gpu = {.44, .40, .51};
    sample.gpuUnit = "ms/batch";
    sample.gpuMb = 100.25;
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
