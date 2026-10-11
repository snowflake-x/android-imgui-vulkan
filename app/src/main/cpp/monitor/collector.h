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
    explicit Collector(const std::atomic<bool> &cancel, CommandRunner runner = {})
        : cancel_(cancel), runner_(std::move(runner)) {}
    Values sample(const Config &config);
    void reset();
    const FrameSample &frameSample() const { return latestFrames_; }

  private:
    CommandResult root(const std::string &script, int timeout = 2000);
    std::string read(const std::string &path);
    void hardware(const Config &config, Values &values);
    void foreground(const Config &config, Values &values);
    void engine(const Config &config, Values &values);
    const std::atomic<bool> &cancel_;
    CommandRunner runner_;
    bool opted_ = false, authorized_ = false;
    std::string rootError_;
    FrameStats frames_;
    FrameSample latestFrames_;
    std::optional<Ticks> ticks_;
};
}
