#include "monitor/gpu_timing.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unistd.h>

namespace vulkanscope::monitor {
namespace {
constexpr std::size_t MaxCaptureBytes = 262144;
constexpr std::size_t MaxTraceBytes = 196608;

GpuTimingResult unavailable(const GpuProcess &process, const std::string &why) {
    GpuTimingResult result;
    result.values["gpu_identity"] = process.identity();
    result.values["gpu_time"] = "N/A (" + why + ")";
    result.values["gpu_time_source"] = why;
    return result;
}

std::optional<std::uint64_t> integer(const std::string &text) {
    std::uint64_t value = 0;
    if (text.empty()) return {};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    return value;
}

std::optional<std::string> section(const std::string &text, const std::string &name) {
    const auto beginTag = "VSCOPE_" + name + "_BEGIN\n";
    const auto endTag = "VSCOPE_" + name + "_END\n";
    const auto begin = text.find(beginTag);
    if (begin == std::string::npos || text.find(beginTag, begin + 1) != std::string::npos) return {};
    const auto end = text.find(endTag, begin + beginTag.size());
    if (end == std::string::npos || text.find(endTag, end + 1) != std::string::npos) return {};
    return text.substr(begin + beginTag.size(), end - begin - beginTag.size());
}

bool field(const std::string &format, const std::string &name, unsigned bytes) {
    return std::regex_search(format, std::regex("field:[^;\\n]*[ \\t]" + name +
        ";[ \\t]*offset:[0-9]+;[ \\t]*size:" + std::to_string(bytes) + ";[ \\t]*signed:0;"));
}

std::optional<std::map<std::string, std::uint64_t>> keys(const std::string &text) {
    std::map<std::string, std::uint64_t> result;
    static const std::regex pattern(R"((?:^|[ ,])([a-z_]+)=([^ ,\n]+))");
    for (std::sregex_iterator at(text.begin(), text.end(), pattern), end; at != end; ++at) {
        const auto name = (*at)[1].str();
        if (name != "ctx" && name != "ts" && name != "start" && name != "retire" && name != "active" &&
            name != "gpu_id" && name != "uid" && name != "start_time_ns" && name != "end_time_ns" &&
            name != "total_active_duration_ns") continue;
        const auto value = integer((*at)[2]);
        if (!value || !result.emplace(name, *value).second) return {};
    }
    return result;
}

bool contextMap(const std::string &text, const GpuProcess &process, std::set<std::uint64_t> &ids) {
    std::istringstream lines(text);
    std::string line;
    static const std::regex context(R"(^id: ([0-9]+) type: .* priority: [0-9]+ process: .* \(([0-9]+)\) tid: [0-9]+$)");
    while (std::getline(lines, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, context)) return false;
        const auto id = integer(match[1]), pid = integer(match[2]);
        if (!id || !pid || !*id || *pid != static_cast<unsigned>(process.pid) ||
            !ids.insert(*id).second || ids.size() > 256) return false;
    }
    return !ids.empty();
}

bool intactStats(const std::string &text) {
    std::istringstream lines(text);
    std::string line;
    unsigned cpus = 0, overruns = 0, commits = 0, drops = 0;
    static const std::regex counter(R"(^\s*(overrun|commit overrun|dropped events):\s*([0-9]+)\s*$)");
    while (std::getline(lines, line)) {
        if (line.rfind("VSCOPE_CPU=", 0) == 0) {
            ++cpus;
            if (cpus > 256) return false;
        }
        std::smatch match;
        if (!std::regex_match(line, match, counter)) continue;
        const auto value = integer(match[2]);
        if (!value || *value) return false;
        if (match[1] == "overrun") ++overruns;
        if (match[1] == "commit overrun") ++commits;
        if (match[1] == "dropped events") ++drops;
    }
    return cpus && cpus == overruns && cpus == commits && cpus == drops;
}

/** @brief 只清理本次唯一实例；不修改根实例、全局事件或其他分析工具的采样。 */
std::string cleanupScript(const std::string &instance) {
    return "cleanup_ok=0; for trace_root in /sys/kernel/tracing /sys/kernel/debug/tracing; do "
        "trace_instance=\"$trace_root/instances/" + instance + "\"; "
        "[ ! -d \"$trace_instance\" ] || { "
        "echo 0 > \"$trace_instance/tracing_on\" || cleanup_ok=1; "
        "echo 0 > \"$trace_instance/events/enable\" || cleanup_ok=1; "
        "rmdir \"$trace_instance\" || cleanup_ok=1; }; done; ";
}
}

