#include "monitor/model.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <time.h>

namespace vulkanscope::monitor {
EngineIdentity IdentifyEngine(const std::string &maps) {
    EngineIdentity result;
    std::set<std::string> names, evidence;
    std::istringstream lines(maps);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream row(line);
        std::string range, permissions, offset, device, inode, path;
        if (!(row >> range >> permissions >> offset >> device >> inode >> path) ||
            permissions.find('x') == std::string::npos || path.empty() || path.front() != '/') continue;
        const auto library = path.substr(path.rfind('/') + 1);
        const std::pair<const char *, const char *> signatures[] = {
            {"libunity.so", "Unity"}, {"libUE4.so", "Unreal Engine 4"},
            {"libUnreal.so", "Unreal Engine"}, {"libgodot_android.so", "Godot"},
            {"libcocos2dcpp.so", "Cocos2d-x"}, {"libcocos2djs.so", "Cocos2d-JS"},
            {"libcocos.so", "Cocos"}, {"libdmengine.so", "Defold"}
        };
        for (const auto &signature : signatures) {
            if (library != signature.first) continue;
            names.insert(signature.second);
            evidence.insert(path);
        }
    }
    if (!names.empty()) {
        result.name.clear();
        for (const auto &name : names) {
            if (!result.name.empty()) result.name += " + ";
            result.name += name;
        }
        if (names.size() > 1) result.name += " (multiple libraries)";
    }
    for (const auto &path : evidence) {
        if (result.evidence.size() >= 2048) break;
        result.evidence += path.substr(0, 2048 - result.evidence.size()) + "\n";
    }
    return result;
}
const std::vector<Metric> &Metrics() {
    static const std::vector<Metric> value = {{"foreground", "Foreground", true},
                                              {"fps", "Foreground FPS", true},
                                              {"frame_time", "Frame time", true},
                                              {"frame_p95", "P95 frame time", false},
                                              {"fps_source", "FPS source", true},
                                              {"surface", "Surface", false},
                                              {"graphics_api", "Graphics API (root call tracing)", false},
                                              {"gpu_model", "GPU model", true},
                                              {"gpu_time", "GPU execution time (root tracing)", false},
                                              {"app_memory", "Process memory (RSS)", true},
                                              {"gpu_memory", "GPU shared memory (KGSL)", true},
                                              {"gpu_driver", "GPU driver", false},
                                              {"gpu_api", "Device Vulkan API version", false},
                                              {"cpu_freq", "CPU clock", true},
                                              {"cpu_load", "CPU load", false},
                                              {"cpu_power", "CPU power", false},
                                              {"gpu_freq", "GPU clock", false},
                                              {"gpu_load", "GPU load", false},
                                              {"memory", "System RAM", false},
                                              {"battery_power", "Battery power (device)", false},
                                              {"battery_temp", "Battery temperature", true},
                                              {"thermal", "Thermal state", false},
                                              {"display_resolution", "Display resolution", false},
                                              {"display_refresh", "Display refresh", false},
                                              {"engine_name", "Game engine", true},
                                              {"engine_resolution", "Engine render size", true},
                                              {"engine_output", "Engine output size", false},
                                              {"engine_source", "Engine source", false}};
    return value;
}
Config::Config() {
    for (const auto &m : Metrics()) {
        order.emplace_back(m.id);
        enabled[m.id] = m.enabled;
    }
}
bool Config::has(const std::string &id) const {
    auto it = enabled.find(id);
    return it != enabled.end() && it->second;
}
void ApplyMetricPreset(Config &config, MetricPreset preset) {
    for (auto &item : config.enabled)
        item.second = false;
    for (const char *id : {"gpu_model", "fps", "frame_time", "app_memory", "gpu_memory"})
        config.enabled[id] = true;
    if (preset == MetricPreset::Game) {
        for (const char *id : {"foreground", "frame_p95", "fps_source", "engine_resolution",
                               "engine_source", "engine_name", "battery_temp"})
            config.enabled[id] = true;
    } else {
        for (const char *id : {"cpu_freq", "cpu_load", "cpu_power", "gpu_freq", "gpu_load", "memory",
                               "battery_power", "battery_temp", "thermal"})
            config.enabled[id] = true;
    }
}
void Config::normalize() {
    managerPlugins &= 15;
    if (hudStyle != "metal" && hudStyle != "rtss") hudStyle = "metal";
    if (language != "system" && language != "en" && language != "zh-Hans") language = "system";
    if (std::find(SampleIntervals.begin(), SampleIntervals.end(), interval) == SampleIntervals.end())
        interval = 1000;
    textSize = std::clamp(textSize, 10, 24);
    opacity = std::clamp(opacity, 20, 100);
    corner = std::clamp(corner, 0, 3);
    if (!std::isfinite(powerScale) || powerScale <= 0)
        powerScale = 0.000001;
    surface.resize(std::min<std::size_t>(surface.size(), 512));
    powerPath.resize(std::min<std::size_t>(powerPath.size(), 512));
    std::set<std::string> known, seen;
    for (const auto &m : Metrics())
        known.insert(m.id);
    std::vector<std::string> clean;
    for (const auto &id : order)
        if (known.count(id) && seen.insert(id).second)
            clean.push_back(id);
    for (const auto &m : Metrics())
        if (seen.insert(m.id).second)
            clean.emplace_back(m.id);
    order = std::move(clean);
}
bool SaveConfig(const std::string &path, Config c, std::string &error) {
    c.normalize();
    std::ofstream out(path + ".tmp", std::ios::trunc);
    out.imbue(std::locale::classic());
    out << "VulkanScopeHUD 8\n"
        << c.root << ' ' << c.interval << ' ' << c.textSize << ' ' << c.opacity << ' ' << c.corner << ' '
        << std::setprecision(17) << c.powerScale << '\n'
        << std::quoted(c.surface) << '\n'
        << std::quoted(c.powerPath) << '\n'
        << c.hudGraph << ' ' << c.hudExtras << '\n'
        << std::quoted(c.language) << '\n' << c.autoRoot << '\n' << c.managerPlugins << '\n' << c.rootDisplay << '\n' << c.liveUpdates << '\n' << std::quoted(c.hudStyle) << '\n';
    for (const auto &id : c.order)
        out << id << ' ' << c.has(id) << '\n';
    out.close();
    if (!out || std::rename((path + ".tmp").c_str(), path.c_str()) != 0) {
        error = "Unable to save native HUD configuration";
        std::remove((path + ".tmp").c_str());
        return false;
    }
    error.clear();
    return true;
}
Config LoadConfig(const std::string &path) {
    Config c;
    std::ifstream in(path);
    in.imbue(std::locale::classic());
    std::string magic;
    int version;
    if (!(in >> magic >> version) || magic != "VulkanScopeHUD" || (version < 1 || version > 8))
        return c;
    if (!(in >> c.root >> c.interval >> c.textSize >> c.opacity >> c.corner >> c.powerScale >>
          std::quoted(c.surface) >> std::quoted(c.powerPath)))
        return Config{};
    if (version >= 2 && !(in >> c.hudGraph >> c.hudExtras))
        return Config{};
    if (version >= 3 && !(in >> std::quoted(c.language))) return Config{};
    if (version >= 4 && !(in >> c.autoRoot)) return Config{};
    if (version >= 5 && !(in >> c.managerPlugins)) return Config{};
    if (version >= 6 && !(in >> c.rootDisplay)) return Config{};
    if (version >= 7 && !(in >> c.liveUpdates)) return Config{};
    if (version >= 8 && !(in >> std::quoted(c.hudStyle))) return Config{};
    c.order.clear();
    std::string id;
    bool enabled;
    while (in >> id >> enabled) {
        c.order.push_back(id);
        c.enabled[id] = enabled;
    }
    c.normalize();
    return c;
}
std::string Quote(const std::string &value) {
    std::string result = "'";
    for (char ch : value)
        result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
}
bool ValidPowerPath(const std::string &value) {
    return std::regex_match(value, std::regex("/sys/[A-Za-z0-9_./:-]+")) &&
           !std::regex_search(value, std::regex("(^|/)\\.\\.(/|$)"));
}
std::int64_t MonotonicNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
std::int64_t BootMs() {
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
std::optional<double> Number(const std::string &value) {
    std::istringstream in(value);
    in.imbue(std::locale::classic());
    double n;
    std::string extra;
    if (!(in >> n) || (in >> extra) || !std::isfinite(n))
        return {};
    return n;
}
std::string Format(double value, int precision) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
std::string Report(const Config &config, const Values &values) {
    std::string text;
    for (const auto &id : config.order)
        if (config.has(id)) {
            auto m =
                std::find_if(Metrics().begin(), Metrics().end(), [&](const Metric &x) { return id == x.id; });
            if (m == Metrics().end())
                continue;
            auto value = values.find(id);
            if (!text.empty())
                text += '\n';
            text +=
                std::string(m->label) + ": " + (value == values.end() ? "N/A (not sampled)" : value->second);
        }
    return text.empty() ? "Performance HUD: no fields selected" : text;
}
std::optional<Ticks> CpuTicks(const std::string &text) {
    std::istringstream in(text);
    std::string name;
    Ticks result{};
    if (!(in >> name) || name != "cpu")
        return {};
    for (auto &n : result) {
        std::int64_t v;
        if (!(in >> v) || v < 0)
            return {};
        n = v;
    }
    return result;
}
std::optional<double> CpuLoad(const Ticks &before, const Ticks &after) {
    double total = 0, idle = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (after[i] < before[i])
            return {};
        double d = static_cast<double>(after[i] - before[i]);
        total += d;
        if (i == 3 || i == 4)
            idle += d;
    }
    if (total <= 0)
        return {};
    return (total - idle) * 100 / total;
}
std::optional<double> GpuLoad(const std::string &text) {
    std::istringstream in(text);
    double busy, total;
    std::string extra;
    if (!(in >> busy >> total) || (in >> extra) || !std::isfinite(busy) || !std::isfinite(total) ||
        total <= 0 || busy < 0 || busy > total)
        return {};
    return busy * 100 / total;
}
std::optional<Foreground> ParseForeground(const std::string &text, bool activities) {
    std::regex component("\\bu([0-9]+)\\s+([A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+)/");
    std::istringstream lines(text);
    std::string line;
    int best = 0;
    std::optional<Foreground> result;
    while (std::getline(lines, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
            continue;
        const auto delimiter = line.find_first_of(":=", start);
        if (delimiter == std::string::npos)
            continue;
        auto marker = line.substr(start, delimiter - start);
        marker.erase(marker.find_last_not_of(" \t\r") + 1);
        int priority = 0;
        if (activities) {
            /** @brief 全局 ResumedActivity 才是聚焦活动；每个任务都可能有 topResumedActivity。 */
            if (marker == "ResumedActivity")
                priority = 3;
            else if (marker == "topResumedActivity")
                priority = 2;
            else if (marker == "mResumedActivity")
                priority = 1;
        } else {
            if (marker == "mCurrentFocus")
                priority = 2;
            else if (marker == "mFocusedApp")
                priority = 1;
        }
        if (!priority || priority < best)
            continue;
        auto content = line.substr(delimiter + 1);
        content.erase(0, content.find_first_not_of(" \t\r"));
        content.erase(content.find_last_not_of(" \t\r") + 1);
        /** @brief 权威焦点为空或同等级身份冲突时，不用较低优先级的旧任务替代。 */
        if (content == "null") {
            best = priority;
            result.reset();
            continue;
        }
        std::smatch m;
        if (!std::regex_search(content, m, component))
            continue;
        try {
            int user = std::stoi(m[1]);
            if (user > 21474)
                continue;
            if (priority > best) {
                result = Foreground{m[2], user};
                best = priority;
            } else if (result && (result->package != m[2] || result->user != user)) {
                result.reset();
            }
        } catch (...) {
        }
    }
    return result;
}
/**
 * @brief Android 15/16 的 --list 输出调试对象；--latency 仍要求原始 name#id。
 * @note 仅剥离带有效序号和已知属性尾缀的包装，保留名字内部的空格、括号和属性样式文本。
 */
static std::optional<std::string> SurfaceName(const std::string &line) {
    const std::string prefix = "RequestedLayerState{";
    if (line.compare(0, prefix.size(), prefix) != 0) {
        if (line.size() > MaxSurfaceNameBytes || line.find('\0') != std::string::npos)
            return {};
        return line;
    }
    if (line.size() > MaxSurfaceNameBytes + 1024 || line.back() != '}' ||
        line.find('\0') != std::string::npos)
        return {};
    const auto hash = line.rfind('#');
    if (hash == std::string::npos || hash <= prefix.size())
        return {};
    const auto end = line.size() - 1;
    std::size_t position = hash + 1;
    auto digits = [&]() {
        const auto start = position;
        while (position < end && line[position] >= '0' && line[position] <= '9')
            ++position;
        return position > start;
    };
    if (!digits())
        return {};
    const auto nameEnd = position;
    auto consume = [&](const std::string &token) {
        if (line.compare(position, token.size(), token) != 0)
            return false;
        position += token.size();
        return true;
    };
    auto numberAttribute = [&](const std::string &token, bool signedValue = false) {
        if (!consume(token))
            return true;
        if (signedValue && position < end && line[position] == '-')
            ++position;
        return digits();
    };
    if (!numberAttribute(" parentId=") || !numberAttribute(" relativeParentId="))
        return {};
    if (consume(" mirrorId={")) {
        do {
            if (!digits() || !consume(","))
                return {};
        } while (position < end && line[position] != '}');
        if (!consume("}"))
            return {};
    }
    consume(" !handle");
    if (!numberAttribute(" z=", true) || !numberAttribute(" layerStack=") || position != end ||
        nameEnd - prefix.size() > MaxSurfaceNameBytes)
        return {};
    return line.substr(prefix.size(), nameEnd - prefix.size());
}
std::vector<std::string> Surfaces(const std::string &text, const std::string &package,
                                  const std::string &overrideName) {
    const auto overrideSurface = SurfaceName(overrideName);
    if (package.empty() || !overrideSurface)
        return {};
    auto contains = [](const std::string &s, const std::string &p) {
        auto token = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
        };
        std::size_t pos = 0;
        while ((pos = s.find(p, pos)) != std::string::npos) {
            if ((pos == 0 || !token(s[pos - 1])) && (pos + p.size() == s.size() || !token(s[pos + p.size()])))
                return true;
            ++pos;
        }
        return false;
    };
    auto score = [](const std::string &s) {
        return (s.find("SurfaceView") != std::string::npos ? 100 : 0) +
               (s.find("BLAST") != std::string::npos ? 20 : 0) -
               (s.find("Splash") != std::string::npos || s.find("Starting") != std::string::npos ? 100 : 0);
    };
    std::vector<std::string> result;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto normalized = SurfaceName(line);
        if (!normalized)
            continue;
        line = std::move(*normalized);
        if (!contains(line, package) || contains(line, "dev.snowflake.vulkan") ||
            (!overrideSurface->empty() && line != *overrideSurface) ||
            std::find(result.begin(), result.end(), line) != result.end())
            continue;
        const auto position = std::find_if(result.begin(), result.end(),
                                           [&](const auto &candidate) { return score(line) > score(candidate); });
        if (position != result.end() || result.size() < MaxSurfaceCandidates) {
            result.insert(position, std::move(line));
            if (result.size() > MaxSurfaceCandidates)
                result.pop_back();
        }
    }
    return result;
}
Trace ParseLatency(const std::string &text, std::int64_t now) {
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    auto period = Number(line);
    if (!period || *period <= 0 || *period > 1e9)
        return {};
    std::set<std::int64_t> stamps;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::int64_t a, b, c;
        std::string extra;
        if (row >> a >> b >> c && !(row >> extra) && b > 0 && b != INT64_MAX && b <= now)
            stamps.insert(b);
    }
    return {!stamps.empty(), {stamps.begin(), stamps.end()}};
}
void FrameStats::reset() {
    key_.clear();
    sample_ = present_ = 0;
}
FrameSample FrameStats::update(const std::string &key, const Trace &trace, std::int64_t now) {
    FrameSample result;
    if (!trace.supported || trace.presents.empty()) {
        reset();
        result.status = "Surface latency unavailable";
        return result;
    }
    const auto &times = trace.presents;
    auto latest = times.back();
    if (key_ != key || !sample_ || now <= sample_ || now - sample_ > 10000000000LL || latest < present_) {
        key_ = key;
        sample_ = now;
        present_ = latest;
        result.status = "Collecting presentation timestamps";
        return result;
    }
    result.resetHistory = false;
    auto elapsed = now - sample_, baseline = present_;
    sample_ = now;
    present_ = std::max(present_, latest);
    auto first = std::upper_bound(times.begin(), times.end(), baseline);
    if (first == times.end()) {
        result.fps = 0;
        return result;
    }
    result.truncated = times.front() > baseline;
    std::vector<double> intervals;
    auto last = result.truncated ? *first : baseline;
    for (auto it = first; it != times.end(); ++it) {
        if (*it > last)
            intervals.push_back((*it - last) / 1e6);
        last = *it;
    }
    if (result.truncated && intervals.empty()) {
        result.status = "Presentation ring changed";
        return result;
    }
    result.fps = result.truncated ? (times.end() - first - 1) * 1e9 / (latest - *first)
                                  : (times.end() - first) * 1e9 / elapsed;
    if (!intervals.empty()) {
        result.mean = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
        result.intervals = intervals;
        std::sort(intervals.begin(), intervals.end());
        result.p95 = intervals[static_cast<std::size_t>(std::ceil(intervals.size() * .95)) - 1];
    }
    return result;
}

