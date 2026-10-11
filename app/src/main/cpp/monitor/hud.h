#pragma once
#include "monitor/model.h"
#include <array>
#include <deque>
#include <optional>

namespace vulkanscope::monitor {
struct HudRange {
    std::optional<double> current, minimum, maximum;
};
struct HudSnapshot {
    Values values;
    HudRange fps, present, gpu;
    std::optional<double> processMb, gpuMb;
    std::string gpuUnit;
    std::vector<std::optional<double>> presentGraph, gpuGraph;
    std::string status = "Start HUD to capture a foreground application";
    std::int64_t capturedNs = 0;
};
/** @brief 统计只接收新呈现间隔；120 帧均值、1200 帧极值、200 帧曲线，不对轮询次数计数。 */
class HudHistory {
  public:
    HudSnapshot update(const Values &values, const FrameSample &frames, std::int64_t now);
    void reset();

  private:
    std::string identity_;
    std::deque<std::optional<double>> intervals_;
};
/** @brief Classic HUD 所需的几何尺寸，以字体像素为单位缩放并约束在可用区域内。 */
struct HudSize {
    float width, height, fontSize;
};
HudSize MeasureHud(const Config &config, float fontSize, float maxWidth, float maxHeight);
/** @brief 将实际图形调用及跟踪失败阶段显示为短标题，不将呈现来源或设备能力当作应用 API。 */
std::string GraphicsApiCaption(const Config &config, const HudSnapshot &snapshot);
/** @brief 分别展示本次目标、呈现采样状态、GPU 指标能力及可选游戏引擎，避免缺失原因互相覆盖。 */
std::vector<std::string> HudCaptions(const Config &config, const HudSnapshot &snapshot);
}
