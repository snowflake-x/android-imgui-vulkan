#include "monitor/gpu_timing.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vulkanscope::monitor;
namespace {
const GpuProcess target{123, 10001, 456, "com.example.game"};
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
std::string section(const std::string &name, const std::string &text) {
    return "VSCOPE_" + name + "_BEGIN\n" + text + "VSCOPE_" + name + "_END\n";
}
std::string field(const std::string &name, unsigned size) {
    return "\tfield:unsigned long " + name + ";\toffset:8;\tsize:" + std::to_string(size) + ";\tsigned:0;\n";
}
std::string format(bool batch) {
    if (batch) return field("id", 4) + field("start", 8) + field("retire", 8) + field("active", 8);
    return field("gpu_id", 4) + field("uid", 4) + field("start_time_ns", 8) +
           field("end_time_ns", 8) + field("total_active_duration_ns", 8);
}
std::string work(const std::string &active = "200000000", const std::string &uid = "10001") {
    return " kgsl_worker-987 [002] ...1 12.900000: gpu_work_period: gpu_id=1 uid=" + uid +
        " start_time_ns=1000000000 end_time_ns=1900000000 total_active_duration_ns=" + active + "\n";
}
std::string batch(const std::string &active = "19200", const std::string &ctx = "7", const std::string &ts = "21") {
    return " kgsl_worker-987 [002] ...1 12.900000: adreno_cmdbatch_retired: ctx=" + ctx +
        " ctx_prio=1 ts=" + ts + " inflight=0 recovery=none flags=none start=2000000 retire=2038400 rb_id=1, r/w=0/0, "
        "q_inflight=0, dq_id=1, submitted_to_rb=1900000 retired_on_gmu=2040000 active=" + active + "\n";
}
CommandResult capture(bool isBatch, const std::string &events) {
    const std::string context = "id: 7 type: GL priority: 1 process: game (123) tid: 125\n";
    std::string out = std::string("VSCOPE_GPU_BACKEND=") + (isBatch ? "batch\n" : "window\n") +
        section("IDENTITY", target.identity() + "\n") + section("FORMAT", format(isBatch));
    if (isBatch) out += section("CONTEXT_BEFORE", context) + section("CONTEXT_AFTER", context);
    const auto count = std::to_string(std::count(events.begin(), events.end(), '\n'));
    out += section("TRACE", "# tracer: nop\n# entries-in-buffer/entries-written: " + count + "/" + count + "   #P:8\n#\n" + events);
    out += section("STATS", "VSCOPE_CPU=cpu0\nentries: 1\noverrun: 0\ncommit overrun: 0\ndropped events: 0\n");
    out += "VSCOPE_GPU_COMPLETE\n";
    return {true, out, {}};
}
void replace(std::string &text, const std::string &before, const std::string &after) {
    const auto at = text.find(before);
    require(at != std::string::npos, "fixture replacement found");
    text.replace(at, before.size(), after);
}
void unavailable(const CommandResult &input, const char *message) {
    auto result = ParseGpuTimingCapture(target, input);
    require(result.values.at("gpu_time").rfind("N/A", 0) == 0 && !result.values.count("gpu_time_ms"), message);
}
void evidence() {
    auto result = ParseGpuTimingCapture(target, capture(false, work()));
    require(result.values.at("gpu_time_ms") == "200.000000", "work period uses explicit ns duration");
    require(result.values.at("gpu_time_window_ms") == "900.000000", "true 900ms window remains visible");
    require(result.values.at("gpu_time_unit") == "ms/window", "window is never reported as frame");
    require(result.values.at("gpu_time_source").find("sharing UID") != std::string::npos, "shared UID scope explicit");
    result = ParseGpuTimingCapture(target, capture(false, work("0")));
    require(result.values.at("gpu_time_ms") == "0.000000", "explicit zero is a valid hardware reading");
    result = ParseGpuTimingCapture(target, capture(true, batch() + batch("38400", "7", "22")));
    require(result.values.at("gpu_time_ms") == "1.500000", "19.2MHz ticks convert to 1.5ms mean");
    require(result.values.at("gpu_time_min_ms") == "1.000000" && result.values.at("gpu_time_max_ms") == "2.000000",
            "batch extrema remain hardware samples");
    require(result.values.at("gpu_time_unit") == "ms/batch", "batch does not become a frame");
    require(!result.values.count("gpu_time_window_ms"), "batch mode has no invented window");
    unavailable(capture(false, ""), "no event is unavailable, not zero");
    unavailable(capture(false, work("1", "10002")), "UID field must match even if worker PID seems plausible");
    unavailable(capture(true, batch("19200", "8")), "unknown context rejected");
    unavailable(capture(true, batch() + batch()), "duplicate batch cannot skew average");
    unavailable(capture(true, batch("40000")), "active cannot exceed SOP-EOP interval");
    unavailable(capture(false, work("900000001")), "active cannot exceed work period");
    unavailable(capture(false, work("-1")), "negative hardware value rejected");
    unavailable(capture(false, work("18446744073709551616")), "overflow rejected");
    unavailable(capture(false, work() + work()), "overlapping work windows rejected");
}
void malformed() {
    auto good = capture(false, work());
    auto input = good;
    input.truncated = true;
    unavailable(input, "command output truncation rejected");
    input = good; replace(input.output, "VSCOPE_GPU_COMPLETE\n", "");
    unavailable(input, "completion marker required");
    input = good; input.output += "VSCOPE_GPU_COMPLETE\n";
    unavailable(input, "duplicate completion marker rejected");
    input = good; input.output += "partial output";
    unavailable(input, "completion marker must be the final output");
    input = good; replace(input.output, target.identity(), "com.example.game:10001:123:999");
    unavailable(input, "process generation mismatch rejected");
    input = good; replace(input.output, "entries-written: 1/1", "entries-written: 1/2");
    unavailable(input, "ringbuffer overwrite rejected");
    input = good; replace(input.output, "entries-written: 1/1", "entries-written: 2/2");
    unavailable(input, "actual records must match trace count");
    input = good; replace(input.output, "entries-written: 1/1", "entries-written: 18446744073709551616/18446744073709551616");
    unavailable(input, "overflow in both trace counters rejected");
    input = good; replace(input.output, "overrun: 0", "overrun: 1");
    unavailable(input, "overrun rejected");
    input = good; replace(input.output, "dropped events: 0", "dropped events: 1");
    unavailable(input, "dropped trace rejected");
    input = good; replace(input.output, "commit overrun: 0\n", "");
    unavailable(input, "missing integrity stats rejected");
    input = good; replace(input.output, "field:unsigned long uid;\toffset:8;\tsize:4", "field:unsigned long uid;\toffset:8;\tsize:8");
    unavailable(input, "wrong field size rejected");
    input = good; replace(input.output, "total_active_duration_ns;", "unknown_active;");
    unavailable(input, "incompatible event format rejected");
    input = capture(true, batch());
    replace(input.output, "VSCOPE_CONTEXT_AFTER_BEGIN\nid: 7 type: GL priority: 1 process: game (123)",
                          "VSCOPE_CONTEXT_AFTER_BEGIN\nid: 7 type: GL priority: 1 process: game (321)");
    unavailable(input, "context PID recycled during capture rejected");
    input = good; input.success = false; input.error = "Command timed out";
    unavailable(input, "timed out capture discarded even with complete-looking output");
}
void lifecycle() {
    for (int failure = 0; failure != 4; ++failure) {
        std::atomic<bool> cancel{false};
        int calls = 0;
        auto runner = [&](const std::string &script, int timeout) -> CommandResult {
            ++calls;
            const std::atomic<bool> active{false};
            require(timeout > 0 && timeout <= 6500, "bounded timing commands");
            require(RunCommand({"sh", "-n", "-c", script}, active, 1000).success, "generated timing shell parses");
            if (calls == 1) {
                require(RunCommand({"sh", "-c", "timeout() { shift 5; sh -n -c \"$1\"; }; " + script}, active, 1000).success,
                        "quoted capture body itself has valid shell syntax");
                require(script.rfind("timeout -s TERM 4 sh -c ", 0) == 0, "root-side timeout present");
                require(script.find("/instances/vscope_gpu_") != std::string::npos, "isolated instance");
                require(script.find("sleep 1.1") != std::string::npos, "window capture covers 900ms emission");
                require(script.find("common_pid") == std::string::npos, "worker PID is never used for ownership");
                require(script.find("trap ") != std::string::npos && script.find("/cmdline") != std::string::npos,
                        "cleanup trap and process checks present");
                if (failure) {
                    cancel = failure == 1;
                    return {false, {}, failure == 1 ? "Cancelled" : "Command timed out"};
                }
                return capture(false, work());
            }
            require(calls == 2 && script.find("exit $cleanup_ok") != std::string::npos,
                    "independent cleanup runs after capture");
            if (failure) require(script.find("sleep 4.1;") == 0, "cleanup waits out detached root capture deadline");
            return {failure != 3, {}, failure == 3 ? "denied" : ""};
        };
        const auto result = ReadGpuTiming(target, cancel, runner);
        require(calls == 2, "one capture and one cleanup");
        require(result.cleanupFailed == (failure == 3), "failed cleanup propagated to collector interlock");
        require(result.values.count("gpu_time_ms") == static_cast<unsigned>(failure == 0), "failed capture never yields samples");
    }
    std::atomic<bool> cancel{true};
    int calls = 0;
    auto runner = [&](const std::string &, int) { ++calls; return CommandResult{}; };
    require(ReadGpuTiming(target, cancel, runner).values.at("gpu_time").find("Cancelled") != std::string::npos && !calls,
            "already cancelled capture never creates instance");
    cancel = false;
    GpuProcess bad = target; bad.package = "game;id";
    require(ReadGpuTiming(bad, cancel, runner).values.at("gpu_time").find("Invalid") != std::string::npos && !calls,
            "invalid process cannot inject script");
    for (int phase : {1, 2}) {
        calls = 0; cancel = false;
        const auto result = ReadGpuTiming(target, cancel, [&](const std::string &, int) {
            ++calls;
            if (calls == phase) cancel = true;
            return calls == 1 ? capture(false, work()) : CommandResult{true, {}, {}};
        });
        require(calls == 2 && !result.values.count("gpu_time_ms") && !result.cleanupFailed,
                "cancellation during successful capture or cleanup discards samples after cleaning");
    }
}
}
int main() {
    try { evidence(); malformed(); lifecycle(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "GPU hardware timing tests passed\n";
}
