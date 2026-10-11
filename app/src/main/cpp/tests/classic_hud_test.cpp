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
/** @brief 慢采样显示原始年龄，超过硬期限或停止后不得继续显示旧数值。 */
void snapshotFreshness() {
    HudSnapshot sample;
    sample.capturedNs = 1000000000LL;
    sample.fps = {60, 50, 61};
    sample.present = {16.7, 16, 20};
    sample.gpu = {1, 1, 1};
    sample.processMb = 123;
    sample.values["engine_name"] = "Unity";
    sample.values["graphics_api_status"] = "observed";
    const auto slow = DisplaySnapshot(sample, 8000000000LL, 1000, false);
    require(slow.fps.current == 60 && slow.processMb == 123 && slow.gpu.current == 1,
            "slow root cycle does not blank every metric at five seconds");
    require(slow.capturedNs == sample.capturedNs && slow.status.find("7.0s ago") != std::string::npos,
            "retained data is explicitly old and never receives a new timestamp");
    const auto expired = DisplaySnapshot(sample, 17000000000LL, 1000, false);
    require(!expired.fps.current && !expired.processMb && !expired.gpu.current &&
            expired.values.at("graphics_api_status") == "waiting", "hard expiry clears stale metrics");
    const auto stopped = DisplaySnapshot(sample, 2000000000LL, 1000, true);
    require(!stopped.fps.current && stopped.values.at("engine_name") == "N/A (Capture stopped)",
            "stopping never retains numeric data or engine identity");
    HudSnapshot missing;
    missing.capturedNs = 8000000000LL;
    require(!DisplaySnapshot(missing, 9000000000LL, 1000, false).fps.current,
            "explicitly unavailable new samples are not replaced with old measurements");
}
/** @brief 区分图形调用证据、关闭及各失败阶段，不能将设备能力、呈现来源或旧结果误当成活跃 API。 */
void graphicsApiCaptions() {
    Config config;
    HudSnapshot sample;
    sample.values["gpu_api"] = "1.3.284";
    sample.values["fps_source"] = "SurfaceFlinger actual-present";
    config.root = true;
    require(GraphicsApiCaption(config, sample) == "API: OFF", "root does not enable API tracing");
    config.enabled["graphics_api"] = true;
    require(GraphicsApiCaption(config, sample) == "API: waiting", "device capability is not active API evidence");
    for (const auto *observed : {"OpenGL ES (EGL swap observed)", "Vulkan (calls observed)",
                               "OpenGL ES + Vulkan (calls observed)"}) {
        sample.values["graphics_api"] = observed;
        for (const char *status : {"", "observed"}) {
            sample.values["graphics_api_status"] = status;
            const std::string expected(observed);
            require(GraphicsApiCaption(config, sample) == expected.substr(0, expected.find(" (")),
                    "confirmed and legacy API evidence keeps the API name");
        }
    }
    sample.values["graphics_api"] = "Vulkan (calls observed)";
    const std::pair<const char *, const char *> statuses[] = {
        {"disabled", "API: OFF"},
        {"root_disabled", "API: root OFF"},
        {"root_unavailable", "API: root blocked"},
        {"waiting", "API: waiting"},
        {"target_unavailable", "API: no target"},
        {"symbols_unavailable", "API: no symbols"},
        {"libraries_unavailable", "API: no libraries"},
        {"trace_unavailable", "API: trace blocked"},
        {"probe_failed", "API: probe failed"},
        {"capture_failed", "API: capture failed"},
        {"no_calls", "API: no calls"},
        {"cleanup_failed", "API: cleanup failed"},
        {"cancelled", "API: cancelled"},
        {"unknown_future_status", "API: unavailable"},
    };
    for (const auto &status : statuses) {
        sample.values["graphics_api_status"] = status.first;
        const auto caption = GraphicsApiCaption(config, sample);
        require(caption == status.second && caption.size() <= 20,
                "each failure stage fits the HUD and overrides stale observed calls");
    }
    sample.values["graphics_api_status"] = "mapped";
    sample.values["graphics_api"] = "OpenGL ES + Vulkan (libraries loaded; calls unconfirmed)";
    require(GraphicsApiCaption(config, sample) == "Loaded: GLES+Vulkan", "library evidence never looks like confirmed calls");
    sample.values.erase("graphics_api_status");
    sample.values["graphics_api"] = "N/A (no accessible EGL/Vulkan probe symbols)";
    require(GraphicsApiCaption(config, sample) == "API: unavailable", "legacy N/A never becomes an API name");
    sample.values["graphics_api"] = "SurfaceFlinger";
    require(GraphicsApiCaption(config, sample) == "API: unavailable", "presentation source is not an API");
    sample.values["graphics_api"] = "Vulkan (calls observed)";
    sample.values["graphics_api_status"] = "observed";
    config.root = false;
    require(GraphicsApiCaption(config, sample) == "API: root OFF", "disabling root hides earlier calls");
    config.enabled["graphics_api"] = false;
    require(GraphicsApiCaption(config, sample) == "API: OFF", "disabling the field hides earlier calls");
    for (auto preset : {MetricPreset::Game, MetricPreset::Hardware}) {
        config.enabled["graphics_api"] = true;
        ApplyMetricPreset(config, preset);
        require(!config.has("graphics_api"), "field presets keep tracing off");
    }
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
    config.enabled["engine_name"] = true;
    config.root = true;
    sample.values["engine_name"] = "Unity";
    auto captions = HudCaptions(config, sample);
    require(captions.back() == "Engine: Unity", "engine displayed independently of resolution");
    config.root = false;
    require(HudCaptions(config, sample).back() == "Engine: root OFF", "root optout immediately hides old engine");
    config.root = true;
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
    snapshotFreshness();
    graphicsApiCaptions();
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
