#pragma once
#include "monitor/collector.h"
#include "monitor/diagnostics.h"
#include "monitor/hud.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace vulkanscope::monitor {
enum class RootAccess { Unknown, Requesting, Granted, Unavailable };
struct PlatformSample {
    Values values;
    std::string system;
    bool interactive = true, overlayAllowed = true;
};
/** @brief 进程级原生会话；配置/快照受锁保护，采集线程可取消且必须 join 后销毁。 */
class Session {
  public:
    explicit Session(Collector::CommandRunner rootRunner = {}) : rootRunner_(std::move(rootRunner)) {}
    ~Session();
    void initialize(const std::string &directory, std::function<PlatformSample()> platform);
    Config config() const;
    bool configure(Config config);
    /** @brief 在同一配置锁中应用字段修改，避免 Kotlin 表单覆盖并发 Root 授权的更新。 */
    bool updateConfig(const std::function<void(Config &)> &change);
    std::string report() const;
    HudSnapshot hud() const;
    std::string system() const;
    std::string diagnostics(bool game = false) const;
    /** @brief 在锁内复制最近应用诊断，供 ImGui 选择已离开前台的游戏。 */
    std::vector<DiagnosticEntry> diagnosticHistory() const;
    std::string rootReport() const;
    void requestRoot();
    /** @brief 每次主界面生命周期仅自动申请一次，不启动 HUD；拒绝后由用户手动重试。 */
    void requestStartupRoot();
    RootAccess rootAccess() const;
    void start();
    void stop();
    void setError(const std::string &error);
    void fail(const std::string &error);
    void gpu(const Values &values);
    bool interactive() const;
    bool running() const;
    void shutdownDiagnostics();
    void startSystemInfo();
    void stopSystemInfo();

  private:
    void collect();
    bool configureLocked(Config config);
    void beginRootRequest(bool diagnostics);
    mutable std::mutex mutex_;
    std::mutex lifecycle_;
    std::mutex rootLifecycle_;
    std::condition_variable wake_;
    std::string path_, report_ = "Performance HUD stopped", system_, diagnostics_;
    DiagnosticHistory diagnosticHistory_;
    std::string rootReport_ = "Root has not been requested";
    RootAccess rootAccess_ = RootAccess::Unknown;
    Collector::CommandRunner rootRunner_;
    std::uint64_t rootConfigRevision_ = 0;
    bool startupRootAttempted_ = false;
    Config config_;
    HudSnapshot hud_;
    Values gpu_;
    std::function<PlatformSample()> platform_;
    std::atomic<bool> cancel_{true}, rootCancel_{false};
    bool initialized_ = false, interactive_ = true, failed_ = false;
    /** @brief 仅采样线程访问；后续线程在旧线程 join 后继承，HUD 启停不清除此状态。 */
    bool gpuTimingBlocked_ = false;
    /** @brief 与 GPU 计时相同，仅采样线程访问并在 join 后继承清理失败状态。 */
    bool graphicsTracingBlocked_ = false;
    std::atomic<bool> rootBusy_{false}, systemCancel_{true};
    std::thread systemWorker_;
    std::thread worker_, rootWorker_;
};
Session &NativeSession();
}
