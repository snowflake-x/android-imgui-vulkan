#include "monitor/gpu_memory.h"
#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

namespace vulkanscope::monitor {
namespace {
constexpr std::size_t OutputLimit = 262144;
constexpr std::size_t MemoryLimit = 196608;

bool validPackage(const std::string &value) {
    return !value.empty() && value.size() <= 255 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") == std::string::npos;
}

std::optional<std::uint64_t> integer(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    value.remove_prefix(first);
    value = value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
    if (value.size() > 20) return {};
    std::uint64_t result = 0;
    for (char c : value) {
        if (c < '0' || c > '9' || result > (std::numeric_limits<std::uint64_t>::max() - (c - '0')) / 10)
            return {};
        result = result * 10 + (c - '0');
    }
    return result;
}

bool add(std::uint64_t &total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}

CommandResult run(const std::string &script, const std::atomic<bool> &cancel,
                  const GpuCommandRunner &runner) {
    if (cancel) return {false, {}, "cancelled"};
    auto result = runner ? runner(script, 3000)
                         : RunCommand({"su", "-c", "timeout -s TERM 2 sh -c " + Quote(script)}, cancel, 3000, OutputLimit);
    if (cancel) return {false, {}, "cancelled"};
    if (result.truncated || result.output.size() > OutputLimit)
        return {false, {}, "GPU sample output truncated", true};
    return result;
}

std::string status(const CommandResult &result) {
    return result.error.empty() ? "GPU process changed, inaccessible, or command failed" : result.error.substr(0, 256);
}

Values unavailable(const std::string &reason, const GpuProcess *process = nullptr) {
    Values result{{"gpu_memory", "N/A (" + reason + ")"}};
    if (process) result["gpu_identity"] = process->identity();
    return result;
}

Values measured(std::uint64_t bytes, const std::string &path, const GpuProcess &process) {
    const auto mb = Format(static_cast<double>(bytes) / 1000000.0, 2);
    return {{"gpu_memory", mb + " MB (KGSL allocations + proportional imports; main process)"},
            {"gpu_memory_mb", mb}, {"gpu_identity", process.identity()},
            {"gpu_memory_source", path + "; driver allocations + proportional DMA-BUF imports, shared RAM; bytes / 1000000"}};
}

std::vector<std::string> sysfsPaths(const GpuProcess &process) {
    const auto pid = std::to_string(process.pid);
    return {"/sys/class/kgsl/kgsl/proc/" + pid,
            "/sys/class/kgsl/kgsl-3d0/proc/" + pid,
            "/sys/devices/virtual/kgsl/kgsl/proc/" + pid};
}

std::vector<std::string> debugfsPaths(const GpuProcess &process) {
    const auto pid = std::to_string(process.pid);
    return {"/sys/kernel/debug/kgsl/proc/" + pid + "/mem", "/d/kgsl/proc/" + pid + "/mem"};
}

/** @brief 复现 KGSL kernel + imported_mem 的计量口径；CPU 映射和虚拟地址预留均不重复累计。 */
std::optional<std::uint64_t> memoryTable(const std::string &text) {
    if (text.size() > MemoryLimit) return {};
    std::istringstream lines(text);
    std::string line, token;
    if (!std::getline(lines, line)) return {};
    std::istringstream header(line);
    std::vector<std::string> names;
    while (header >> token) names.push_back(token);
    std::map<std::string, std::size_t> columns;
    for (const auto *name : {"gpuaddr", "useraddr", "size", "id", "flags", "type", "eglsrf", "eglimg"}) {
        const auto at = std::find(names.begin(), names.end(), name);
        if (at == names.end() || std::count(names.begin(), names.end(), name) != 1) return {};
        columns[name] = static_cast<std::size_t>(at - names.begin());
    }
    std::set<std::uint64_t> seen;
    std::uint64_t total = 0;
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        std::istringstream row(line);
        std::vector<std::string> fields;
        while (row >> token) fields.push_back(token);
        if (fields.size() < names.size()) return {};
        auto size = integer(fields[columns["size"]]), id = integer(fields[columns["id"]]);
        auto surfaces = integer(fields[columns["eglsrf"]]), images = integer(fields[columns["eglimg"]]);
        if (!size || !id || !surfaces || !images || !seen.insert(*id).second || seen.size() > 8192) return {};
        const auto &type = fields[columns["type"]], &flags = fields[columns["flags"]];
        if (flags.size() < 7 || flags.size() > 10) return {};
        if (type == "gpumem") {
            if (flags.size() == 10 && flags[9] == 'v') continue;
            if (!add(total, *size)) return {};
        } else if (type == "ion") {
            std::uint64_t count = *surfaces;
            if (!add(count, *images) || !add(total, *size / std::max<std::uint64_t>(1, count))) return {};
        } else if (type != "usermem") {
            return {};
        }
    }
    return total;
}
}

