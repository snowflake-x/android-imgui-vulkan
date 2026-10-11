#include "monitor/collector.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace vulkanscope::monitor;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
Config frameConfig() {
    Config config;
    for (auto &field : config.enabled)
        field.second = false;
    config.root = true;
    config.enabled["fps"] = config.enabled["frame_time"] = true;
    return config;
}
std::string layer(int id) {
    return "SurfaceView[com.game.app/.Main](BLAST)#" + std::to_string(id);
}
std::string trace(std::int64_t base, int batch) {
    std::string text = "16666667\n";
    for (int i = 0; i < 4; ++i)
        text += "1 " + std::to_string(base + (batch * 4 + i) * 16666667LL) + " 3\n";
    return text;
}
void selectionAndRotation() {
    std::atomic<bool> cancelled{false};
    auto config = frameConfig();
    int count = 5, valid = 5, batch = 0;
    std::string package = "com.game.app";
    std::vector<std::string> queries;
    const auto base = MonotonicNs() - 500000000;
    Collector collector(cancelled, [&](const std::string &script, int timeout) {
        if (script == "id -u") return CommandResult{true, "0\n", ""};
        if (script.find("dumpsys activity activities") != std::string::npos)
            return CommandResult{true, "ResumedActivity: ActivityRecord{x u0 " + package + "/.Main}", ""};
        if (script.find("dumpsys SurfaceFlinger --list") != std::string::npos) {
            std::string list;
            for (int i = 1; i <= count; ++i)
                list += "RequestedLayerState{" + layer(i) + " parentId=100 z=-2}\n";
            return CommandResult{true, list, ""};
        }
        require(timeout > 0 && timeout <= 2000, "latency query has a bounded timeout");
        require(script.find("RequestedLayerState{") == std::string::npos,
                "Android debug wrapper must never be passed as a latency layer name");
        queries.push_back(script);
        return CommandResult{true, script == "dumpsys SurfaceFlinger --latency " + Quote(layer(valid))
                                     ? trace(base, batch++) : "16666667\n", ""};
    });
    auto values = collector.sample(config);
    require(values["surface"] == layer(5) && queries.size() == 5 && !collector.frameSample().fps,
            "fifth real buffer layer is found after four header-only layers and first sample warms up");
    queries.clear();
    values = collector.sample(config);
    require(queries.size() == 1 && values["surface"] == layer(5) && collector.frameSample().fps &&
                collector.frameSample().mean && std::abs(*collector.frameSample().mean - 16.666667) < .001,
            "cached active layer provides actual presentation intervals on the next sample");
    config.surface = layer(1);
    queries.clear();
    values = collector.sample(config);
    require(!collector.frameSample().fps && queries.size() == 1 &&
                values["surface_queries"].find("header-only") != std::string::npos,
            "manual override is honored and header-only failure is explained");
    config.surface.clear();
    collector.reset();
    count = 24;
    valid = 21;
    bool found = false;
    for (int poll = 0; poll < 4 && !found; ++poll) {
        queries.clear();
        values = collector.sample(config);
        require(queries.size() <= 8, "candidate probing stays bounded per sample");
        found = values["surface"] == layer(21);
    }
    require(found, "candidate rotation reaches buffer layers beyond the first probe batch");
    valid = 0;
    collector.sample(config);
    queries.clear();
    collector.sample(config);
    require(!queries.empty() && queries.front() != "dumpsys SurfaceFlinger --latency " + Quote(layer(21)),
            "an invalid cached layer loses priority so later candidates receive the full query budget");
    package = "com.other.game";
    queries.clear();
    values = collector.sample(config);
    require(queries.empty() && !collector.frameSample().fps &&
                collector.frameSample().status == "No matching game layers",
            "switching app never reuses another app's cached surface or frame history");
}
void listPipelineAndFailures() {
    const auto path = "/tmp/vulkanscope-surface-list-" + std::to_string(getpid());
    {
        std::ofstream output(path);
        for (int i = 0; i < 3000; ++i)
            output << "RequestedLayerState{com.unrelated.app/Window#" << i << " parentId=1 z=0}\n";
        output << "RequestedLayerState{" << layer(1) << " parentId=100 z=0}\n";
    }
    std::atomic<bool> cancelled{false};
    auto config = frameConfig();
    int exitCode = 0;
    std::string latencyError;
    Collector collector(cancelled, [&](const std::string &script, int timeout) {
        if (script == "id -u") return CommandResult{true, "0\n", ""};
        if (script.find("dumpsys activity activities") != std::string::npos)
            return CommandResult{true, "ResumedActivity: ActivityRecord{x u0 com.game.app/.Main}", ""};
        if (script.find("dumpsys SurfaceFlinger --list") != std::string::npos) {
            const auto fixture = "dumpsys() { cat " + Quote(path) + "; return " +
                                 std::to_string(exitCode) + "; }; " + script;
            return RunCommand({"bash", "-c", fixture}, cancelled, timeout);
        }
        return CommandResult{latencyError.empty(), "16666667\n", latencyError};
    });
    auto values = collector.sample(config);
    require(values["surface_candidates"] == "1" && values["surface_list_output"].size() < 200,
            "filtering before capture retains a game layer beyond a 32 KiB unrelated layer prefix");
    exitCode = 9;
    values = collector.sample(config);
    require(values["surface_list_query"].find("failed") != std::string::npos &&
                collector.frameSample().status.find("Surface list query failed") == 0,
            "pipefail propagates SurfaceFlinger query failure rather than accepting partial output");
    exitCode = 0;
    std::ofstream(path) << "com.unrelated.app/Main#1\n";
    values = collector.sample(config);
    require(values["surface_list_query"] == "ok" && collector.frameSample().status == "No matching game layers",
            "grep finding no game layer is distinct from a failed list command");
    std::ofstream(path) << layer(1) << '\n';
    latencyError = "Command timed out";
    values = collector.sample(config);
    require(collector.frameSample().status.find(latencyError) != std::string::npos &&
                values["surface_queries"].find(layer(1)) != std::string::npos,
            "diagnostics retain the attempted exact layer and latency command failure");
    std::remove(path.c_str());
    const auto truncated = RunCommand({"sh", "-c", "printf 12345"}, cancelled, 1000, 4);
    require(truncated.success && truncated.truncated && truncated.output == "1234",
            "bounded command capture explicitly reports truncation without claiming command failure");
}
}
int main() {
    try {
        selectionAndRotation();
        listPipelineAndFailures();
        std::cout << "Surface collection regression tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
