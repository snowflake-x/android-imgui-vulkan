#include "monitor/collector.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace vulkanscope::monitor;

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Config gpuConfig(bool memory, bool timing) {
    Config config;
    for (auto &entry : config.enabled) entry.second = false;
    config.root = true;
    config.enabled["gpu_memory"] = memory;
    config.enabled["gpu_time"] = timing;
    return config;
}

bool missing(const Values &values, const std::string &key) {
    const auto found = values.find(key);
    return found != values.end() && found->second.rfind("N/A", 0) == 0;
}

/** @brief 注入真实采集器依赖的 root 输出，未知命令立即失败以捕获越权或额外探测。 */
struct Device {
    std::atomic<bool> cancelled{false};
    bool authorized = true;
    std::string package = "com.game.app", processPackage = package;
    int user = 0, pid = 123, uid = 10001;
    std::uint64_t start = 456, kernelBytes = 1000000, importedBytes = 2000000;
    int authorizations = 0, identities = 0, memories = 0, surfaceLists = 0;
    int captures = 0, cleanups = 0;
    bool cleanupFails = false;

    std::string timingCapture() const {
        const auto identity = processPackage + ":" + std::to_string(uid) + ":" +
                              std::to_string(pid) + ":" + std::to_string(start);
        return "VSCOPE_GPU_BACKEND=window\nVSCOPE_IDENTITY_BEGIN\n" + identity +
            "\nVSCOPE_IDENTITY_END\nVSCOPE_FORMAT_BEGIN\n"
            "name: gpu_work_period\nID: 777\nformat:\n"
            " field:unsigned int gpu_id; offset:8; size:4; signed:0;\n"
            " field:unsigned int uid; offset:12; size:4; signed:0;\n"
            " field:u64 start_time_ns; offset:16; size:8; signed:0;\n"
            " field:u64 end_time_ns; offset:24; size:8; signed:0;\n"
            " field:u64 total_active_duration_ns; offset:32; size:8; signed:0;\n"
            "VSCOPE_FORMAT_END\nVSCOPE_TRACE_BEGIN\n"
            "# entries-in-buffer/entries-written: 2/2\n"
            " worker-999 [000] .... 1.9: gpu_work_period: gpu_id=1 uid=" + std::to_string(uid) +
            " start_time_ns=1000000000 end_time_ns=1900000000 total_active_duration_ns=100000000\n"
            " worker-999 [000] .... 2.9: gpu_work_period: gpu_id=1 uid=" + std::to_string(uid) +
            " start_time_ns=2000000000 end_time_ns=2900000000 total_active_duration_ns=300000000\n"
            "VSCOPE_TRACE_END\nVSCOPE_STATS_BEGIN\nVSCOPE_CPU=cpu0\n"
            "overrun: 0\ncommit overrun: 0\ndropped events: 0\n"
            "VSCOPE_STATS_END\nVSCOPE_GPU_COMPLETE\n";
    }

    CommandResult command(const std::string &script, int timeout) {
        require(timeout > 0 && timeout <= 30000, "all root operations have a finite timeout");
        if (script == "id -u") {
            ++authorizations;
            return {authorized, authorized ? "0\n" : "", authorized ? "" : "permission denied"};
        }
        const bool cleanup = script.find("/instances/vscope_gpu_") != std::string::npos &&
                             script.find("exit $cleanup_ok") != std::string::npos;
        require(authorized && timeout <= (cleanup ? 6500 : 5000),
                "only independent trace cleanup receives the extended telemetry timeout");
        if (script.find("dumpsys activity activities") != std::string::npos)
            return {true, "ResumedActivity: ActivityRecord{x u" + std::to_string(user) + " " +
                          package + "/.Main}", ""};
        if (script.find("PROCESS_COMPLETE") != std::string::npos) {
            ++identities;
            return {true, "PID=" + std::to_string(pid) + "\nUID=" + std::to_string(uid) +
                          "\nSTART=" + std::to_string(start) + "\nCMD=" + processPackage +
                          "\nEND\nPROCESS_COMPLETE\n", ""};
        }
        if (script.find("MEMORY_COMPLETE") != std::string::npos) {
            ++memories;
            return {true, "SOURCE=/sys/class/kgsl/kgsl/proc/" + std::to_string(pid) +
                          "\nkernel=" + std::to_string(kernelBytes) +
                          "\nimported_mem=" + std::to_string(importedBytes) +
                          "\nEND\nMEMORY_COMPLETE\n", ""};
        }
        if (script.find("dumpsys SurfaceFlinger --list") != std::string::npos) {
            ++surfaceLists;
            return {false, "", "SurfaceFlinger denied"};
        }
        if (script.find("VSCOPE_GPU_BACKEND") != std::string::npos) {
            ++captures;
            return {true, timingCapture(), ""};
        }
        if (cleanup) {
            ++cleanups;
            return {!cleanupFails, "", cleanupFails ? "permission denied" : ""};
        }
        throw std::runtime_error("unexpected root query: " + script.substr(0, 240));
    }

