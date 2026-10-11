#pragma once
#include "monitor/model.h"
#include "monitor/process.h"
#include <atomic>
#include <functional>
namespace vulkanscope::monitor {
std::string ReadFile(const std::string &path, std::size_t limit = 32768);
/** @brief 单线程原生采样器，平台只补充系统 API 数据，文件与命令均由 C++ 读取。 */
class Collector {
  public:
    using CommandRunner = std::function<CommandResult(const std::string &, int)>;
    explicit Collector(const std::atomic<bool> &cancel, CommandRunner runner = {}, bool gpuTimingBlocked = false)
        : cancel_(cancel), runner_(std::move(runner)), gpuTimingCleanupFailed_(gpuTimingBlocked) {}
    Values sample(const Config &config);
    void reset();
    const FrameSample &frameSample() const { return latestFrames_; }
    /** @brief 宿主跨 HUD 启停保留清理失败状态，禁止遗留实例尚未处理时继续创建跟踪。 */
    bool gpuTimingBlocked() const { return gpuTimingCleanupFailed_; }

  private:
    CommandResult root(const std::string &script, int timeout = 2000, std::size_t limit = 32768);
    std::string read(const std::string &path);
    void hardware(const Config &config, Values &values);
    void foreground(const Config &config, Values &values);
    void gpuTelemetry(const Config &config, const Foreground &foreground, Values &values);
    void presentation(const Config &config, const Foreground &foreground, Values &values);
    void engine(const Config &config, Values &values);
    const std::atomic<bool> &cancel_;
    CommandRunner runner_;
    bool opted_ = false, authorized_ = false;
    std::string rootError_;
    bool traceCleanupFailed_ = false;
    bool gpuTimingCleanupFailed_ = false;
    FrameStats frames_;
    FrameSample latestFrames_;
    std::string surfaceIdentity_, selectedSurface_;
    std::size_t surfaceCursor_ = 0;
    std::optional<Ticks> ticks_;
};
}
