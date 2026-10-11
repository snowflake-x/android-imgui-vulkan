#include "monitor/collector.h"
#include "monitor/graphics_trace.h"
#include "monitor/gpu_memory.h"
#include "monitor/gpu_timing.h"
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fnmatch.h>
#include <fstream>
#include <regex>
#include <sstream>
namespace vulkanscope::monitor {
std::string ReadFile(const std::string &path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    std::string value(limit, '\0');
    in.read(value.data(), static_cast<std::streamsize>(limit));
    value.resize(in.gcount());
    return value;
}
static std::vector<std::string> paths(const std::string &pattern) {
    const auto wildcard = pattern.find('*');
    if (wildcard == std::string::npos)
        return {pattern};
    const auto start = pattern.rfind('/', wildcard), end = pattern.find('/', wildcard);
    const auto parent = pattern.substr(0, start), name = pattern.substr(start + 1, end - start - 1);
    const auto suffix = end == std::string::npos ? "" : pattern.substr(end);
    std::vector<std::string> result;
    DIR *directory = opendir(parent.c_str());
    if (!directory)
        return result;
    while (auto *entry = readdir(directory)) {
        if (entry->d_name[0] == '.' || fnmatch(name.c_str(), entry->d_name, 0) != 0)
            continue;
        auto children = paths(parent + "/" + entry->d_name + suffix);
        result.insert(result.end(), children.begin(), children.end());
    }
    closedir(directory);
    std::sort(result.begin(), result.end());
    return result;
}
CommandResult Collector::root(const std::string &script, int timeout, std::size_t limit) {
    if (runner_)
        return runner_(script, timeout);
    return RunCommand({"su", "-c", script}, cancel_, timeout, limit);
}
std::string Collector::read(const std::string &path) {
    auto value = ReadFile(path);
    if (value.empty() && authorized_ && !cancel_) {
        auto r = root("head -c 32768 " + Quote(path));
        if (r.success)
            value = r.output;
    }
    return value;
}
void Collector::reset() {
    frames_.reset();
    latestFrames_ = {};
    surfaceIdentity_.clear();
    selectedSurface_.clear();
    surfaceCursor_ = 0;
    ticks_.reset();
}
Values Collector::sample(const Config &config) {
    if (!config.root) {
        opted_ = authorized_ = false;
        rootError_ = "Root disabled";
    } else if (!opted_) {
        opted_ = true;
        auto answer = root("id -u", 30000);
        authorized_ = answer.success && std::regex_search(answer.output, std::regex("(^|\n)0(\r?\n|$)"));
        rootError_ = authorized_ ? "" : "Root unavailable or denied; disable/re-enable to retry";
    }
    Values values;
    values["root_status"] = authorized_ ? "granted" : rootError_;
    foreground(config, values);
    if (!cancel_)
        hardware(config, values);
    if (!cancel_)
        engine(config, values);
    return values;
}
void Collector::hardware(const Config &c, Values &v) {
    if (c.has("cpu_load")) {
        auto now = CpuTicks(read("/proc/stat"));
        auto load = now && ticks_ ? CpuLoad(*ticks_, *now) : std::nullopt;
        ticks_ = now;
        v["cpu_load"] = load ? Format(*load) + "% (all CPUs)" : "N/A (counters inaccessible or warming up)";
    } else
        ticks_.reset();
    if (c.has("cpu_freq")) {
        auto files = paths("/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq");
        if (files.empty())
            files = paths("/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq");
        std::map<std::string, double> clocks;
        auto accept = [&](const std::string &path, const std::string &value) {
            auto n = Number(value);
            std::smatch match;
            static const std::regex group("/(policy[0-9]+|cpu[0-9]+)/");
            if (n && *n > 0 && std::regex_search(path, match, group))
                clocks.emplace(match[1], *n / 1000);
        };
        for (const auto &path : files) {
            if (cancel_)
                break;
            auto value = read(path);
            auto n = Number(value);
            if (!n || *n <= 0)
                value = read(path.substr(0, path.rfind('/') + 1) + "cpuinfo_cur_freq");
            accept(path, value);
        }
        if (clocks.empty() && authorized_ && !cancel_) {
            auto discovery = root("for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq "
                                  "/sys/devices/system/cpu/cpufreq/policy*/cpuinfo_cur_freq "
                                  "/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do "
                                  "[ -r \"$p\" ] || continue; printf '%s\\t' \"$p\"; head -c 1024 \"$p\"; "
                                  "printf '\\n'; done; exit 0");
            if (discovery.success) {
                std::istringstream input(discovery.output);
                std::string line;
                while (std::getline(input, line)) {
                    const auto tab = line.find('\t');
                    if (tab != std::string::npos)
                        accept(line.substr(0, tab), line.substr(tab + 1));
                }
            }
        }
        bool policies = std::any_of(clocks.begin(), clocks.end(),
                                    [](const auto &item) { return item.first.rfind("policy", 0) == 0; });
        std::string text;
        for (const auto &[name, mhz] : clocks) {
            if (policies && name.rfind("policy", 0) != 0)
                continue;
            if (!text.empty())
                text += "; ";
            text += name + " " + Format(mhz, 0) + " MHz";
        }
        v["cpu_freq"] = text.empty() ? "N/A (CPU clock sensors inaccessible)" : text;
    }
    if (c.has("cpu_power")) {
        auto n = ValidPowerPath(c.powerPath) ? Number(read(c.powerPath)) : std::nullopt;
        v["cpu_power"] = n && *n >= 0 && std::isfinite(*n * c.powerScale)
                             ? Format(*n * c.powerScale, 3) + " W (CPU sensor)"
                             : "N/A (configure a readable /sys/ CPU power sensor)";
    }
    if (c.has("gpu_freq")) {
        auto n = Number(read("/sys/class/kgsl/kgsl-3d0/gpuclk"));
        if (!n)
            for (const auto &path : paths("/sys/class/devfreq/*/cur_freq")) {
                if (path.find("gpu") == std::string::npos && path.find("mali") == std::string::npos &&
                    path.find("kgsl") == std::string::npos)
                    continue;
                n = Number(read(path));
                if (n)
                    break;
            }
        if (!n && authorized_ && !cancel_) {
            auto discovery =
                root("for p in /sys/class/devfreq/*; do case \"$p\" in *gpu*|*mali*|*kgsl*) "
                     "[ -r \"$p/cur_freq\" ] && head -c 1024 \"$p/cur_freq\" && break;; esac; done; exit 0");
            if (discovery.success)
                n = Number(discovery.output);
        }
        v["gpu_freq"] = n && *n >= 0 ? Format(*n / 1e6, 0) + " MHz" : "N/A (GPU clock inaccessible)";
    }
    if (c.has("gpu_load")) {
        auto n = GpuLoad(read("/sys/class/kgsl/kgsl-3d0/gpubusy"));
        v["gpu_load"] = n ? Format(*n) + "% (Adreno busy/total)" : "N/A (GPU counters unsupported)";
    }
    if (c.has("memory")) {
        std::istringstream in(read("/proc/meminfo"));
        std::string line;
        double total = 0, available = -1;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string key;
            double n;
            if (row >> key >> n) {
                if (key == "MemTotal:")
                    total = n;
                if (key == "MemAvailable:")
                    available = n;
            }
        }
        v["memory"] = total > 0 && available >= 0 && available <= total
                          ? Format((total - available) / 1048576, 2) + " / " + Format(total / 1048576, 2) +
                                " GiB (used/total)"
                          : "N/A (memory counters unavailable)";
    }
}
/** @brief 在 Android mksh 中先压缩焦点记录再有界保留尾部，全局焦点位于完整任务列表之后。 */
static std::string ForegroundQuery(bool activities) {
    const std::string command = activities ? "dumpsys activity activities" : "dumpsys window windows";
    const std::string markers = activities ? "ResumedActivity|topResumedActivity|mResumedActivity"
                                           : "mCurrentFocus|mFocusedApp";
    return "set -o pipefail; " + command + " 2>&1 | sed -n -E " +
           Quote("/^[[:space:]]*(" + markers + ")[[:space:]]*[:=]|[Pp]ermission [Dd]enial|"
                 "[Pp]ermission denied|[Ee]rror|[Tt]imed out|[Tt]imeout|Can.t find service/p") +
           " | tail -c 24576";
}
/** @brief 为缺失焦点保留有界命令诊断，区分调用失败和成功但没有当前活动记录。 */
static std::string ForegroundQueryStatus(const CommandResult &result, bool found) {
    if (result.success && found)
        return "Current activity identified";
    std::string status = result.success ? "No current activity record"
                                       : (result.error.empty() ? "Command failed" : result.error);
    if (!result.output.empty())
        status += ": " + result.output.substr(0, 512);
    return status;
}
void Collector::foreground(const Config &c, Values &v) {
    latestFrames_ = {};
    auto unavailable = [&](const std::string &why) {
        for (const auto *id : {"foreground", "fps", "frame_time", "frame_p95", "fps_source", "surface", "graphics_api",
                               "app_memory", "gpu_time", "gpu_memory"})
            v[id] = "N/A (" + why + ")";
        frames_.reset();
        surfaceIdentity_.clear();
        selectedSurface_.clear();
        surfaceCursor_ = 0;
    };
    if (!authorized_) {
        unavailable(rootError_);
        return;
    }
    auto activity = root(ForegroundQuery(true));
    auto foreground = activity.success ? ParseForeground(activity.output, true) : std::nullopt;
    v["activity_query"] = ForegroundQueryStatus(activity, foreground.has_value());
    if (!foreground && !cancel_) {
        auto window = root(ForegroundQuery(false));
        if (window.success)
            foreground = ParseForeground(window.output, false);
        v["window_query"] = ForegroundQueryStatus(window, foreground.has_value());
    }
    if (!foreground) {
        std::string why = "Foreground unavailable; activity: " + v["activity_query"];
        if (v.count("window_query"))
            why += "; window: " + v["window_query"];
        unavailable(why);
        return;
    }
    if (foreground->package == "dev.snowflake.vulkan") {
        unavailable("Monitor is in foreground");
        return;
    }
    v["foreground"] = foreground->package;
    v["foreground_user"] = std::to_string(foreground->user);
    if (c.has("graphics_api") && !cancel_) {
        v["graphics_api"] = traceCleanupFailed_
            ? "N/A (trace cleanup failed; restart device before retrying)"
            : TraceGraphics(foreground->package, foreground->user, cancel_, runner_);
        traceCleanupFailed_ = v["graphics_api"].find("trace cleanup failed") != std::string::npos;
    }
    if (c.has("app_memory")) {
        v["app_memory"] = "N/A (process RSS unavailable)";
        auto pids = root("pidof " + Quote(foreground->package));
        std::istringstream ids(pids.success ? pids.output : "");
        int pid = 0;
        for (int attempt = 0; attempt < 16 && ids >> pid && !cancel_; ++attempt) {
            if (pid <= 0) continue;
            auto status = root("cat /proc/" + std::to_string(pid) + "/status");
            if (!status.success) continue;
            std::istringstream lines(status.output);
            std::string line;
            long long uid = -1;
            double rss = -1;
            while (std::getline(lines, line)) {
                if (line.rfind("Uid:", 0) == 0) { std::istringstream field(line.substr(4)); field >> uid; }
                if (line.rfind("VmRSS:", 0) == 0) {
                    std::istringstream field(line.substr(6));
                    double amount; std::string unit;
                    if (field >> amount >> unit && unit == "kB") rss = amount;
                }
            }
            if (uid >= 0 && uid / 100000 == foreground->user && std::isfinite(rss) && rss >= 0) {
                v["app_memory_mb"] = Format(rss * 1024 / 1000000, 2);
                v["app_memory"] = v["app_memory_mb"] + " MB RSS (main process)";
                break;
            }
        }
    }
    if (!cancel_)
        gpuTelemetry(c, *foreground, v);
    if (!c.has("fps") && !c.has("frame_time") && !c.has("frame_p95") && !c.has("fps_source") &&
        !c.has("surface")) {
        frames_.reset();
        return;
    }
    presentation(c, *foreground, v);
}
/** @brief GPU 统计与 SurfaceFlinger 独立，读取前后验证同一前台主进程的身份。 */
void Collector::gpuTelemetry(const Config &c, const Foreground &foreground, Values &v) {
    if (!c.has("gpu_time") && !c.has("gpu_memory"))
        return;
    std::string error;
    const auto process = FindGpuProcess(foreground.package, foreground.user, cancel_, error, runner_);
    if (!process) {
        for (const auto *id : {"gpu_time", "gpu_memory"})
            if (c.has(id))
                v[id] = "N/A (" + error + ")";
        return;
    }
    v["gpu_identity"] = process->identity();
    if (c.has("gpu_memory") && !cancel_) {
        auto memory = ReadGpuMemory(*process, cancel_, runner_);
        v.insert(memory.begin(), memory.end());
    }
    if (c.has("gpu_time") && !cancel_) {
        if (gpuTimingCleanupFailed_) {
            v["gpu_time"] = "N/A (GPU trace cleanup failed; restart device before retrying)";
        } else {
            auto timing = ReadGpuTiming(*process, cancel_, runner_);
            gpuTimingCleanupFailed_ = timing.cleanupFailed;
            v.insert(timing.values.begin(), timing.values.end());
        }
    }
}
void Collector::presentation(const Config &c, const Foreground &foreground, Values &v) {
    const auto identity = std::to_string(foreground.user) + ":" + foreground.package + ":" + c.surface;
    if (identity != surfaceIdentity_) {
        frames_.reset();
        surfaceIdentity_ = identity;
        selectedSurface_.clear();
        surfaceCursor_ = 0;
    }
    /** @brief 先过滤目标包再限制输出，避免系统其余图层挤掉列表尾部的游戏图层。 */
    auto list = root("set -o pipefail; dumpsys SurfaceFlinger --list | { grep -F -e " +
                        Quote(foreground.package) +
                        " -e 'Permission Denial' -e 'Permission denied' -e 'Error' "
                        "-e \"Can't find service\"; query_status=$?; [ \"$query_status\" -le 1 ]; }",
                     2000, 131072);
    auto candidates =
        list.success ? Surfaces(list.output, foreground.package, c.surface) : std::vector<std::string>{};
    v["surface_list_query"] = list.success ? "ok" : list.error;
    if (list.truncated)
        v["surface_list_query"] += " (output truncated)";
    v["surface_list_output"] = list.output.substr(0, 16384);
    v["surface_candidates"] = std::to_string(candidates.size());
    std::vector<std::string> probes;
    const auto cached = std::find(candidates.begin(), candidates.end(), selectedSurface_);
    if (cached != candidates.end())
        probes.push_back(*cached);
    else
        selectedSurface_.clear();
    /** @brief 固定每轮工作上限并轮转未命中的候选，避免旧容器层永久遮挡后面的缓冲层。 */
    for (std::size_t checked = 0; checked < candidates.size() && probes.size() < 8; ++checked) {
        const auto &candidate = candidates[(surfaceCursor_ + checked) % candidates.size()];
        if (candidate != selectedSurface_)
            probes.push_back(candidate);
    }
    Trace best;
    std::string name;
    std::int64_t sampled = 0;
    bool active = false;
    std::string failure = !list.success ? "Surface list query failed: " + list.error
                           : candidates.empty() ? (c.surface.empty() ? "No matching game layers"
                                                     : "Configured Surface not found for this app")
                                                : "No valid presentation timestamps";
    std::string details;
    const auto deadline = MonotonicNs() + 2500000000LL;
    std::size_t queried = 0;
    for (const auto &candidate : probes) {
        if (cancel_ || MonotonicNs() >= deadline)
            break;
        const auto remaining = std::max<std::int64_t>(1, (deadline - MonotonicNs()) / 1000000);
        auto latency = root("dumpsys SurfaceFlinger --latency " + Quote(candidate),
                            static_cast<int>(std::min<std::int64_t>(2000, remaining)));
        ++queried;
        const auto position = std::find(candidates.begin(), candidates.end(), candidate);
        if (candidate != selectedSurface_)
            surfaceCursor_ = (static_cast<std::size_t>(position - candidates.begin()) + 1) % candidates.size();
        auto now = MonotonicNs();
        auto trace = latency.success ? ParseLatency(latency.output, now) : Trace{};
        const auto reason = !latency.success ? latency.error
                            : trace.supported ? "actual-present timestamps available"
                            : latency.output.empty() ? "empty latency output"
                            : "no valid actual-present timestamps (header-only, zero or pending ring)";
        details += candidate + "\n  " + reason + "\n  output: " + latency.output.substr(0, 512) + "\n";
        if (!latency.success)
            failure = "Surface latency query failed: " + latency.error;
        bool fresh = trace.supported && now - trace.presents.back() <= 1000000000LL;
        if (trace.supported && (!best.supported || (!active && fresh))) {
            best = std::move(trace);
            name = candidate;
            sampled = now;
            active = fresh;
        }
        if (active || cancel_)
            break;
    }
    v["surface_probe_count"] = std::to_string(queried);
    v["surface_queries"] = std::move(details);
    /** @brief 无效或过期缓存不得在每轮优先消耗查询预算，否则新缓冲层可能一直超时。 */
    selectedSurface_ = active ? name : std::string{};
    auto sample =
        frames_.update(v["foreground_user"] + ":" + foreground.package + ":" + name, best, sampled);
    if (!best.supported)
        sample.status = failure;
    latestFrames_ = sample;
    const auto na = "N/A (" +
                    (sample.status.empty() ? std::string("no new presentation intervals") : sample.status) +
                    ")";
    v["fps"] = sample.fps ? Format(*sample.fps) : na;
    v["frame_time"] = sample.mean ? Format(*sample.mean, 2) + " ms" : na;
    v["frame_p95"] = sample.p95 ? Format(*sample.p95, 2) + " ms" : na;
    v["surface"] = name.empty() ? "N/A (no matching active layer)" : name;
    v["fps_source"] = best.supported
                          ? (sample.truncated ? "SurfaceFlinger actual-present (retained ring only)"
                                              : "SurfaceFlinger actual-present")
                          : "N/A (" + failure + ")";
}
void Collector::engine(const Config &c, Values &v) {
    if (!c.has("engine_resolution") && !c.has("engine_output") && !c.has("engine_source"))
        return;
    auto unavailable = [&](const std::string &why) {
        for (const auto *id : {"engine_resolution", "engine_output", "engine_source"})
            v[id] = "N/A (" + why + ")";
    };
    if (!authorized_) {
        unavailable(rootError_);
        return;
    }
    if (v.find("foreground_user") == v.end()) {
        unavailable("Foreground package unavailable");
        return;
    }
    const auto package = v["foreground"], user = v["foreground_user"];
    const auto internal = "/data/user/" + user + "/" + package + "/files/vulkanscope-render.json";
    const auto external =
        "/storage/emulated/" + user + "/Android/data/" + package + "/files/vulkanscope-render.json";
    auto result = root("if [ -f " + Quote(internal) + " ]; then head -c 4097 " + Quote(internal) +
                           "; elif [ -f " + Quote(external) + " ]; then head -c 4097 " + Quote(external) +
                           "; else printf 'NO_REPORT'; fi",
                       1500);
    if (!result.success) {
        unavailable(result.error);
        return;
    }
    if (result.output == "NO_REPORT") {
        unavailable("No compatible engine resolution reader");
        auto probe =
            root("for p in $(pidof " + Quote(package) +
                 "); do "
                 "uid=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/$p/status); "
                 "case \"$uid\" in ''|*[!0-9]*) continue;; esac; [ $((uid / 100000)) -eq " +
                 user +
                 " ] || continue; "
                 "printf 'pid=%s user=%s\\n' \"$p\" " +
                 user +
                 "; head -c 1048576 /proc/$p/maps | grep -Eo '/lib(unity|UE4|Unreal)\\.so([[:space:]]|$)' | "
                 "head -n 1; break; done; "
                 "dumpsys package " +
                 Quote(package) +
                 " | sed -n 's/^[[:space:]]*versionCode=\\([0-9]*\\).*/versionCode=\\1/p' | head -n 1");
        if (probe.success) {
            std::string engine = probe.output.find("/libunity.so") != std::string::npos ? "Unity"
                                 : probe.output.find("/libUE4.so") != std::string::npos ||
                                         probe.output.find("/libUnreal.so") != std::string::npos
                                     ? "Unreal"
                                     : "Unknown";
            v["engine_source"] = package + " / " + engine + " / " + probe.output;
        }
        return;
    }
    std::string error;
    auto report = ParseTelemetry(result.output, package, BootMs(), error);
    if (!report) {
        unavailable(error);
        return;
    }
    auto pid = std::to_string(report->pid);
    auto process = root("head -c 256 /proc/" + pid + "/cmdline; printf '\\n'; sed -n '/^Uid:/p' /proc/" +
                            pid + "/status",
                        1500);
    auto name = process.output.substr(0, process.output.find_first_of(std::string("\0\n", 2)));
    std::smatch uid;
    std::regex uidPattern("Uid:[ \\t]+([0-9]+)");
    bool validUser = false;
    if (std::regex_search(process.output, uid, uidPattern))
        try {
            validUser = std::stoll(uid[1]) / 100000 == std::stoi(user);
        } catch (...) {
        }
    if (!process.success || !validUser || (name != package && name.rfind(package + ":", 0) != 0) ||
        BootMs() - report->stamp > 5000) {
        unavailable("Engine process mismatch or expired report");
        return;
    }
    v["engine_resolution"] = std::to_string(report->width) + " x " + std::to_string(report->height) + " px";
    v["engine_output"] = report->outputWidth ? std::to_string(report->outputWidth) + " x " +
                                                   std::to_string(report->outputHeight) + " px"
                                             : "N/A (not reported by engine)";
    v["engine_source"] = report->engine + " / " + report->source + " (engine-reported)";
}
}