/** @brief 遥测协议仅接受扁平对象中的字符串与整数，拒绝重复键和隐式类型转换。 */
std::optional<Telemetry> ParseTelemetry(const std::string &json, const std::string &package, std::int64_t now,
                                        std::string &error) {
    error = "Malformed engine report";
    if (json.size() > 4096) {
        error = "Engine report exceeds 4 KiB";
        return {};
    }
    std::size_t pos = 0;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::int64_t> ints;
    std::set<std::string> keys;
    auto ws = [&] {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t'))
            ++pos;
    };
    auto take = [&](char c) {
        ws();
        if (pos == json.size() || json[pos] != c)
            return false;
        ++pos;
        return true;
    };
    auto str = [&]() -> std::optional<std::string> {
        if (!take('"'))
            return {};
        std::string out;
        while (pos < json.size()) {
            unsigned char c = json[pos++];
            if (c == '"')
                return out;
            if (c < 32 || c > 126)
                return {};
            if (c == '\\') {
                if (pos == json.size())
                    return {};
                c = json[pos++];
                if (c == 'u') {
                    if (pos + 4 > json.size())
                        return {};
                    unsigned n = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = json[pos++];
                        n *= 16;
                        if (h >= '0' && h <= '9')
                            n += h - '0';
                        else if (h >= 'a' && h <= 'f')
                            n += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            n += h - 'A' + 10;
                        else
                            return {};
                    }
                    if (n < 32 || n > 126)
                        return {};
                    c = static_cast<unsigned char>(n);
                } else if (c != '"' && c != '\\' && c != '/')
                    return {};
            }
            out += static_cast<char>(c);
        }
        return {};
    };
    if (!take('{'))
        return {};
    do {
        auto key = str();
        if (!key || !keys.insert(*key).second || !take(':'))
            return {};
        ws();
        if (pos == json.size())
            return {};
        if (json[pos] == '"') {
            auto value = str();
            if (!value)
                return {};
            strings[*key] = *value;
        } else {
            auto begin = pos;
            if (json[pos] == '-')
                ++pos;
            auto digits = pos;
            while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
                ++pos;
            if (digits == pos || (pos - digits > 1 && json[digits] == '0'))
                return {};
            try {
                ints[*key] = std::stoll(json.substr(begin, pos - begin));
            } catch (...) {
                return {};
            }
        }
    } while (take(','));
    if (!take('}'))
        return {};
    ws();
    if (pos != json.size())
        return {};
    auto integer = [&](const char *key, std::int64_t low, std::int64_t high) {
        auto it = ints.find(key);
        return it != ints.end() && it->second >= low && it->second <= high;
    };
    if (!integer("schemaVersion", 1, 1) || strings["packageName"] != package ||
        (strings["engine"] != "Unity" && strings["engine"] != "Unreal") || strings["source"].empty() ||
        strings["source"].size() > 128 || !integer("pid", 1, INT32_MAX) ||
        !integer("renderWidth", 1, 32768) || !integer("renderHeight", 1, 32768))
        return {};
    if (!integer("elapsedRealtimeMs", 0, now) || now - ints["elapsedRealtimeMs"] > 5000) {
        error = "Engine report expired or clock invalid";
        return {};
    }
    bool output = keys.count("outputWidth") || keys.count("outputHeight");
    if (output && (!integer("outputWidth", 1, 32768) || !integer("outputHeight", 1, 32768)))
        return {};
    Telemetry t{strings["engine"],
                strings["source"],
                ints["elapsedRealtimeMs"],
                static_cast<int>(ints["pid"]),
                static_cast<int>(ints["renderWidth"]),
                static_cast<int>(ints["renderHeight"]),
                output ? static_cast<int>(ints["outputWidth"]) : 0,
                output ? static_cast<int>(ints["outputHeight"]) : 0};
    error.clear();
    return t;
}
}