GpuTimingResult ParseGpuTimingCapture(const GpuProcess &process, const CommandResult &capture) {
    auto fail = [&](const std::string &why) {
        auto result = unavailable(process, why);
        result.values["gpu_time_queries"] = (capture.error + "\n" + capture.output).substr(0, 4096);
        return result;
    };
    if (GpuIdentityScript(process) == "false") return fail("Invalid GPU target process");
    if (!capture.success) return fail(capture.error.empty() ? "GPU tracing unavailable or denied" : capture.error);
    if (capture.truncated || capture.output.size() > MaxCaptureBytes)
        return fail("GPU trace output truncated");
    const std::string complete = "VSCOPE_GPU_COMPLETE\n";
    const auto completed = capture.output.find(complete);
    if (completed == std::string::npos || completed + complete.size() != capture.output.size())
        return fail("GPU trace incomplete or process changed");
    const auto identity = section(capture.output, "IDENTITY");
    const auto format = section(capture.output, "FORMAT");
    const auto trace = section(capture.output, "TRACE");
    const auto stats = section(capture.output, "STATS");
    if (!identity || *identity != process.identity() + "\n" || !format || !trace || !stats)
        return fail("GPU trace identity or sections invalid");
    if (trace->size() > MaxTraceBytes || trace->find("LOST EVENTS") != std::string::npos || !intactStats(*stats))
        return fail("GPU trace incomplete or samples lost");
    std::smatch header;
    static const std::regex counts(R"(entries-in-buffer/entries-written:\s*([0-9]+)/([0-9]+))");
    if (!std::regex_search(*trace, header, counts))
        return fail("GPU trace counters missing or overwritten");
    const auto recorded = integer(header[1]), written = integer(header[2]);
    if (!recorded || !written || recorded != written)
        return fail("GPU trace counters missing or overwritten");
    const bool batch = capture.output.find("VSCOPE_GPU_BACKEND=batch\n") != std::string::npos;
    const bool window = capture.output.find("VSCOPE_GPU_BACKEND=window\n") != std::string::npos;
    if (batch == window) return fail("GPU trace backend invalid");
    std::set<std::uint64_t> contexts;
    if (batch) {
        if (!field(*format, "id", 4) || !field(*format, "start", 8) || !field(*format, "retire", 8) ||
            !field(*format, "active", 8)) return fail("KGSL hardware timing format unsupported");
        const auto before = section(capture.output, "CONTEXT_BEFORE");
        const auto after = section(capture.output, "CONTEXT_AFTER");
        std::set<std::uint64_t> afterIds;
        if (!before || !after || !contextMap(*before, process, contexts) ||
            !contextMap(*after, process, afterIds) || contexts != afterIds)
            return fail("KGSL context ownership changed or inaccessible");
    } else if (!field(*format, "gpu_id", 4) || !field(*format, "uid", 4) ||
               !field(*format, "start_time_ns", 8) || !field(*format, "end_time_ns", 8) ||
               !field(*format, "total_active_duration_ns", 8))
        return fail("GPU work-period format unsupported");
    std::istringstream lines(*trace);
    std::string line;
    std::vector<double> durations, windows;
    std::set<std::string> samples;
    std::optional<std::uint64_t> gpu;
    std::uint64_t lastEnd = 0;
    static const std::regex event(R"(\s[0-9]+\.[0-9]+: (adreno_cmdbatch_retired|gpu_work_period): (.*)$)");
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::smatch match;
        if (!std::regex_search(line, match, event)) return fail("Unrecognized GPU trace event");
        if ((match[1] == "adreno_cmdbatch_retired") != batch) return fail("Unexpected GPU trace event");
        const auto parsed = keys(match[2]);
        if (!parsed) return fail("Invalid GPU event numbers");
        const auto &v = *parsed;
        auto has = [&](const std::string &key) { return v.find(key) != v.end(); };
        if (batch) {
            if (!has("ctx") || !has("ts") || !has("start") || !has("retire") || !has("active") ||
                !contexts.count(v.at("ctx"))) return fail("GPU batch context not owned by target");
            const auto start = v.at("start"), end = v.at("retire"), active = v.at("active");
            if (!start || end < start || active > end - start || end - start > 19200000ULL * 60)
                return fail("GPU batch hardware timestamps invalid");
            const auto key = std::to_string(v.at("ctx")) + ":" + std::to_string(v.at("ts"));
            if (!samples.insert(key).second) return fail("Duplicate GPU batch event");
            durations.push_back(static_cast<double>(active) / 19200.0);
        } else {
            if (!has("gpu_id") || !has("uid") || !has("start_time_ns") || !has("end_time_ns") ||
                !has("total_active_duration_ns") || v.at("uid") != process.uid)
                return fail("GPU work period UID does not match target");
            const auto start = v.at("start_time_ns"), end = v.at("end_time_ns");
            const auto active = v.at("total_active_duration_ns");
            if (end <= start || end - start > 60000000000ULL || active > end - start || start < lastEnd ||
                (gpu && *gpu != v.at("gpu_id"))) return fail("GPU work period range or GPU identity invalid");
            gpu = v.at("gpu_id");
            lastEnd = end;
            durations.push_back(static_cast<double>(active) / 1000000.0);
            windows.push_back(static_cast<double>(end - start) / 1000000.0);
        }
        if (durations.size() > 4096) return fail("GPU trace sample limit exceeded");
    }
    if (durations.size() != *recorded) return fail("GPU trace event count does not match captured data");
    if (durations.empty()) return fail("No target GPU work observed in capture");
    double total = 0, windowTotal = 0;
    for (const auto value : durations) total += value;
    for (const auto value : windows) windowTotal += value;
    const double mean = total / static_cast<double>(durations.size());
    const std::string unit = batch ? "ms/batch" : "ms/window";
    GpuTimingResult result;
    auto &v = result.values;
    v["gpu_identity"] = process.identity();
    v["gpu_time_ms"] = Format(mean, 6);
    v["gpu_time_min_ms"] = Format(*std::min_element(durations.begin(), durations.end()), 6);
    v["gpu_time_max_ms"] = Format(*std::max_element(durations.begin(), durations.end()), 6);
    v["gpu_time_unit"] = unit;
    v["gpu_time"] = Format(mean, 2) + " " + unit;
    v["gpu_time_source"] = batch ? "KGSL hardware active ticks / 19.2 MHz; main process; not per frame"
        : "power/gpu_work_period; UID=" + std::to_string(process.uid) + "; includes all processes sharing UID; not per frame";
    if (window) {
        v["gpu_time_window_ms"] = Format(windowTotal / static_cast<double>(windows.size()), 6);
        v["gpu_time"] += " (window " + Format(windowTotal / static_cast<double>(windows.size()), 2) + " ms; shared UID)";
    }
    v["gpu_time_queries"] = "backend=" + std::string(batch ? "KGSL batch" : "GPU work period") +
        "; samples=" + std::to_string(durations.size()) + "; " + v["gpu_time_source"];
    return result;
}

