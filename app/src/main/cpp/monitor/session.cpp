#include "monitor/session.h"
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
    c.normalize();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    if (!SaveConfig(path_, c, error)) {
        report_ = error;
        return false;
    }
    config_ = std::move(c);
    wake_.notify_all();
    return true;
}
HudSnapshot Session::hud() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = hud_;
    if (cancel_ || MonotonicNs() - result.capturedNs > std::max(5000, config_.interval * 3) * 1000000LL) {
        result = {};
        result.values = hud_.values;
        result.status = cancel_ ? "Capture stopped" : "Waiting for a fresh sample";
    }
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
}
void Session::fail(const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    report_ = text;
    cancel_ = true;
    wake_.notify_all();
}
void Session::start() {
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
    cancel_ = false;
    setError("Starting native performance capture...");
    worker_ = std::thread([this] { collect(); });
}
void Session::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    cancel_ = true;
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failed_)
        report_ = "Performance HUD stopped";
}
void Session::startSystemInfo() {
    if (!systemCancel_.exchange(false))
        return;
    systemWorker_ = std::thread([this] {
        while (!systemCancel_) {
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
            wake_.wait_for(lock, std::chrono::seconds(1), [this] { return systemCancel_.load(); });
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
    rootCancel_ = true;
    if (rootWorker_.joinable())
        rootWorker_.join();
}
void Session::requestRoot() {
    if (rootBusy_.exchange(true))
        return;
    if (rootWorker_.joinable())
        rootWorker_.join();
    rootCancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rootReport_ = "Waiting for su authorization (30 second timeout)...";
    }
    rootWorker_ = std::thread([this] {
        auto result =
            RunCommand({"su", "-c",
                        "id; uname -a; cat /proc/cpuinfo; head -30 /proc/meminfo; getenforce; for z in "
                        "/sys/class/thermal/thermal_zone*; do cat \"$z/type\" \"$z/temp\" 2>/dev/null; done"},
                       rootCancel_, 30000);
        std::lock_guard<std::mutex> lock(mutex_);
        rootReport_ = (result.success ? "Root diagnostics completed" : result.error) + "\n" + result.output +
                      "\n(Output limited to 32 KiB)";
        rootBusy_ = false;
    });
}
void Session::collect() {
    Collector collector(cancel_, {}, gpuTimingBlocked_);
    HudHistory history;
    while (!cancel_) {
        const auto c = config();
        try {
            auto platform = platform_ ? platform_() : PlatformSample{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                interactive_ = platform.interactive;
                system_ = platform.system;
            }
            if (!platform.overlayAllowed) {
                fail("Overlay permission revoked; stop and grant permission again");
                break;
            }
            if (!platform.interactive) {
                collector.reset();
                history.reset();
                setError("Screen off; sampling paused");
            } else {
                auto values = collector.sample(c);
                gpuTimingBlocked_ = collector.gpuTimingBlocked();
                if (cancel_)
                    break;
                for (auto &item : platform.values)
                    values[item.first] = std::move(item.second);
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancel_)
                    break;
                values.insert(gpu_.begin(), gpu_.end());
                hud_ = history.update(values, collector.frameSample(), MonotonicNs());
                report_ = Report(c, values);
                std::ostringstream text;
                text << system_ << "\nSample CLOCK_BOOTTIME ms: " << BootMs() << "\nRoot opt-in: " << c.root
                     << '\n';
                text << "Enabled fields:";
                for (const auto &id : c.order)
                    if (c.has(id))
                        text << ' ' << id;
                text << '\n';
                for (const auto &item : values)
                    text << item.first << ": " << item.second << '\n';
                diagnostics_ = text.str();
                diagnosticHistory_.record(values, diagnostics_);
            }
        } catch (const std::exception &e) {
            gpuTimingBlocked_ = collector.gpuTimingBlocked();
            history.reset();
            collector.reset();
            setError(std::string("Native sampling error: ") + e.what());
        }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(c.interval), [this] { return cancel_.load(); });
    }
}
}