std::string GpuProcess::identity() const {
    return package + ":" + std::to_string(uid) + ":" + std::to_string(pid) + ":" + std::to_string(start);
}

std::string GpuIdentityScript(const GpuProcess &process) {
    if (process.pid <= 0 || process.start == 0 || !validPackage(process.package)) return "false";
    const auto path = "/proc/" + std::to_string(process.pid);
    return "[ \"$(head -c 4096 " + path + "/stat 2>/dev/null | sed 's/.*) //' | cut -d' ' -f20)\" = " +
        Quote(std::to_string(process.start)) + " ] && "
        "[ \"$(head -c 16384 " + path + "/status 2>/dev/null | sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p')\" = " +
        Quote(std::to_string(process.uid)) + " ] && "
        "[ \"$(head -c 4096 " + path + "/cmdline 2>/dev/null | tr '\\000' '\\n' | head -n 1)\" = " + Quote(process.package) + " ]";
}

std::optional<GpuProcess> FindGpuProcess(const std::string &package, int user,
                                       const std::atomic<bool> &cancel, std::string &error,
                                       const GpuCommandRunner &runner) {
    error.clear();
    if (!validPackage(package) || user < 0) { error = "invalid GPU target"; return {}; }
    auto result = run("n=0; for p in $(pidof " + Quote(package) + "); do "
        "case \"$p\" in ''|*[!0-9]*) continue;; esac; n=$((n + 1)); [ \"$n\" -le 16 ] || break; "
        "printf 'PID=%s\\n' \"$p\"; "
        "printf 'UID='; head -c 16384 /proc/$p/status 2>/dev/null | sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p'; printf '\\n'; "
        "printf 'START='; head -c 4096 /proc/$p/stat 2>/dev/null | sed 's/.*) //' | cut -d' ' -f20; printf '\\n'; "
        "printf 'CMD='; head -c 4096 /proc/$p/cmdline 2>/dev/null | tr '\\000' '\\n' | head -n 1; printf '\\nEND\\n'; "
        "done; printf 'PROCESS_COMPLETE\\n'", cancel, runner);
    if (!result.success) { error = status(result); return {}; }
    if (result.output.size() > 32768 || result.output.size() < 17 ||
        result.output.compare(result.output.size() - 17, 17, "PROCESS_COMPLETE\n") != 0) {
        error = "incomplete GPU process identity"; return {};
    }
    std::istringstream lines(result.output);
    std::map<std::string, std::string> fields;
    std::string line;
    unsigned candidates = 0;
    bool malformed = false;
    while (std::getline(lines, line)) {
        if (line.empty() || line == "PROCESS_COMPLETE") continue;
        if (line == "END") {
            if (++candidates > 16) break;
            const auto pid = integer(fields["PID"]), uid = integer(fields["UID"]), start = integer(fields["START"]);
            if (!malformed && fields.size() == 4 && pid && *pid > 0 && *pid <= std::numeric_limits<int>::max() &&
                uid && *uid <= std::numeric_limits<std::uint32_t>::max() && *uid / 100000 == static_cast<unsigned>(user) &&
                start && *start > 0 && fields["CMD"] == package)
                return GpuProcess{static_cast<int>(*pid), static_cast<std::uint32_t>(*uid), *start, package};
            fields.clear(); malformed = false;
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos || !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
            malformed = true;
    }
    error = "main GPU process inaccessible or identity/user mismatch";
    return {};
}

Values ReadGpuMemory(const GpuProcess &process, const std::atomic<bool> &cancel,
                     const GpuCommandRunner &runner) {
    const auto identity = GpuIdentityScript(process);
    if (identity == "false") return unavailable("invalid GPU target");
    const auto paths = sysfsPaths(process);
    std::string queries;
    auto record = [&](const std::vector<std::string> &attempted, const CommandResult &result) {
        for (const auto &path : attempted) queries += "Probe: " + path + '\n';
        queries += result.success ? "Command: complete\n" : "Command: " + status(result) + '\n';
        queries += result.output.substr(0, 2048) + '\n';
        if (queries.size() > 4096) queries.resize(4096);
    };
    auto finish = [&](Values values) {
        values["gpu_memory_queries"] = queries;
        return values;
    };
    std::string script = identity + " || exit 1; ";
    for (const auto &path : paths) {
        script += "if [ -d " + Quote(path) + " ]; then ok=1; printf '%s\\n' " + Quote("SOURCE=" + path) + "; ";
        for (const auto *field : {"kernel", "imported_mem"}) {
            const auto file = path + "/" + field;
            script += "if [ -r " + Quote(file) + " ]; then printf '%s' " + Quote(std::string(field) + "=") +
                "; head -c 64 " + Quote(file) + " 2>/dev/null || ok=0; printf '\\n'; else ok=0; fi; ";
        }
        script += "printf 'END\\n'; if [ \"$ok\" -eq 1 ]; then " + identity +
            " || exit 1; printf 'MEMORY_COMPLETE\\n'; exit 0; fi; else printf '%s\\n' " +
            Quote("UNAVAILABLE=" + path) + "; fi; ";
    }
    script += identity + " || exit 1; printf 'MEMORY_COMPLETE\\n'";
    auto scalar = run(script, cancel, runner);
    record(paths, scalar);
    if (!scalar.success) return finish(unavailable(status(scalar), &process));
    if (scalar.output.size() < 16 || scalar.output.compare(scalar.output.size() - 16, 16, "MEMORY_COMPLETE\n") != 0)
        return finish(unavailable("incomplete GPU memory sample", &process));
    std::istringstream lines(scalar.output);
    std::string line, source;
    std::map<std::string, std::string> fields;
    std::optional<std::uint64_t> allocatedOnly;
    bool malformed = false;
    while (std::getline(lines, line)) {
        if (line.empty() || line == "MEMORY_COMPLETE" || line.rfind("UNAVAILABLE=", 0) == 0) continue;
        if (line.rfind("SOURCE=", 0) == 0) {
            source = line.substr(7); fields.clear(); malformed = false;
        } else if (line == "END") {
            if (!malformed && std::find(paths.begin(), paths.end(), source) != paths.end()) {
                const auto kernel = integer(fields["kernel"]), imported = integer(fields["imported_mem"]);
                if (kernel && imported) {
                    auto total = *kernel;
                    if (add(total, *imported)) return finish(measured(total, source, process));
                }
                if (kernel) allocatedOnly = kernel;
            }
        } else {
            const auto equal = line.find('=');
            if (equal == std::string::npos || !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
                malformed = true;
        }
    }
    const auto fallback = debugfsPaths(process);
    script = identity + " || exit 1; ";
    for (const auto &path : fallback)
        script += "if [ -r " + Quote(path) + " ]; then printf '%s\\n' " + Quote("SOURCE=" + path) +
            "; head -c " + std::to_string(MemoryLimit + 1) + " " + Quote(path) + " || exit 1; printf '\\n'; " +
            identity + " || exit 1; printf 'MEMORY_COMPLETE\\n'; exit 0; fi; ";
    script += "printf 'UNAVAILABLE\\n'; " + identity + " || exit 1; printf 'MEMORY_COMPLETE\\n'";
    auto table = run(script, cancel, runner);
    record(fallback, table);
    if (!table.success) return finish(unavailable(status(table), &process));
    if (table.output.size() < 16 || table.output.compare(table.output.size() - 16, 16, "MEMORY_COMPLETE\n") != 0)
        return finish(unavailable("incomplete GPU memory table", &process));
    const auto firstLine = table.output.find('\n');
    if (table.output.rfind("SOURCE=", 0) == 0 && firstLine != std::string::npos) {
        source = table.output.substr(7, firstLine - 7);
        if (std::find(fallback.begin(), fallback.end(), source) != fallback.end()) {
            auto bytes = memoryTable(table.output.substr(firstLine + 1, table.output.size() - firstLine - 17));
            if (bytes) return finish(measured(*bytes, source, process));
        }
        return finish(unavailable("invalid or truncated KGSL memory table", &process));
    }
    if (allocatedOnly)
        return finish(unavailable("KGSL imports unavailable; allocation-only " +
            Format(static_cast<double>(*allocatedOnly) / 1000000.0, 2) + " MB", &process));
    return finish(unavailable("per-process KGSL memory counters unavailable or denied", &process));
}

Values ReadGpuMemory(const std::string &package, int user, const std::atomic<bool> &cancel,
                     const GpuCommandRunner &runner) {
    std::string error;
    const auto process = FindGpuProcess(package, user, cancel, error, runner);
    return process ? ReadGpuMemory(*process, cancel, runner) : unavailable(error);
}
}