GpuTimingResult ReadGpuTiming(const GpuProcess &process, const std::atomic<bool> &cancel,
                              const GpuCommandRunner &runner) {
    const auto identity = GpuIdentityScript(process);
    if (identity == "false") return unavailable(process, "Invalid GPU target process");
    if (cancel) return unavailable(process, "Cancelled");
    auto run = [&](const std::string &script, int timeout, bool cleanup = false) {
        if (runner) return runner(script, timeout);
        const std::atomic<bool> active{false};
        return RunCommand({"su", "-c", script}, cleanup ? active : cancel, timeout, MaxCaptureBytes);
    };
    const auto instance = "vscope_gpu_" + std::to_string(getpid()) + "_" + std::to_string(MonotonicNs());
    const auto cleanup = cleanupScript(instance);
    std::string script = "export LC_ALL=C; " + identity + " || { echo GPU_PROCESS_CHANGED; exit 1; }; "
        "format_has() { printf '%s\\n' \"$fmt\" | grep -E -q \"field:.* $1;[[:space:]]*offset:[0-9]+;[[:space:]]*size:$2;[[:space:]]*signed:0;\"; }; "
        "t=''; for trace_root in /sys/kernel/tracing /sys/kernel/debug/tracing; do "
        "[ -d \"$trace_root/instances\" ] && { t=$trace_root; break; }; done; "
        "[ -n \"$t\" ] || { echo GPU_TRACEFS_UNAVAILABLE; exit 1; }; "
        "backend=window; event=power/gpu_work_period; ctx_dir=/sys/kernel/debug/kgsl/kgsl-3d0/ctx; "
        "ctx_ids=''; ctx_before=''; filter=''; "
        "if [ -r \"$t/events/kgsl/adreno_cmdbatch_retired/format\" ] && [ -d \"$ctx_dir\" ]; then "
        "fmt=$(head -c 16384 \"$t/events/kgsl/adreno_cmdbatch_retired/format\"); "
        "if format_has id 4 && format_has start 8 && format_has retire 8 && format_has active 8; then "
        "ctx_count=0; for ctx_file in \"$ctx_dir\"/*; do "
        "ctx_id=${ctx_file##*/}; case \"$ctx_id\" in ''|*[!0-9]*) continue;; esac; "
        "ctx_count=$((ctx_count+1)); [ \"$ctx_count\" -le 256 ] || break; "
        "ctx_line=$(head -n 1 \"$ctx_file\" 2>/dev/null) || continue; "
        "ctx_pid=$(printf '%s' \"$ctx_line\" | sed -n 's/.* process: .* (\\([0-9]*\\)) tid: [0-9]*$/\\1/p'); "
        "[ \"$ctx_pid\" = " + Quote(std::to_string(process.pid)) + " ] || continue; "
        "ctx_ids=\"$ctx_ids $ctx_id\"; ctx_before=\"${ctx_before}${ctx_line}\n"
        "\"; [ -z \"$filter\" ] || filter=\"$filter || \"; filter=\"${filter}id == $ctx_id\"; done; "
        "[ -z \"$filter\" ] || { backend=batch; event=kgsl/adreno_cmdbatch_retired; }; fi; fi; "
        "[ -r \"$t/events/$event/format\" ] || { echo GPU_TIMING_EVENT_UNAVAILABLE; exit 1; }; "
        "fmt=$(head -c 16384 \"$t/events/$event/format\") || exit 1; "
        "[ ${#fmt} -lt 16383 ] || { echo GPU_FORMAT_TRUNCATED; exit 1; }; "
        "if [ \"$backend\" = window ]; then filter=" + Quote("uid == " + std::to_string(process.uid)) + "; "
        "if ! { format_has gpu_id 4 && format_has uid 4 && format_has start_time_ns 8 && "
        "format_has end_time_ns 8 && format_has total_active_duration_ns 8; }; then echo GPU_FORMAT_UNSUPPORTED; exit 1; fi; fi; "
        "mkdir \"$t/instances/" + instance + "\" || exit 1; "
        "trap " + Quote(cleanup) + " EXIT; trap 'exit 1' TERM INT HUP; "
        "i=\"$t/instances/" + instance + "\"; echo 0 > \"$i/tracing_on\" || exit 1; "
        "echo nop > \"$i/current_tracer\" || exit 1; "
        "echo 64 > \"$i/buffer_size_kb\" || exit 1; echo 0 > \"$i/events/enable\" || exit 1; "
        "echo \"$filter\" > \"$i/events/$event/filter\" || exit 1; "
        "echo 1 > \"$i/events/$event/enable\" || exit 1; " + identity + " || exit 1; "
        "echo 1 > \"$i/tracing_on\" || exit 1; "
        "if [ \"$backend\" = batch ]; then sleep 0.2 || exit 1; else sleep 1.1 || exit 1; fi; "
        "echo 0 > \"$i/tracing_on\" || exit 1; " + identity + " || { echo GPU_PROCESS_CHANGED; exit 1; }; "
        "printf 'VSCOPE_GPU_BACKEND=%s\\nVSCOPE_IDENTITY_BEGIN\\n%s\\nVSCOPE_IDENTITY_END\\n' \"$backend\" " + Quote(process.identity()) + "; "
        "printf 'VSCOPE_FORMAT_BEGIN\\n%s\\nVSCOPE_FORMAT_END\\n' \"$fmt\"; "
        "if [ \"$backend\" = batch ]; then "
        "printf 'VSCOPE_CONTEXT_BEFORE_BEGIN\\n%sVSCOPE_CONTEXT_BEFORE_END\\n' \"$ctx_before\"; "
        "echo VSCOPE_CONTEXT_AFTER_BEGIN; for ctx_id in $ctx_ids; do head -n 1 \"$ctx_dir/$ctx_id\" || exit 1; done; "
        "echo VSCOPE_CONTEXT_AFTER_END; fi; "
        "trace_text=$(head -c 196609 \"$i/trace\") || exit 1; "
        "[ ${#trace_text} -lt 196608 ] || { echo GPU_TRACE_TRUNCATED; exit 1; }; "
        "printf 'VSCOPE_TRACE_BEGIN\\n%s\\nVSCOPE_TRACE_END\\n' \"$trace_text\"; "
        "echo VSCOPE_STATS_BEGIN; for stats_file in \"$i\"/per_cpu/cpu*/stats; do "
        "printf 'VSCOPE_CPU=%s\\n' \"$stats_file\"; cat \"$stats_file\" || exit 1; done; "
        "echo VSCOPE_STATS_END; " + identity + " || exit 1; echo VSCOPE_GPU_COMPLETE; ";
    auto capture = run("timeout -s TERM 4 sh -c " + Quote(script), 5000);
    /** @brief su 可能脱离进程组；取消后先等待 root 侧时限，再用不受取消标志影响的命令清理。 */
    auto cleaned = run((capture.success ? std::string{} : "sleep 4.1; ") + cleanup +
                       "exit $cleanup_ok", 6500, true);
    if (!cleaned.success || cleaned.truncated) {
        auto result = unavailable(process, "GPU trace cleanup failed; restart device before retrying");
        result.cleanupFailed = true;
        result.values["gpu_time_queries"] = (cleaned.error + "\n" + cleaned.output).substr(0, 4096);
        return result;
    }
    if (cancel) return unavailable(process, "Cancelled");
    return ParseGpuTimingCapture(process, capture);
}
}
