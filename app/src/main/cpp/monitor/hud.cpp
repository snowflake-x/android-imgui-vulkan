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
    result.gpuGraph.resize(result.presentGraph.size());
    if (auto memory = Number(get("app_memory_mb")); memory && *memory >= 0)
        result.processMb = *memory;
    result.status = frames.status;
    if (!frames.fps && result.status.empty())
        result.status = get("fps_source");
    if (result.fps.current)
        result.status.clear();
    return result;
}
HudSize MeasureHud(const Config &config, float fontSize, float maxWidth, float maxHeight) {
    float rows = 6.0f;
    if (config.hudExtras) {
        for (const auto &id : config.order)
            if (config.has(id) && id != "fps" && id != "frame_time" && id != "gpu_model" &&
                id != "app_memory")
                rows += 0.7f;
    }
    const float unitsWide = 18.8f;
    const float unitsHigh = 0.7f + rows * 1.23f + (config.hudGraph ? 3.6f : 0.0f) + 0.65f;
    fontSize = std::max(1.0f, std::min({fontSize, maxWidth / unitsWide, maxHeight / unitsHigh}));
    return {std::min(maxWidth, unitsWide * fontSize), std::min(maxHeight, unitsHigh * fontSize), fontSize};
}
}
