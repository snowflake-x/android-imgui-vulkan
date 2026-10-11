#include "monitor/model.h"
#include "monitor/process.h"
#include "monitor/session.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
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
        if (script == "dumpsys activity activities")
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
        processMemory();
        systemSnapshots();
        std::cout << "Native monitor regression tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
