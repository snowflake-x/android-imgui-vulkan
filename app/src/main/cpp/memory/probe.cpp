#include "memory/scan.h"
#include "memory/inspect.h"
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace vulkanscope::memory {
namespace {
struct File {
    int fd;
    explicit File(int value) : fd(value) {}
    ~File() { if (fd >= 0) close(fd); }
    File(const File &) = delete;
};
std::string readFile(const std::string &path, std::size_t limit = 2u << 20) {
    std::ifstream in(path);
    std::string result, buffer(4096, 0);
    while (in && result.size() < limit) {
        in.read(buffer.data(), std::min(buffer.size(), limit - result.size()));
        result.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (result.size() == limit) throw std::runtime_error("Process data exceeds bound");
    return result;
}
Process process(int pid) {
    Process result;
    result.pid = pid;
    const auto path = "/proc/" + std::to_string(pid) + "/";
    result.start = StartTicks(readFile(path + "stat", 8192));
    std::istringstream status(readFile(path + "status", 65536));
    std::string line;
    while (std::getline(status, line))
        if (line.rfind("Uid:", 0) == 0) { std::istringstream fields(line.substr(4)); fields >> result.uid; }
    auto name = readFile(path + "cmdline", 65536);
    name.resize(name.find('\0') == std::string::npos ? name.size() : name.find('\0'));
    for (unsigned char c : name)
        if (c < 33 || c > 126) return result;
    if (name.size() <= 256) result.name = name;
    return result;
}
}
std::string Probe(const std::string &operation, int pid, std::uint64_t start,
                  const std::string &kind, const std::string &input) {
    if (getuid() != 0) throw std::runtime_error("Root UID 0 required");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto stop = [&] {
        pollfd pipe{STDOUT_FILENO, 0, 0};
        poll(&pipe, 1, 0);
        return (pipe.revents & (POLLERR | POLLHUP)) || std::chrono::steady_clock::now() >= deadline;
    };
    if (operation == "list") {
        DIR *directory = opendir("/proc");
        if (!directory) throw std::runtime_error("Cannot list /proc");
        std::vector<Process> processes;
        while (auto *entry = readdir(directory)) {
            if (stop() || processes.size() >= 2048) break;
            std::string id = entry->d_name;
            if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
            try {
                auto p = process(std::stoi(id));
                if (p.uid % 100000 >= 10000 && p.uid % 100000 < 99000 && p.start && !p.name.empty())
                    processes.push_back(std::move(p));
            } catch (const std::exception &) {}
        }
        closedir(directory);
        std::sort(processes.begin(), processes.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        std::ostringstream out;
        for (const auto &p : processes) out << "P\t" << p.pid << '\t' << p.start << '\t' << p.uid << '\t' << p.name << '\n';
        out << "App processes: " << processes.size() << (stop() || processes.size() >= 2048 ? " (partial)" : "") << "\nMEMSCAN_DONE\n";
        return out.str();
    }
    if (operation != "scan" && operation != "unity" && operation != "view") throw std::invalid_argument("Unknown probe operation");
    if (pid <= 0 || !start) throw std::invalid_argument("Select a process first");
    const auto target = process(pid);
    if (target.start != start || target.uid % 100000 < 10000 || target.uid % 100000 >= 99000)
        throw std::runtime_error("Target exited, PID reused, or not an application process; refresh process list");
    const auto path = "/proc/" + std::to_string(pid) + "/";
    const auto maps = readFile(path + "maps");
    auto regions = ParseMaps(maps);
    File memory(open((path + "mem").c_str(), O_RDONLY | O_CLOEXEC));
    if (memory.fd < 0) throw std::runtime_error("Cannot read /proc/PID/mem (root/SELinux or process restrictions)");
    if (StartTicks(readFile(path + "stat", 8192)) != start) throw std::runtime_error("Target changed before read");
    Reader read = [&](std::uint64_t address, void *data, std::size_t size) -> std::size_t {
        if (stop()) return 0;
        const auto it = std::upper_bound(regions.begin(), regions.end(), address,
            [](auto value, const auto &region) { return value < region.begin; });
        if (it == regions.begin() || address >= (it - 1)->end) return 0;
        size = std::min<std::uint64_t>(size, (it - 1)->end - address);
        const auto n = pread(memory.fd, data, size, static_cast<off_t>(address));
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    };
    const bool unity = maps.find("libunity.so") != std::string::npos || maps.find("libil2cpp.so") != std::string::npos;
    std::ostringstream out;
    out << "Target: " << target.name << "\nPID: " << pid << " UID: " << target.uid << " start: " << start
        << "\nEngine: " << (unity ? "Unity" : "Unknown") << "\n";
    if (operation == "view") {
        std::size_t a = 0, b = 0;
        const auto address = std::stoull(kind, &a, 16), count = std::stoull(input, &b, 10);
        if (a != kind.size() || b != input.size() || kind.front() == '-' || input.front() == '-')
            throw std::invalid_argument("Invalid address or view length");
        File exe(open((path + "exe").c_str(), O_RDONLY | O_CLOEXEC));
        unsigned char elf[6]{};
        if (exe.fd < 0 || pread(exe.fd, elf, sizeof(elf), 0) != sizeof(elf) ||
            elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
            (elf[4] != 1 && elf[4] != 2) || elf[5] != 1)
            throw std::runtime_error("Cannot determine target pointer width / byte order from ELF");
        out << Inspect(address, count, elf[4] == 2 ? 8 : 4, regions, read, stop, unity);
    } else if (operation == "scan") {
        const auto result = Scan(regions, read, stop, Pattern(kind, input));
        out << "Exact " << kind << " search (little-endian for numbers)\nRead: " << result.bytes
            << " bytes; unreadable pages: " << result.unreadable << "\nMatches: " << result.addresses.size()
            << "\nCoverage: " << (result.partial ? "PARTIAL (time/byte/result limit or unreadable pages)" : "complete readable RAM mappings") << '\n';
        for (auto address : result.addresses) out << "0x" << std::hex << address << std::dec << '\n';
    } else {
        if (!unity) throw std::runtime_error("Unity libraries were not found in the selected process");
        if (maps.find("libil2cpp.so") == std::string::npos)
            throw std::runtime_error("Unity detected, but no IL2CPP library; Mono class enumeration is not supported");
        /** @brief Prioritize metadata mappings, then anonymous RAM, while preserving bounded coverage. */
        auto candidates = regions;
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
            auto rank = [](const auto &r) { return r.path.find("global-metadata") != std::string::npos ? 0 :
                (r.path.empty() || r.path[0] == '[') ? 1 : 2; };
            return rank(a) < rank(b);
        });
        ScanResult found;
        bool dumped = false;
        std::string reason = "No supported metadata header found";
        unsigned examined = 0;
        for (const auto &region : candidates) {
            for (auto cursor = region.begin; cursor < region.end && !dumped;) {
                if (stop() || found.bytes >= (512ULL << 20) || examined >= 32) { found.partial = true; break; }
                const auto end = std::min<std::uint64_t>(region.end, cursor + (8ULL << 20));
                auto batch = Scan({{cursor, end, region.path}}, read, stop, {0xaf, 0x1b, 0xb1, 0xfa},
                    (512ULL << 20) - found.bytes, 32 - examined);
                found.bytes += batch.bytes; found.unreadable += batch.unreadable;
                found.partial = found.partial || batch.partial;
                for (auto address : batch.addresses) {
                    ++examined;
                    auto metadata = ReadMetadata(address, read, stop);
                    if (!metadata.valid) { reason = metadata.error; continue; }
                    out << "Source: live IL2CPP metadata at 0x" << std::hex << address << std::dec
                        << "\nMetadata version: " << metadata.version << " type stride: " << metadata.stride
                        << "\nTypes: " << metadata.total << "; exported: " << metadata.emitted
                        << "\nExport: " << (metadata.partial ? "PARTIAL (output limit)" : "complete validated type table")
                        << "\nThese are metadata-defined types, not a list of instantiated objects.\n\n" << metadata.report;
                    dumped = true; break;
                }
                if (end == region.end) break;
                cursor = end - 3;
            }
            if (dumped || stop() || found.bytes >= (512ULL << 20) || examined >= 32) break;
        }
        if (stop() || found.bytes >= (512ULL << 20) || examined >= 32) found.partial = true;
        if (!dumped) out << "Dump unavailable: " << reason
            << "\nScanned bytes: " << found.bytes << "; unreadable pages: " << found.unreadable
            << "\nCoverage: " << (found.partial ? "PARTIAL" : "complete readable RAM mappings")
            << "\nEncrypted/modified metadata and unsupported layouts cannot be decoded by this backend.\n";
    }
    if (StartTicks(readFile(path + "stat", 8192)) != start)
        throw std::runtime_error("Target exited or changed during capture; result discarded");
    out << "\nMEMSCAN_DONE\n";
    return out.str();
}
}
