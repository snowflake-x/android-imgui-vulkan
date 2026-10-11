#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vulkanscope::monitor {
struct Metric {
    const char *id;
    const char *label;
    bool enabled;
};
const std::vector<Metric> &Metrics();
inline constexpr std::array<int, 5> SampleIntervals{250, 500, 1000, 2000, 5000};
struct Config {
    bool root = false;
    bool autoRoot = true;
    bool rootDisplay = true;
    bool liveUpdates = false;
    unsigned managerPlugins = 7;
    bool hudGraph = true, hudExtras = false;
    int interval = 1000, textSize = 14, opacity = 80, corner = 0;
    std::string surface, powerPath;
    std::string language = "system";
    std::string hudStyle = "metal";
    double powerScale = 0.000001;
    std::vector<std::string> order;
    std::map<std::string, bool> enabled;
    Config();
    bool has(const std::string &id) const;
    void normalize();
};
enum class MetricPreset { Game, Hardware };
/** @brief 应用字段预设并保留经典 HUD 核心字段；不改变权限和独立显示设置。 */
void ApplyMetricPreset(Config &config, MetricPreset preset);
/** @brief 原子保存私有配置；失败返回错误，不覆盖已有文件。 */
bool SaveConfig(const std::string &path, Config config, std::string &error);
Config LoadConfig(const std::string &path);
std::string Quote(const std::string &value);
bool ValidPowerPath(const std::string &value);
std::int64_t MonotonicNs();
std::int64_t BootMs();
std::optional<double> Number(const std::string &value);
std::string Format(double value, int precision = 1);
using Values = std::map<std::string, std::string>;
std::string Report(const Config &config, const Values &values);
using Ticks = std::array<std::uint64_t, 8>;
std::optional<Ticks> CpuTicks(const std::string &text);
std::optional<double> CpuLoad(const Ticks &before, const Ticks &after);
std::optional<double> GpuLoad(const std::string &text);
struct Foreground {
    std::string package;
    int user = -1;
};
std::optional<Foreground> ParseForeground(const std::string &text, bool activities);
inline constexpr std::size_t MaxSurfaceCandidates = 128;
inline constexpr std::size_t MaxSurfaceNameBytes = 4096;
/**
 * @brief 归一化 Android 图层名称后返回限定游戏包的去重候选，优先 SurfaceView 与 BLAST 缓冲图层。
 * @note 保留至多 MaxSurfaceCandidates 个候选，每个名字不超过 MaxSurfaceNameBytes 字节；
 *       调用方负责跨轮采样预算，不能仅查询固定的前几个候选。
 */
std::vector<std::string> Surfaces(const std::string &text, const std::string &package,
                                  const std::string &overrideName);
struct Trace {
    bool supported = false;
    std::vector<std::int64_t> presents;
};
Trace ParseLatency(const std::string &text, std::int64_t now);
struct FrameSample {
    std::optional<double> fps, mean, p95;
    bool truncated = false, resetHistory = true;
    std::vector<double> intervals;
    std::string status;
};
/** @brief 仅由采样线程持有，切换应用/Surface 或休眠后重置基线。 */
class FrameStats {
  public:
    FrameSample update(const std::string &key, const Trace &trace, std::int64_t now);
    void reset();

  private:
    std::string key_;
    std::int64_t sample_ = 0, present_ = 0;
};
/** @brief 运行进程的引擎库证据；多个引擎同时出现时保留歧义。 */
struct EngineIdentity {
    std::string name = "Unknown";
    std::string evidence;
};
/** @brief 按可执行映射的完整库文件名识别引擎，不用包名或任意字符串猜测。 */
EngineIdentity IdentifyEngine(const std::string &maps);
struct Telemetry {
    std::string engine, source;
    std::int64_t stamp = 0;
    int pid = 0, width = 0, height = 0, outputWidth = 0, outputHeight = 0;
};
/** @brief 严格验证有界 JSON、包身份、整数尺寸与 CLOCK_BOOTTIME 新鲜度。 */
std::optional<Telemetry> ParseTelemetry(const std::string &json, const std::string &package, std::int64_t now,
                                        std::string &error);
}
