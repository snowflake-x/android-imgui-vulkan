#include "monitor/session.h"
#include <algorithm>
#include <chrono>
#include <sstream>
namespace vulkanscope::monitor {
Session &NativeSession() {
    static Session session;
    return session;
}
Session::~Session() {
    stop();
    shutdownDiagnostics();
    stopSystemInfo();
}
void Session::initialize(const std::string &directory, std::function<PlatformSample()> platform) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_)
        return;
    path_ = directory + "/native-hud.conf";
    config_ = LoadConfig(path_);
    platform_ = std::move(platform);
    initialized_ = true;
}
Config Session::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}
bool Session::configure(Config c) {
    std::lock_guard<std::mutex> lock(mutex_);
    return configureLocked(std::move(c));
}
bool Session::updateConfig(const std::function<void(Config &)> &change) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto config = config_;
    change(config);
    return configureLocked(std::move(config));
}
bool Session::configureLocked(Config c) {
    c.normalize();
    std::string error;
    if (!SaveConfig(path_, c, error)) {
        report_ = error;
        return false;
    }
    if (config_.root != c.root || config_.autoRoot != c.autoRoot) ++rootConfigRevision_;
    if (config_.root != c.root || config_.interval != c.interval || config_.enabled != c.enabled ||
        config_.surface != c.surface || config_.powerPath != c.powerPath || config_.powerScale != c.powerScale)
        ++samplingRevision_;
    config_ = std::move(c);
    ++displayRevision_;
    wake_.notify_all();
    return true;
}
void Session::collapse(bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    collapsed_ = value; ++displayRevision_; wake_.notify_all();
}
std::uint64_t Session::displayRevision() const {
    std::lock_guard<std::mutex> lock(mutex_); return displayRevision_;
}
bool Session::waitForDisplay(std::uint64_t &revision, const std::atomic<bool> &cancelled) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(100), [&] { return cancelled || revision != displayRevision_; });
    if (cancelled || revision == displayRevision_) return false;
    revision = displayRevision_; return true;
}
HudSnapshot Session::hud() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = DisplaySnapshot(hud_, MonotonicNs(), config_.interval, cancel_);
    result.values.insert(gpu_.begin(), gpu_.end());
    return result;
}
std::string Session::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}
std::string Session::system() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_;
}
std::string Session::diagnostics(bool game) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!game)
        return diagnostics_;
    const auto &entries = diagnosticHistory_.entries();
    return entries.empty() ? std::string{} : entries.front().text;
}
std::vector<DiagnosticEntry> Session::diagnosticHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnosticHistory_.entries();
}
std::string Session::rootReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rootReport_;
}
RootAccess Session::rootAccess() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rootAccess_;
}
bool Session::interactive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return interactive_;
}
bool Session::running() const { return !cancel_; }
void Session::gpu(const Values &values) {
    std::lock_guard<std::mutex> lock(mutex_);
    gpu_ = values;
}
void Session::setError(const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    report_ = text;
    ++displayRevision_; wake_.notify_all();
}
void Session::fail(const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    report_ = text;
    cancel_ = true;
    ++displayRevision_; wake_.notify_all();
}
void Session::start(bool rootDisplay) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    if (!cancel_)
        return;
    if (worker_.joinable())
        worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = false;
        hud_ = {};
    }
    collapsed_ = false;
    rootDisplay_ = rootDisplay;
    cancel_ = false;
    setError("Starting native performance capture...");
    worker_ = std::thread([this] {
        try { collect(); }
        catch (const std::exception &error) { fail(std::string("Sampling workers unavailable: ") + error.what()); }
    });
}
void Session::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    cancel_ = true;
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    rootDisplay_ = false;
    collapsed_ = false;
    if (!failed_)
        report_ = "Performance HUD stopped";
}
void Session::startSystemInfo() {
    if (!systemCancel_.exchange(false))
        return;
    systemWorker_ = std::thread([this] {
        while (!systemCancel_) {
            const auto cycleStart = std::chrono::steady_clock::now();
            if (!running() && platform_) {
                try {
                    auto sample = platform_();
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!systemCancel_)
                        system_ = std::move(sample.system);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    system_ = "System information unavailable";
                }
            }
            std::unique_lock<std::mutex> lock(mutex_);
            const auto revision = samplingRevision_;
            wake_.wait_until(lock, cycleStart + std::chrono::milliseconds(config_.interval), [this, revision] {
                return systemCancel_.load() || samplingRevision_ != revision;
            });
        }
    });
}
void Session::stopSystemInfo() {
    systemCancel_ = true;
    wake_.notify_all();
    if (systemWorker_.joinable())
        systemWorker_.join();
}
void Session::shutdownDiagnostics() {
    std::lock_guard<std::mutex> lifecycle(rootLifecycle_);
    rootCancel_ = true;
    if (rootWorker_.joinable())
        rootWorker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    startupRootAttempted_ = false;
}
void Session::requestRoot() {
    std::lock_guard<std::mutex> lifecycle(rootLifecycle_);
    beginRootRequest(true);
}
void Session::requestStartupRoot() {
    std::lock_guard<std::mutex> lifecycle(rootLifecycle_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || !config_.autoRoot || startupRootAttempted_) return;
        startupRootAttempted_ = true;
    }
    beginRootRequest(false);
}
void Session::beginRootRequest(bool diagnostics) {
    if (rootBusy_.exchange(true))
        return;
    if (rootWorker_.joinable())
        rootWorker_.join();
    rootCancel_ = false;
    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        revision = rootConfigRevision_;
        rootAccess_ = RootAccess::Requesting;
        rootReport_ = "Waiting for su authorization (30 second timeout)...";
    }
    rootWorker_ = std::thread([this, revision, diagnostics] {
        try {
            auto run = [&](const std::string &script, int timeout) {
                return rootRunner_ ? rootRunner_(script, timeout)
                                   : RunCommand({"su", "-c", script}, rootCancel_, timeout);
            };
            const auto result = run("id -u", 30000);
            std::istringstream answer(result.output);
            int uid = -1;
            std::string extra;
            const bool granted = result.success && !result.truncated && (answer >> uid) && uid == 0 && !(answer >> extra);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rootAccess_ = granted && !rootCancel_ ? RootAccess::Granted : RootAccess::Unavailable;
                rootReport_ = rootCancel_ ? "Root request cancelled" : granted ? "Root granted"
                    : "Root unavailable or denied. Retry manually after granting access. " + result.error;
                /** @brief 授权期间用户关闭采样或自动申请后，不得由迟到结果重新开启。 */
                if (granted && !rootCancel_ && revision == rootConfigRevision_ && (diagnostics || config_.autoRoot)) {
                    config_.root = true;
                    ++samplingRevision_;
                    std::string error;
                    if (!SaveConfig(path_, config_, error)) rootReport_ += "\n" + error;
                }
            }
            if (granted && diagnostics && !rootCancel_) {
                auto detail = run("id; uname -a; cat /proc/cpuinfo; head -30 /proc/meminfo; getenforce; for z in "
                                  "/sys/class/thermal/thermal_zone*; do cat \"$z/type\" \"$z/temp\" 2>/dev/null; done", 5000);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!rootCancel_) rootReport_ += "\n" + detail.output + "\n" + detail.error;
            }
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(mutex_);
            rootAccess_ = RootAccess::Unavailable;
            rootReport_ = std::string("Root request failed: ") + error.what();
        }
        rootBusy_ = false;
        wake_.notify_all();
    });
}
void Session::collect() {
    ParallelSampler sampler([this] {
        auto platform = platform_ ? platform_() : PlatformSample{};
        /** @brief 撤销权限立即终止服务，不等待下一个数据发布节拍。 */
        if (!rootDisplay_ && !platform.overlayAllowed)
            fail("Overlay permission revoked; stop and grant permission again");
        return platform;
    }, gpuTimingBlocked_, graphicsTracingBlocked_);
    HudHistory history;
    auto next = std::chrono::steady_clock::now();
    while (!cancel_) {
        Config c;
        std::uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            c = config_; revision = samplingRevision_;
        }
        try {
            auto sample = sampler.tick(c, revision, MonotonicNs());
            if (sample.targetChanged) history.reset();
            auto &platform = sample.platform;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                interactive_ = platform.interactive;
                if (!platform.system.empty()) system_ = platform.system;
            }
            if (!rootDisplay_ && !platform.overlayAllowed) {
                fail("Overlay permission revoked; stop and grant permission again");
                break;
            }
            if (!platform.interactive) {
                history.reset();
                std::lock_guard<std::mutex> lock(mutex_);
                hud_ = {}; hud_.status = "Screen off; sampling paused";
                report_ = hud_.status;
                ++displayRevision_; wake_.notify_all();
            } else {
                auto &values = sample.values;
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancel_) break;
                values.insert(gpu_.begin(), gpu_.end());
                hud_ = history.update(values, sample.frames, MonotonicNs());
                if (!sample.delays.empty()) {
                    if (!hud_.status.empty()) hud_.status += "; ";
                    hud_.status += "Delayed: " + sample.delays;
                }
                ++displayRevision_; wake_.notify_all();
                report_ = Report(c, values);
                std::ostringstream text;
                text << system_ << "\nSample CLOCK_BOOTTIME ms: " << BootMs() << "\nRoot opt-in: " << c.root << '\n';
                text << "Enabled fields:";
                for (const auto &id : c.order) if (c.has(id)) text << ' ' << id;
                text << '\n';
                for (const auto &item : values) text << item.first << ": " << item.second << '\n';
                diagnostics_ = text.str();
                diagnosticHistory_.record(values, diagnostics_);
            }
        } catch (const std::exception &e) {
            history.reset();
            setError(std::string("Native sampling error: ") + e.what());
        }
        std::unique_lock<std::mutex> lock(mutex_);
        /** @brief 固定发布节拍，不等待采样通道；错过周期时跳过，不补发突发任务。 */
        next += std::chrono::milliseconds(c.interval);
        if (next <= std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now() + std::chrono::milliseconds(c.interval);
        wake_.wait_until(lock, next, [this, revision] { return cancel_.load() || samplingRevision_ != revision; });
        if (samplingRevision_ != revision) next = std::chrono::steady_clock::now();
    }
    sampler.stop();
    gpuTimingBlocked_ = sampler.gpuTimingBlocked();
    graphicsTracingBlocked_ = sampler.graphicsTracingBlocked();
}
}