    Collector collector() {
        return Collector(cancelled, [this](const std::string &script, int timeout) {
            return command(script, timeout);
        });
    }
};

void permissionGates() {
    Device device;
    auto collector = device.collector();
    auto config = gpuConfig(true, true);
    config.root = false;
    auto values = collector.sample(config);
    require(device.authorizations == 0 && device.identities == 0 && device.memories == 0 &&
                device.captures == 0 && missing(values, "gpu_memory") && missing(values, "gpu_time"),
            "disabled root never queries GPU processes, memory, or timing");
    require(values.count("gpu_memory_mb") == 0 && values.count("gpu_time_ms") == 0,
            "disabled root cannot publish GPU numeric values");
    device.authorized = false;
    config.root = true;
    values = collector.sample(config);
    collector.sample(config);
    require(device.authorizations == 1 && device.identities == 0 && device.memories == 0 &&
                device.captures == 0 && missing(values, "gpu_memory") && missing(values, "gpu_time"),
            "denied root does not attempt telemetry or repeatedly request authorization");
}

void independentMemory() {
    Device device;
    auto collector = device.collector();
    auto config = gpuConfig(true, false);
    auto values = collector.sample(config);
    require(values.at("gpu_memory_mb") == "3.00" &&
                values.at("gpu_identity") == "com.game.app:10001:123:456" &&
                values.at("gpu_memory_source").find("/sys/class/kgsl/kgsl/proc/123") == 0 &&
                values.at("gpu_memory_source").find("shared RAM") != std::string::npos,
            "collector propagates verified process memory and its actual shared-memory accounting source");
    require(device.identities == 1 && device.memories == 1 && device.captures == 0 &&
                device.surfaceLists == 0 && values.count("gpu_time_ms") == 0,
            "memory-only collection needs one identity lookup and no frame or timing probe");
    config.enabled["fps"] = true;
    values = collector.sample(config);
    require(device.surfaceLists == 1 && values.at("gpu_memory_mb") == "3.00" && missing(values, "fps") &&
                !collector.frameSample().fps,
            "SurfaceFlinger failure cannot erase independent GPU memory measurements");

    config = gpuConfig(false, false);
    const int identities = device.identities, memories = device.memories;
    values = collector.sample(config);
    require(device.identities == identities && device.memories == memories && device.captures == 0 &&
                values.count("gpu_identity") == 0 && values.count("gpu_memory_mb") == 0 &&
                values.count("gpu_time_ms") == 0,
            "disabled GPU fields perform no GPU probe and do not retain earlier measurements");
}

void foregroundIsolation() {
    Device device;
    auto collector = device.collector();
    const auto config = gpuConfig(true, false);
    require(collector.sample(config).at("gpu_memory_mb") == "3.00", "first process has GPU memory");
    device.package = "com.other.game";
    auto values = collector.sample(config);
    require(missing(values, "gpu_memory") && values.count("gpu_memory_mb") == 0 &&
                values.count("gpu_identity") == 0 && device.memories == 1,
            "switching apps rejects stale process identity and never reuses the previous memory number");
    device.processPackage = device.package;
    device.kernelBytes = 5000000;
    values = collector.sample(config);
    require(values.at("gpu_memory_mb") == "7.00" &&
                values.at("gpu_identity").find("com.other.game:") == 0,
            "a verified new app publishes only its own measurement");
    device.user = 10;
    values = collector.sample(config);
    require(missing(values, "gpu_memory") && values.count("gpu_memory_mb") == 0 &&
                values.count("gpu_identity") == 0 && device.memories == 2,
            "switching Android users rejects the same package under the former UID");
    device.uid = 1010001;
    device.start = 789;
    device.kernelBytes = 9000000;
    values = collector.sample(config);
    require(values.at("gpu_memory_mb") == "11.00" &&
                values.at("gpu_identity") == "com.other.game:1010001:123:789",
            "new user and process generation replace all earlier GPU identity and values");
    device.package = "dev.snowflake.vulkan";
    const int identities = device.identities;
    values = collector.sample(config);
    require(device.identities == identities && values.count("gpu_memory_mb") == 0 &&
                values.count("gpu_identity") == 0 && missing(values, "gpu_memory"),
            "returning to the monitor discards game readings without probing its own GPU");
}

