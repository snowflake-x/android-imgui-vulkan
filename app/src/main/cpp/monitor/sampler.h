#pragma once
#include "monitor/collector.h"
#include <memory>

namespace vulkanscope::monitor {
struct PlatformSample {
    Values values;
    std::string system;
    bool interactive = true, overlayAllowed = true;
};
struct SamplePacket {
    Values values;
    FrameSample frames;
    PlatformSample platform;
};
struct ParallelSample : SamplePacket {
    bool targetChanged = false;
    std::string delays;
};
/** @brief 固定通道各自串行采集；主线程按周期合并，忙通道不排队，换目标取消并丢弃旧结果。 */
class ParallelSampler {
  public:
    using Task = std::function<SamplePacket(SamplePart, const Config &, const Values &, const std::atomic<bool> &)>;
    ParallelSampler(std::function<PlatformSample()> platform, bool gpuBlocked, bool graphicsBlocked, Task task = {});
    ~ParallelSampler();
    /** @brief 仅宿主调度线程调用；非阻塞发布已完成结果并派发下一轮采集。 */
    ParallelSample tick(const Config &config, std::uint64_t revision, std::int64_t now);
    /** @brief 先取消所有通道再 join，保留跟踪清理失败状态供下一会话继承。 */
    void stop();
    bool gpuTimingBlocked() const;
    bool graphicsTracingBlocked() const;
  private:
    struct State;
    std::unique_ptr<State> state_;
};
}
