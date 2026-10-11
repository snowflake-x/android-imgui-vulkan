#pragma once
#include "monitor/collector.h"
#include "monitor/hud.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace vulkanscope::monitor {
struct PlatformSample {
    Values values;
    std::string system;
    bool interactive = true, overlayAllowed = true;
};
/** @brief 进程级原生会话；配置/快照受锁保护，采集线程可取消且必须 join 后销毁。 */
class Session {
  public:
    ~Session();
    void initialize(const std::string &directory, std::function<PlatformSample()> platform);
    Config config() const;
    bool configure(Config config);
    std::string report() const;
    HudSnapshot hud() const;
    std::string system() const;
    std::string diagnostics(bool game = false) const;
    std::string rootReport() const;
    void requestRoot();
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
    mutable std::mutex mutex_;
    std::mutex lifecycle_;
    std::condition_variable wake_;
    std::string path_, report_ = "Performance HUD stopped", system_, diagnostics_, gameDiagnostics_;
    std::string rootReport_ = "Not requested. Root access requires an explicit action.";
    Config config_;
    HudSnapshot hud_;
    Values gpu_;
    std::function<PlatformSample()> platform_;
    std::atomic<bool> cancel_{true}, rootCancel_{false};
    bool initialized_ = false, interactive_ = true, failed_ = false;
    std::atomic<bool> rootBusy_{false}, systemCancel_{true};
    std::thread systemWorker_;
    std::thread worker_, rootWorker_;
};
Session &NativeSession();
}