void independentTiming() {
    Device device;
    auto collector = device.collector();
    auto config = gpuConfig(true, true);
    auto values = collector.sample(config);
    require(!collector.gpuTimingBlocked() && device.identities == 1 && device.memories == 1 &&
                device.captures == 1 && device.cleanups == 1 &&
                values.at("gpu_memory_mb") == "3.00" && values.at("gpu_time_ms") == "200.000000" &&
                values.at("gpu_time_min_ms") == "100.000000" && values.at("gpu_time_max_ms") == "300.000000",
            "memory and timing share one verified process lookup and propagate measured values");
    require(values.at("gpu_time_unit") == "ms/window" && values.at("gpu_time_window_ms") == "900.000000" &&
                values.at("gpu_time_source").find("not per frame") != std::string::npos &&
                values.at("gpu_time").find("shared UID") != std::string::npos,
            "collector preserves the source's window and shared-UID scope rather than claiming per-frame GPU time");
    config.enabled["gpu_memory"] = false;
    values = collector.sample(config);
    require(device.identities == 2 && device.memories == 1 && device.captures == 2 && device.cleanups == 2 &&
                values.count("gpu_memory_mb") == 0 && values.at("gpu_time_ms") == "200.000000",
            "timing-only collection never executes a memory query or retains previous memory values");
    device.user = 10;
    values = collector.sample(config);
    require(missing(values, "gpu_time") && values.count("gpu_time_ms") == 0 &&
                values.count("gpu_time_min_ms") == 0 && values.count("gpu_time_max_ms") == 0 &&
                values.count("gpu_time_unit") == 0 && device.captures == 2 && device.cleanups == 2,
            "user mismatch clears every old GPU timing value without creating a trace instance");
    device.package = "com.other.game";
    values = collector.sample(config);
    require(missing(values, "gpu_time") && values.count("gpu_time_ms") == 0 && device.captures == 2,
            "app switch cannot display the previous app's GPU timing");
}

void cleanupFailurePersists() {
    Device device;
    auto collector = device.collector();
    auto config = gpuConfig(true, true);
    device.cleanupFails = true;
    auto values = collector.sample(config);
    require(collector.gpuTimingBlocked() && device.captures == 1 && device.cleanups == 1 &&
                values.at("gpu_memory_mb") == "3.00" &&
                missing(values, "gpu_time") && values.at("gpu_time").find("cleanup failed") != std::string::npos &&
                values.count("gpu_time_ms") == 0,
            "failed trace cleanup suppresses captured timing but leaves independent memory valid");
    device.cleanupFails = false;
    device.kernelBytes = 5000000;
    values = collector.sample(config);
    require(device.memories == 2 && device.captures == 1 && device.cleanups == 1 &&
                values.at("gpu_memory_mb") == "7.00" && missing(values, "gpu_time"),
            "subsequent samples refresh memory while refusing another trace after cleanup failure");
    collector.reset();
    config.root = false;
    values = collector.sample(config);
    require(values.count("gpu_memory_mb") == 0 && values.count("gpu_time_ms") == 0 &&
                device.memories == 2 && device.captures == 1,
            "revoking root discards both GPU values immediately after a cleanup failure");
    config.root = true;
    values = collector.sample(config);
    require(device.authorizations == 2 && device.memories == 3 && device.captures == 1 && device.cleanups == 1 &&
                values.at("gpu_memory_mb") == "7.00" && missing(values, "gpu_time") &&
                values.at("gpu_time").find("cleanup failed") != std::string::npos,
            "collector reset and root reauthorization cannot clear the unsafe-trace latch");
    Collector restarted(device.cancelled, [&](const std::string &script, int timeout) {
        return device.command(script, timeout);
    }, collector.gpuTimingBlocked());
    device.kernelBytes = 9000000;
    values = restarted.sample(config);
    require(restarted.gpuTimingBlocked() && device.authorizations == 3 && device.memories == 4 &&
                device.captures == 1 && device.cleanups == 1 && values.at("gpu_memory_mb") == "11.00" &&
                values.count("gpu_time_ms") == 0 && missing(values, "gpu_time") &&
                values.at("gpu_time").find("cleanup failed") != std::string::npos,
            "recreated sampling workers inherit the unsafe-trace latch while still refreshing GPU memory");
}
}

int main() {
    try {
        permissionGates();
        independentMemory();
        foregroundIsolation();
        independentTiming();
        cleanupFailurePersists();
        std::cout << "GPU collection regression tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
