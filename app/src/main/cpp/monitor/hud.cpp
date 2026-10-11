#include "monitor/hud.h"
#include <algorithm>
#include <cmath>

namespace vulkanscope::monitor {
void HudHistory::reset() {
    identity_.clear();
    intervals_.clear();
}
HudSnapshot HudHistory::update(const Values &values, const FrameSample &frames, std::int64_t now) {
    HudSnapshot result;
    result.values = values;
    result.capturedNs = now;
    auto get = [&](const char *id) {
        auto it = values.find(id);
        return it == values.end() ? std::string{} : it->second;
    };
    const auto identity = get("foreground_user") + ":" + get("foreground") + ":" + get("surface");
    if (identity != identity_ || frames.resetHistory) {
        intervals_.clear();
        identity_ = identity;
    }
    if (frames.truncated && !intervals_.empty())
        intervals_.push_back(std::nullopt);
    for (double interval : frames.intervals) {
        if (std::isfinite(interval) && interval > 0)
            intervals_.push_back(interval);
    }
    while (intervals_.size() > 1200)
        intervals_.pop_front();
    double sum = 0;
    std::size_t count = 0;
    for (auto it = intervals_.rbegin(); it != intervals_.rend(); ++it) {
        if (!*it)
            continue;
        const auto value = **it;
        if (count < 120) {
            sum += value;
            ++count;
        }
        result.present.minimum = result.present.minimum ? std::min(*result.present.minimum, value) : value;
        result.present.maximum = result.present.maximum ? std::max(*result.present.maximum, value) : value;
    }
    if (count && frames.fps && *frames.fps > 0) {
        result.present.current = sum / count;
        result.fps.current = 1000.0 / *result.present.current;
    } else if (frames.fps && *frames.fps == 0)
        result.fps.current = 0;
    if (result.present.minimum)
        result.fps.maximum = 1000 / *result.present.minimum;
    if (result.present.maximum)
        result.fps.minimum = 1000 / *result.present.maximum;
    auto first = intervals_.size() > 200 ? intervals_.end() - 200 : intervals_.begin();
    result.presentGraph.assign(first, intervals_.end());
    if (auto memory = Number(get("app_memory_mb")); memory && *memory >= 0)
        result.processMb = *memory;
    /** @brief GPU 窗口独立于 SurfaceFlinger；只展示本轮已归属进程且带明确单位的采样，不复用旧值。 */
    const auto gpuIdentity = get("gpu_identity");
    auto available = [&](const char *id) {
        const auto status = get(id);
        return !gpuIdentity.empty() && gpuIdentity.rfind("N/A", 0) != 0 && !status.empty() &&
               status.rfind("N/A", 0) != 0 && status != "OFF";
    };
    const auto unit = get("gpu_time_unit");
    if (available("gpu_time") && (unit == "ms/batch" || unit == "ms/window")) {
        const auto current = Number(get("gpu_time_ms")), minimum = Number(get("gpu_time_min_ms")),
                   maximum = Number(get("gpu_time_max_ms"));
        if (current && minimum && maximum && *minimum >= 0 && *minimum <= *current &&
            *current <= *maximum) {
            result.gpu = {current, minimum, maximum};
            result.gpuUnit = unit;
        }
    }
    if (available("gpu_memory"))
        if (auto memory = Number(get("gpu_memory_mb")); memory && *memory >= 0)
            result.gpuMb = *memory;
    result.status = frames.status;
    if (!frames.fps && result.status.empty())
        result.status = get("fps_source");
    if (result.fps.current)
        result.status.clear();
    return result;
}
std::string GraphicsApiCaption(const Config &config, const HudSnapshot &snapshot) {
    if (!config.has("graphics_api"))
        return "API: OFF";
    if (!config.root)
        return "API: root OFF";
    auto get = [&](const char *id) {
        const auto value = snapshot.values.find(id);
        return value == snapshot.values.end() ? std::string{} : value->second;
    };
    const auto status = get("graphics_api_status");
    const std::pair<const char *, const char *> captions[] = {
        {"disabled", "API: OFF"},
        {"root_disabled", "API: root OFF"},
        {"root_unavailable", "API: root blocked"},
        {"waiting", "API: waiting"},
        {"target_unavailable", "API: no target"},
        {"symbols_unavailable", "API: no symbols"},
        {"trace_unavailable", "API: trace blocked"},
        {"probe_failed", "API: probe failed"},
        {"capture_failed", "API: capture failed"},
        {"no_calls", "API: no calls"},
        {"cleanup_failed", "API: cleanup failed"},
        {"cancelled", "API: cancelled"},
    };
    for (const auto &caption : captions)
        if (status == caption.first)
            return caption.second;
    if (!status.empty() && status != "observed")
        return "API: unavailable";
    auto api = get("graphics_api");
    if (api.empty())
        return status.empty() ? "API: waiting" : "API: unavailable";
    api = api.substr(0, api.find(" ("));
    return api == "Vulkan" || api == "OpenGL ES" || api == "OpenGL ES + Vulkan"
               ? api : "API: unavailable";
}
std::array<std::string, 3> HudCaptions(const Config &config, const HudSnapshot &snapshot) {
    const auto foreground = snapshot.values.find("foreground");
    const auto target = foreground == snapshot.values.end() || foreground->second.rfind("N/A", 0) == 0
                            ? "unavailable" : foreground->second;
    const auto state = !config.has("fps") && !config.has("frame_time") ? "FPS / Pre: OFF"
                       : snapshot.status.empty() ? "Pre: ms | Mem: main-process RSS" : snapshot.status;
    auto gpuUnit = snapshot.gpuUnit;
    if (gpuUnit == "ms/window") {
        const auto window = snapshot.values.find("gpu_time_window_ms");
        const auto duration = window == snapshot.values.end() ? std::nullopt : Number(window->second);
        if (duration && *duration > 0)
            gpuUnit = "ms/" + Format(*duration, 0) + "ms";
        gpuUnit += " (UID)";
    }
    std::array<std::string, 3> result{
        "App: " + target, state,
        std::string("GPU: ") + (!config.has("gpu_time") ? "OFF"
                                : snapshot.gpu.current ? gpuUnit : "unavailable") +
            " | []: " + (!config.has("gpu_memory") ? "OFF"
                          : snapshot.gpuMb ? "KGSL shared MB" : "unavailable")};
    for (auto &caption : result)
        std::replace_if(caption.begin(), caption.end(),
                        [](char ch) { return ch == '\n' || ch == '\r' || ch == '\t'; }, ' ');
    return result;
}
HudSize MeasureHud(const Config &config, float fontSize, float maxWidth, float maxHeight) {
    float rows = 6.0f;
    if (config.hudExtras) {
        for (const auto &id : config.order)
            if (config.has(id) && id != "fps" && id != "frame_time" && id != "gpu_model" &&
                id != "app_memory" && id != "gpu_time" && id != "gpu_memory")
                rows += 0.7f;
    }
    const float unitsWide = 18.8f;
    const float unitsHigh = 0.7f + rows * 1.23f + (config.hudGraph ? 3.6f : 0.0f) + 1.9f;
    fontSize = std::max(1.0f, std::min({fontSize, maxWidth / unitsWide, maxHeight / unitsHigh}));
    return {std::min(maxWidth, unitsWide * fontSize), std::min(maxHeight, unitsHigh * fontSize), fontSize};
}
}
