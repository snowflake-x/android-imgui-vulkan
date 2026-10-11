#include "monitor/model.h"
#include "monitor/process.h"
#include "monitor/session.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
using namespace vulkanscope::monitor;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double a, double b) { require(std::abs(a - b) < .001, "numeric mismatch"); }
void frames() {
    auto trace = ParseLatency(
        "16666666\n1 1000000000 3\n1 1000000000 3\n1 0 3\n1 9223372036854775807 3\n1 2000000001 3\n",
        2000000000);
    require(trace.supported && trace.presents.size() == 1, "filter pending, duplicate and future timestamps");
    require(!ParseLatency("Permission denied", 1).supported, "reject denied latency");
    FrameStats stats;
    require(!stats.update("a", trace, 1000000000).fps, "first sample is warmup");
    auto next = stats.update("a", {true, {1000000000, 1100000000, 1200000000}}, 2000000000);
    near(*next.fps, 2);
    near(*next.mean, 100);
    near(*next.p95, 100);
    near(*stats.update("a", {true, {1000000000, 1100000000, 1200000000}}, 3000000000).fps, 0);
    auto ring = stats.update("a", {true, {3100000000, 3200000000, 3300000000}}, 4000000000);
    require(ring.truncated, "flag overwritten history");
    near(*ring.fps, 10);
    require(!stats.update("b", trace, 5000000000).fps, "new surface resets baseline");
    stats.reset();
    require(!stats.update("b", trace, 6000000000).fps, "sleep resets baseline");
}
void identity() {
    auto f =
        ParseForeground("mResumedActivity: ActivityRecord{ab u10 "
                        "com.game.app/.Main}\ntopResumedActivity=ActivityRecord{cd u11 com.other.app/.Play}",
                        true);
    require(f && f->user == 11 && f->package == "com.other.app", "foreground priority and Android user");
    f = ParseForeground("    topResumedActivity=ActivityRecord{a u0 com.android.launcher/.Launcher}\n"
                        "    mResumedActivity: ActivityRecord{b u0 com.android.launcher/.Launcher}\n"
                        "  ResumedActivity: ActivityRecord{c u10 com.game.app/.Play}\n"
                        "    topResumedActivity=ActivityRecord{d u0 com.other.app/.Play}\n", true);
    require(f && f->user == 10 && f->package == "com.game.app",
            "global focused activity overrides all per-task resumed records on recent Android");
    require(!ParseForeground("mLastResumedActivity: ActivityRecord{a u0 com.old.app/.Main}\n"
                             "last topResumedActivity=ActivityRecord{b u0 com.old.app/.Main}", true),
            "historical and incidental activity markers cannot select a foreground app");
    f = ParseForeground("  mFocusedApp=ActivityRecord{a u0 com.android.launcher/.Launcher}\n"
                        "  mCurrentFocus=Window{b u10 com.game.app/com.game.app.Play}\n", false);
    require(f && f->user == 10 && f->package == "com.game.app",
            "window focus fallback retains Android user and prefers the current focused window");
    f = ParseForeground("mFocusedApp=ActivityRecord{a u0 com.old.game/.Main}\n"
                        "mCurrentFocus=Window{b u0 dev.snowflake.vulkan/.MainActivity}\n", false);
    require(f && f->package == "dev.snowflake.vulkan",
            "monitor window focus must not fall through to a previously focused game");
    require(!ParseForeground("mFocusedApp=ActivityRecord{a u0 com.old.game/.Main}\n"
                             "mCurrentFocus=null\n", false),
            "explicit absent window focus cannot select a stale focused application");
    const std::string task = "topResumedActivity=ActivityRecord{a u0 com.android.launcher/.Launcher}\n";
    const std::string game = "topResumedActivity=ActivityRecord{b u10 com.game.app/.Play}\n";
    require(!ParseForeground(task + game + task, true),
            "conflicting task identities remain ambiguous even if a later record repeats the first");
    require(!ParseForeground(game + "topResumedActivity=ActivityRecord{c u11 com.game.app/.Play}\n", true),
            "resumed identities from different Android users are ambiguous");
    f = ParseForeground(game + game, true);
    require(f && f->user == 10 && f->package == "com.game.app",
            "duplicate records for the same package and Android user are unambiguous");
    require(!ParseForeground(task + "  ResumedActivity: null\n", true) &&
                !ParseForeground("  ResumedActivity=null\n" + task, true),
            "explicit absent global focus overrides task records in either dump order");
    auto names = Surfaces(
        "SurfaceView[com.game.app/.Main]\ncom.game.app.clone/Main\ndev.snowflake.vulkan.PerformanceHUD\n",
        "com.game.app", "");
    require(names.size() == 1, "exclude other package and HUD layers");
    require(Surfaces("com.other.app/Main", "com.game.app", "com.other.app/Main").empty(),
            "override cannot escape package");
    require(Quote("a'b;$(id)") == "'a'\\''b;$(id)'", "shell quoting must preserve literal content");
    require(!ValidPowerPath("/sys/../proc/stat") && !ValidPowerPath("/sys/a;id") &&
                ValidPowerPath("/sys/class/power/value"),
            "sensor path validation");
}
void units() {
    auto a = CpuTicks("cpu 100 10 20 200 30 0 0 0 1000 1000"),
         b = CpuTicks("cpu 110 10 20 210 30 0 0 0 2000 2000");
    require(a && b, "CPU ticks");
    near(*CpuLoad(*a, *b), 50);
    require(!CpuLoad(*a, *a) && !CpuLoad(*b, *a), "reset/unchanged counters unavailable");
    require(!CpuTicks("cpu -1 0 0 0 0 0 0 0"), "negative counter rejected");
    near(*GpuLoad("25 100"), 25);
    require(!GpuLoad("1 0") && !GpuLoad("101 100"), "invalid GPU counters");
    require(!Number("nan") && !Number("10 extra"), "strict numeric sensor parser");
}
std::string telemetry() {
    return R"({"schemaVersion":1,"packageName":"com.game.app","engine":"Unity","source":"pre-upscale","pid":123,"elapsedRealtimeMs":10000,"renderWidth":1280,"renderHeight":720,"outputWidth":1920,"outputHeight":1080})";
}
void engine() {
    std::string error;
    auto json = telemetry();
    auto t = ParseTelemetry(json, "com.game.app", 10001, error);
    require(t && t->width == 1280 && t->outputWidth == 1920, "internal and output remain separate");
    require(!ParseTelemetry(json, "com.other.app", 10001, error), "package isolation");
    require(!ParseTelemetry(json, "com.game.app", 15001, error), "expire stale report");
    require(!ParseTelemetry(json, "com.game.app", 9999, error), "reject future report");
    require(!ParseTelemetry(json + "garbage", "com.game.app", 10001, error), "reject trailing input");
    auto bad = json;
    bad.replace(bad.find("1280"), 4, "1280.0");
    require(!ParseTelemetry(bad, "com.game.app", 10001, error), "no float coercion");
    bad = json;
    bad.insert(1, "\"pid\":456,");
    require(!ParseTelemetry(bad, "com.game.app", 10001, error), "reject duplicate keys");
    bad = json;
    bad.replace(bad.find("1280"), 4, "01280");
    require(!ParseTelemetry(bad, "com.game.app", 10001, error), "reject invalid leading zero");
    bad = json;
    bad.replace(bad.find("pre-upscale"), 11, "pre\\u002Dupscale");
    require(ParseTelemetry(bad, "com.game.app", 10001, error).has_value(), "JSON escaped ASCII accepted");
}
void config() {
    const auto path = "/tmp/vulkanscope-config-" + std::to_string(getpid());
    std::string error;
    Config c;
    c.root = true;
    c.autoRoot = false;
    c.language = "zh-Hans";
    c.hudGraph = false;
    c.hudExtras = true;
    c.surface = "SurfaceView[com.game.app] 'quoted'";
    c.powerPath = "/sys/sensor/value";
    c.textSize = 100;
    c.interval = 123;
    c.order = {"fps", "fps", "unknown", "foreground"};
    c.enabled["fps"] = false;
    require(SaveConfig(path, c, error), "save config");
    auto loaded = LoadConfig(path);
    require(loaded.language == "zh-Hans", "persist language preference");
    require(!loaded.autoRoot, "persist startup root opt-out");
    require(!loaded.hudGraph && loaded.hudExtras, "persist classic HUD options");
    require(loaded.root && loaded.surface == c.surface && loaded.textSize == 24 && loaded.interval == 1000 &&
                !loaded.has("fps"),
            "persistence and bounds");
    require(loaded.order.size() == Metrics().size() && loaded.order.front() == "fps", "order deduplication");
    require(!SaveConfig(path + "/missing", c, error), "failed save is reported");
    std::ofstream(path) << "VulkanScopeHUD 1\n0 500 16 90 2 0.000001\n\"layer\"\n\"\"\nfps 0\n";
    auto legacy = LoadConfig(path);
    require(legacy.hudGraph && !legacy.hudExtras && !legacy.has("fps") && legacy.corner == 2,
            "version one preserves selections and adopts classic defaults");
    std::ofstream(path) << "VulkanScopeHUD 2\n0 500 16 90 2 0.000001\n\"layer\"\n\"\"\n0 1\nfps 0\n";
    auto versionTwo = LoadConfig(path);
    require(versionTwo.language == "system" && !versionTwo.hudGraph && versionTwo.hudExtras,
            "version two adopts system language without changing HUD layout");
    std::ofstream(path) << "VulkanScopeHUD 3\n0 500 16 90 2 0.000001\n\"layer\"\n\"\"\n0 1\n\"en\"\nfps 0\n";
    const auto versionThree = LoadConfig(path);
    require(versionThree.autoRoot && !versionThree.root && !versionThree.has("fps"),
            "legacy configuration adopts automatic startup authorization without fabricating a grant");
    std::ofstream(path) << "bad config";
    require(!LoadConfig(path).root, "invalid config fails closed");
    std::remove(path.c_str());
}
void presets() {
    const auto path = "/tmp/vulkanscope-preset-" + std::to_string(getpid());
    std::string error;
    Config config;
    config.language = "zh-Hans";
    config.hudGraph = false;
    config.hudExtras = true;
    config.surface = "SurfaceView[com.game.app]";
    const auto originalOrder = config.order;
    for (auto preset : {MetricPreset::Game, MetricPreset::Hardware}) {
        config.root = preset == MetricPreset::Hardware;
        ApplyMetricPreset(config, preset);
        for (const char *id : {"gpu_model", "fps", "frame_time", "app_memory"})
            require(config.has(id), "field presets retain every classic HUD core value");
        require(config.has("engine_resolution") == (preset == MetricPreset::Game) &&
                    config.has("cpu_load") == (preset == MetricPreset::Hardware),
                "presets replace their optional fields when switched");
        require(config.root == (preset == MetricPreset::Hardware) && config.language == "zh-Hans" &&
                    !config.hudGraph && config.hudExtras && config.surface == "SurfaceView[com.game.app]" &&
                    config.order == originalOrder && !config.has("graphics_api"),
                "presets preserve permissions and presentation without enabling call tracing");
        for (const char *id : {"gpu_model", "fps", "frame_time", "app_memory"})
            config.enabled[id] = false;
        require(SaveConfig(path, config, error), "save manual core field selection after preset");
        const auto loaded = LoadConfig(path);
        for (const char *id : {"gpu_model", "fps", "frame_time", "app_memory"})
            require(!loaded.has(id), "manual core field opt-out remains configurable and persists");
    }
    std::remove(path.c_str());
}
void processes() {
    std::atomic<bool> cancelled{false};
    auto r = RunCommand({"sh", "-c", "printf hello"}, cancelled, 1000);
    require(r.success && r.output == "hello", "execute and capture");
    r = RunCommand({"sh", "-c", "i=0; while [ $i -lt 10000 ]; do printf 1234567890; i=$((i+1)); done"},
                   cancelled, 2000, 100);
    require(r.success && r.output.size() == 100, "drain output beyond capture bound");
    auto start = MonotonicNs();
    r = RunCommand({"sh", "-c", "sleep 10"}, cancelled, 80);
    require(!r.success && MonotonicNs() - start < 1000000000LL, "deadline kills process group");
    std::thread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancelled = true;
    });
    start = MonotonicNs();
    r = RunCommand({"sh", "-c", "sleep 10"}, cancelled, 10000);
    cancel.join();
    require(!r.success && MonotonicNs() - start < 1000000000LL, "cancellation is prompt");
}
void processMemory() {
    std::atomic<bool> cancelled{false};
    int user = 10;
    Collector collector(cancelled, [&](const std::string &script, int) {
        if (script == "id -u") return CommandResult{true, "0\n", ""};
        if (script.find("dumpsys activity activities") != std::string::npos)
            return CommandResult{true, "topResumedActivity=ActivityRecord{cd u10 com.game.app/.Play}", ""};
        if (script == "pidof 'com.game.app'") return CommandResult{true, "123\n", ""};
        if (script == "cat /proc/123/status")
            return CommandResult{true, "Uid:\t" + std::to_string(user * 100000 + 12345) + "\nVmRSS:\t1024 kB\n", ""};
        return CommandResult{};
    });
    Config config;
    for (auto &item : config.enabled) item.second = false;
    config.root = true;
    config.enabled["app_memory"] = true;
    auto values = collector.sample(config);
    require(values["app_memory_mb"] == "1.05", "RSS converts KiB to decimal MB");
    user = 11;
    values = collector.sample(config);
    require(values.count("app_memory_mb") == 0, "reject process from another Android user");
}
void foregroundQueries() {
    const auto path = "/tmp/vulkanscope-foreground-" + std::to_string(getpid());
    {
        std::ofstream dump(path);
        dump << "ACTIVITY MANAGER ACTIVITIES\n";
        for (int i = 0; i < 1000; ++i) {
            dump << "      topResumedActivity=ActivityRecord{a u0 com.android.launcher/.Launcher}\n";
            dump << std::string(100, 'x') << '\n';
        }
        dump << "  ResumedActivity: ActivityRecord{b u10 com.game.app/.Play}\n";
        dump << "mLastResumedActivity: ActivityRecord{c u0 com.old.app/.Main}\n";
    }
    std::atomic<bool> cancelled{false};
    int activityExit = 0, windowExit = 0, windowCalls = 0;
    std::string window = "mCurrentFocus=Window{d u11 com.fallback.app/.Play}\n";
    Collector collector(cancelled, [&](const std::string &script, int timeout) {
        if (script == "id -u") return CommandResult{true, "0\n", ""};
        const bool activity = script.find("dumpsys activity activities") != std::string::npos;
        if (!activity && script.find("dumpsys window windows") == std::string::npos)
            throw std::runtime_error("unexpected command during foreground query test");
        if (!activity) ++windowCalls;
        const auto produce = activity ? "cat " + Quote(path) : "printf '%s' " + Quote(window);
        const auto fixture = "dumpsys() { " + produce + "; return " +
                             std::to_string(activity ? activityExit : windowExit) + "; }; " + script;
        auto result = RunCommand({"bash", "-c", fixture}, cancelled, timeout);
        require(result.output.size() <= 24576, "focus extraction stays below process output capture bound");
        return result;
    });
    Config config;
    for (auto &item : config.enabled) item.second = false;
    config.root = true;
    auto values = collector.sample(config);
    require(values["foreground"] == "com.game.app" && values["foreground_user"] == "10" && windowCalls == 0,
            "execute bounded extraction and retain authoritative focus after a dump larger than 32 KiB");
    activityExit = 7;
    values = collector.sample(config);
    require(values["foreground"] == "com.fallback.app" && values["foreground_user"] == "11" && windowCalls == 1,
            "nonzero activity dump exit survives the filter pipeline and falls back to window focus");
    require(values["activity_query"].find("Command denied or failed") != std::string::npos,
            "successful window fallback retains the ActivityManager failure for diagnosis");
    activityExit = 0;
    std::ofstream(path) << "topResumedActivity=ActivityRecord{a u0 com.android.launcher/.Launcher}\n"
                          "topResumedActivity=ActivityRecord{b u10 com.game.app/.Play}\n";
    values = collector.sample(config);
    require(values["foreground"] == "com.fallback.app" && windowCalls == 2,
            "conflicting task focus without a global record falls back to the window query");
    std::ofstream(path) << "topResumedActivity=ActivityRecord{a u0 com.android.launcher/.Launcher}\n"
                          "ResumedActivity: null\n";
    values = collector.sample(config);
    require(values["foreground"] == "com.fallback.app" && windowCalls == 3,
            "explicit null global focus falls back instead of selecting a stale launcher task");
    window = "mFocusedApp=ActivityRecord{a u11 com.old.game/.Main}\n"
             "mCurrentFocus=Window{b u0 dev.snowflake.vulkan/.MainActivity}\n";
    values = collector.sample(config);
    require(values.count("foreground_user") == 0 && values["foreground"] == "N/A (Monitor is in foreground)",
            "window fallback observes the monitor in foreground without retaining an old game");
    std::ofstream(path) << "Permission Denial: can't dump ActivityManager\n";
    activityExit = 0;
    windowExit = 4;
    window = "Error: window service unavailable\n";
    values = collector.sample(config);
    require(values.count("foreground_user") == 0 &&
                values["foreground"].find("Permission Denial") != std::string::npos &&
                values["foreground"].find("window service unavailable") != std::string::npos,
            "missing foreground distinguishes permission output and a failed window query");
    require(values["app_memory"] == values["foreground"],
            "missing foreground carries the same useful reason into process-memory diagnostics");
    config.root = false;
    values = collector.sample(config);
    require(values["app_memory"] == "N/A (Root disabled)", "root-disabled memory is explained explicitly");
    std::remove(path.c_str());
}
void authorization() {
    std::atomic<bool> cancelled{false};
    int calls = 0;
    Collector collector(cancelled, [&](const std::string &script, int timeout) {
        ++calls;
        require(script == "id -u" && timeout == 30000, "only explicit authorization is attempted");
        return CommandResult{false, "", "denied"};
    });
    Config config;
    collector.sample(config);
    require(calls == 0, "root disabled performs no privileged commands");
    config.root = true;
    collector.sample(config);
    collector.sample(config);
    require(calls == 1, "denial must not repeatedly request root");
    config.root = false;
    collector.sample(config);
    config.root = true;
    collector.sample(config);
    require(calls == 2, "explicit opt-in change permits retry");
}
void graphicsAuthorization() {
    std::atomic<bool> cancelled{false};
    bool rootGranted = true, targetAvailable = true;
    int processQueries = 0;
    const Collector::CommandRunner runner = [&](const std::string &script, int) {
        if (script == "id -u") return CommandResult{rootGranted, rootGranted ? "0\n" : "", "denied"};
        if (script.find("dumpsys activity activities") != std::string::npos)
            return CommandResult{true, targetAvailable
                ? "topResumedActivity=ActivityRecord{cd u0 com.game.app/.Play}" : "", ""};
        if (script.find("dumpsys window windows") != std::string::npos) return CommandResult{true, "", ""};
        if (script.find("pidof ") != std::string::npos) {
            ++processQueries;
            return CommandResult{false, "", "process exited"};
        }
        throw std::runtime_error("unexpected graphics collection command");
    };
    Config config;
    for (auto &field : config.enabled) field.second = false;
    config.root = true;
    Collector collector(cancelled, runner);
    auto values = collector.sample(config);
    require(values["root_status"] == "granted" && values["graphics_api_status"] == "disabled" &&
                values["graphics_api"] == "OFF" && processQueries == 0,
            "root authorization does not implicitly enable graphics tracing");
    config.enabled["graphics_api"] = true;
    values = collector.sample(config);
    require(values["graphics_api_status"] == "target_unavailable" && processQueries == 1 &&
                !values["graphics_api_details"].empty(),
            "graphics process discovery propagates the precise trace failure");
    targetAvailable = false;
    values = collector.sample(config);
    require(values["graphics_api_status"] == "target_unavailable" && processQueries == 1,
            "missing foreground prevents probing and explains the target failure");
    config.root = false;
    values = collector.sample(config);
    require(values["graphics_api_status"] == "root_disabled", "enabled tracing requires root opt-in");
    rootGranted = false;
    config.root = true;
    values = collector.sample(config);
    require(values["graphics_api_status"] == "root_unavailable", "denied root is distinct from missing symbols");
    rootGranted = targetAvailable = true;
    Collector blocked(cancelled, runner, false, true);
    values = blocked.sample(config);
    require(values["graphics_api_status"] == "cleanup_failed" && blocked.graphicsTracingBlocked() &&
                processQueries == 1,
            "a collector restarted after failed cleanup must not create more probes");
    blocked.reset();
    config.root = false;
    blocked.sample(config);
    config.root = true;
    values = blocked.sample(config);
    require(values["graphics_api_status"] == "cleanup_failed" && blocked.graphicsTracingBlocked() &&
                processQueries == 1, "reset and root toggles cannot clear the cleanup failure latch");
}
void systemSnapshots() {
    Session session;
    session.initialize("/tmp/vulkanscope-system-" + std::to_string(getpid()), [] {
        PlatformSample sample;
        sample.system = "Native system snapshot";
        return sample;
    });
    session.startSystemInfo();
    for (int i = 0; i < 100 && session.system().empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    session.stopSystemInfo();
    require(session.system() == "Native system snapshot" && !session.running(),
            "system snapshots update without a HUD session or renderer-side collection");
    session.startSystemInfo();
    session.stopSystemInfo();
}
/** @brief 启动授权仅在后台执行一次，真实 UID 0 才开启采样，并尊重并发关闭及取消。 */
void startupRoot() {
    const auto directory = "/tmp/vulkanscope-startup-root-" + std::to_string(getpid());
    std::filesystem::create_directories(directory);
    for (int mode = 0; mode < 5; ++mode) {
        std::atomic<int> calls{0};
        std::atomic<bool> release{false};
        Session session([&](const std::string &script, int timeout) {
            ++calls;
            require(script == "id -u" && timeout == 30000, "startup requests only bounded authorization");
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return CommandResult{mode != 1, mode == 2 ? "2000\n" : "0\n", mode == 1 ? "denied" : ""};
        });
        session.initialize(directory, {});
        auto c = session.config();
        c.root = false;
        c.autoRoot = mode != 4;
        require(session.configure(c), "save startup preference");
        session.requestStartupRoot();
        session.requestStartupRoot();
        if (mode == 4) {
            require(session.rootAccess() == RootAccess::Unknown && calls == 0, "opt-out performs no root request");
            continue;
        }
        require(session.rootAccess() == RootAccess::Requesting && !session.running(), "startup does not block or start HUD");
        if (mode == 3) {
            c.autoRoot = false;
            require(session.configure(c), "disable automatic root while waiting");
        }
        release = true;
        for (int wait = 0; wait < 1000 && session.rootAccess() == RootAccess::Requesting; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(calls == 1 && session.rootAccess() != RootAccess::Requesting, "denial and success never automatically reprompt");
        require(session.config().root == (mode == 0), "only verified grants enable sampling, without overriding a pending opt-out");
        require(!session.config().has("graphics_api") && !session.config().has("gpu_time"), "authorization leaves tracing opt-ins unchanged");
        session.shutdownDiagnostics();
        require(LoadConfig(directory + "/native-hud.conf").root == (mode == 0), "persist startup authorization outcome");
    }
    std::filesystem::remove_all(directory);
}
void lifecycle() {
    Session session;
    std::atomic<int> calls{0};
    session.initialize("/tmp/vulkanscope-lifecycle-" + std::to_string(getpid()), [&] {
        ++calls;
        PlatformSample p;
        p.interactive = false;
        p.system = "device";
        return p;
    });
    session.start();
    session.start();
    for (int i = 0; i < 100 && calls == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    session.stop();
    require(calls == 1 && !session.running(), "single worker, interruptible sleep and stop");
    require(session.hud().values.at("graphics_api_status") == "cancelled",
            "a stopped HUD cannot display an earlier graphics API as current");
    auto before = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(calls == before, "no publication after joined stop");
    session.start();
    session.stop();
    require(!session.running(), "rapid restart/stop");
    session.fail("Surface lost");
    session.stop();
    require(session.report() == "Surface lost", "teardown preserves terminal failure for UI diagnosis");
}
}
int main() {
    try {
        frames();
        identity();
        units();
        engine();
        config();
        presets();
        processes();
        lifecycle();
        authorization();
        graphicsAuthorization();
        processMemory();
        foregroundQueries();
        systemSnapshots();
        startupRoot();
        std::cout << "Native monitor regression tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
