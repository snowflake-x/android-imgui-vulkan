#include "monitor/graphics_trace.h"
#include "monitor/model.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <elf.h>
#include <fstream>
#include <set>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
namespace vulkanscope::monitor {
namespace {
template<class T> bool object(const std::string &data, std::uint64_t offset, T &out) {
    if (offset > data.size() || sizeof(T) > data.size() - offset) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}
template<class E, class S, class Y, class P>
std::optional<std::uint64_t> offset(const std::string &data, const std::string &name) {
    E elf{};
    if (!object(data, 0, elf) || elf.e_shentsize != sizeof(S) || elf.e_phentsize != sizeof(P) ||
        elf.e_shoff > data.size() || elf.e_phoff > data.size() ||
        elf.e_shnum > (data.size() - elf.e_shoff) / sizeof(S) ||
        elf.e_phnum > (data.size() - elf.e_phoff) / sizeof(P)) return {};
    unsigned symbolsRemaining = 262144;
    for (unsigned i = 0; i < elf.e_shnum; ++i) {
        S section{}, strings{};
        if (!object(data, elf.e_shoff + i * sizeof(S), section) || section.sh_type != SHT_DYNSYM ||
            section.sh_entsize != sizeof(Y) || section.sh_link >= elf.e_shnum ||
            !object(data, elf.e_shoff + section.sh_link * sizeof(S), strings) ||
            section.sh_offset > data.size() || section.sh_size > data.size() - section.sh_offset ||
            strings.sh_offset > data.size() || strings.sh_size > data.size() - strings.sh_offset) continue;
        for (std::uint64_t at = 0; at + sizeof(Y) <= section.sh_size; at += sizeof(Y)) {
            if (symbolsRemaining-- == 0) return {};
            Y symbol{};
            if (!object(data, section.sh_offset + at, symbol) || symbol.st_shndx == SHN_UNDEF ||
                ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_name >= strings.sh_size) continue;
            const auto available = strings.sh_size - symbol.st_name;
            const char *text = data.data() + strings.sh_offset + symbol.st_name;
            if (name.size() + 1 > available || std::memcmp(text, name.c_str(), name.size() + 1)) continue;
            std::uint64_t address = symbol.st_value;
            if (elf.e_machine == EM_ARM) address &= ~std::uint64_t{1};
            for (unsigned j = 0; j < elf.e_phnum; ++j) {
                P segment{};
                if (!object(data, elf.e_phoff + j * sizeof(P), segment) || segment.p_type != PT_LOAD ||
                    !(segment.p_flags & PF_X) || address < segment.p_vaddr ||
                    address - segment.p_vaddr >= segment.p_filesz || segment.p_offset > data.size() ||
                    segment.p_filesz > data.size() - segment.p_offset) continue;
                return segment.p_offset + address - segment.p_vaddr;
            }
        }
    }
    return {};
}
bool safePath(const std::string &path) {
    return !path.empty() && path[0] == '/' && path.find("..") == std::string::npos &&
           path.find_first_not_of("/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@+") == std::string::npos;
}
}
std::optional<std::uint64_t> GraphicsSymbolOffset(const std::string &file, const std::string &symbol) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size < EI_NIDENT || size > 64 * 1024 * 1024) return {};
    std::string data(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(data.data(), size) || std::memcmp(data.data(), ELFMAG, SELFMAG) ||
        data[EI_DATA] != ELFDATA2LSB) return {};
    if (data[EI_CLASS] == ELFCLASS64) return offset<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym, Elf64_Phdr>(data, symbol);
    if (data[EI_CLASS] == ELFCLASS32) return offset<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym, Elf32_Phdr>(data, symbol);
    return {};
}
std::string GraphicsTraceResult(const std::string &trace) {
    bool egl = false, vk = false;
    std::istringstream lines(trace);
    std::string line;
    static const std::regex eventPattern(
        R"(\s[0-9]+\.[0-9]+: (egl_swap|egl_damage|egl_damage_ext|vk_present|vk_submit|vk_submit2|vk_submit2_khr): )");
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::smatch match;
        if (!std::regex_search(line, match, eventPattern)) continue;
        egl |= match[1].str().rfind("egl_", 0) == 0;
        vk |= match[1].str().rfind("vk_", 0) == 0;
    }
    if (egl && vk) return "OpenGL ES + Vulkan (calls observed)";
    if (vk) return "Vulkan (calls observed)";
    if (egl) return "OpenGL ES (EGL swap observed)";
    return "N/A (no graphics calls observed in 200 ms)";
}
std::string TraceGraphics(const std::string &package, int user, const std::atomic<bool> &cancel,
                         const std::function<CommandResult(const std::string &, int)> &runner) {
    if (user < 0 || package.empty() || package.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != std::string::npos)
        return "N/A (invalid target)";
    auto run = [&](const std::string &script, int timeout, bool cleanup = false) {
        if (runner) return runner(script, timeout);
        const std::atomic<bool> active{false};
        return RunCommand({"su", "-c", script}, cleanup ? active : cancel, timeout, 262144);
    };
    auto target = run("for p in $(pidof " + Quote(package) + "); do "
        "u=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/$p/status); "
        "case \"$u\" in ''|*[!0-9]*) continue;; esac; [ $((u / 100000)) -eq " + std::to_string(user) +
        " ] || continue; echo PID=$p; "
        "echo START=$(sed 's/.*) //' /proc/$p/stat | cut -d' ' -f20); "
        "echo UID=$u; head -c 245760 /proc/$p/maps; break; done", 2000);
    std::istringstream maps(target.output);
    std::string line;
    int pid = 0;
    if (!target.success || !std::getline(maps, line) || line.rfind("PID=", 0) != 0)
        return "N/A (target process inaccessible)";
    try { pid = std::stoi(line.substr(4)); } catch (...) { return "N/A (invalid PID)"; }
    if (pid <= 0) return "N/A (invalid PID)";
    std::string start, uid;
    if (!std::getline(maps, start) || start.rfind("START=", 0) != 0 ||
        !std::getline(maps, uid) || uid.rfind("UID=", 0) != 0)
        return "N/A (target identity unavailable)";
    start.erase(0, 6); uid.erase(0, 4);
    if (start.empty() || uid.empty() || start.find_first_not_of("0123456789") != std::string::npos ||
        uid.find_first_not_of("0123456789") != std::string::npos)
        return "N/A (invalid process identity)";
    struct Probe { std::string event, path; std::uint64_t offset; };
    std::vector<Probe> probes;
    std::set<std::string> seen, events;
    while (std::getline(maps, line)) {
        std::istringstream row(line);
        std::string range, permissions, address, device, inode, path;
        if (!(row >> range >> permissions >> address >> device >> inode >> path) ||
            permissions.find('x') == std::string::npos || !safePath(path) || !seen.insert(path).second) continue;
        const auto name = path.substr(path.rfind('/') + 1);
        if (name != "libEGL.so" && name != "libvulkan.so") continue;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || std::to_string(st.st_ino) != inode) continue;
        unsigned majorNumber = 0, minorNumber = 0;
        if (std::sscanf(device.c_str(), "%x:%x", &majorNumber, &minorNumber) != 2 ||
            major(st.st_dev) != majorNumber || minor(st.st_dev) != minorNumber) continue;
        for (const auto &symbol : {std::pair{"eglSwapBuffers", "egl_swap"},
                                  {"eglSwapBuffersWithDamageKHR", "egl_damage"},
                                  {"eglSwapBuffersWithDamageEXT", "egl_damage_ext"},
                                  {"vkQueuePresentKHR", "vk_present"}, {"vkQueueSubmit", "vk_submit"},
                                  {"vkQueueSubmit2", "vk_submit2"}, {"vkQueueSubmit2KHR", "vk_submit2_khr"}}) {
            auto at = GraphicsSymbolOffset(path, symbol.first);
            if (at && events.insert(symbol.second).second) probes.push_back({symbol.second, path, *at});
        }
    }
    if (probes.empty()) return "N/A (no accessible EGL/Vulkan probe symbols)";
    const auto group = "vscope_" + std::to_string(getpid()) + "_" + std::to_string(MonotonicNs());
    const std::string locate = "t=/sys/kernel/tracing; [ -e \"$t/uprobe_events\" ] || t=/sys/kernel/debug/tracing; ";
    std::string cleanup = "cleanup_ok=0; [ ! -d \"$t/instances/" + group + "\" ] || { "
        "echo 0 > \"$t/instances/" + group + "/tracing_on\" || cleanup_ok=1; "
        "echo 0 > \"$t/instances/" + group + "/events/enable\" || cleanup_ok=1; "
        "rmdir \"$t/instances/" + group + "\" || cleanup_ok=1; }; ";
    for (const auto &probe : probes)
        cleanup += "[ ! -d \"$t/events/" + group + "/" + probe.event + "\" ] || { echo '-:" + group +
                   "/" + probe.event + "' >> \"$t/uprobe_events\" || cleanup_ok=1; }; ";
    const auto process = "/proc/" + std::to_string(pid);
    const auto identity = "[ \"$(sed 's/.*) //' " + process + "/stat | cut -d' ' -f20)\" = " + Quote(start) +
        " ] && [ \"$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' " + process + "/status)\" = " + Quote(uid) +
        " ] && [ \"$(tr '\\000' '\\n' < " + process + "/cmdline | head -n 1)\" = " + Quote(package) + " ]";
    std::string script = identity + " || exit 1; " + locate + "[ -w \"$t/uprobe_events\" ] || { echo TRACE_UNAVAILABLE; exit 1; }; "
        "mkdir \"$t/instances/" + group + "\" || exit 1; "
        "trap " + Quote(cleanup) + " EXIT; trap 'exit 1' TERM INT HUP; "
        "i=\"$t/instances/" + group + "\"; echo 0 > \"$i/tracing_on\" || exit 1; echo 16 > \"$i/buffer_size_kb\" || exit 1; "
        "f=''; for task in /proc/" + std::to_string(pid) + "/task/*; do tid=${task##*/}; "
        "case \"$tid\" in ''|*[!0-9]*) continue;; esac; "
        "[ -z \"$f\" ] || f=\"$f || \"; f=\"${f}common_pid == $tid\"; done; [ -n \"$f\" ] || exit 1; ";
    for (const auto &probe : probes) {
        std::ostringstream hex;
        hex << "0x" << std::hex << probe.offset;
        script += "echo " + Quote("p:" + group + "/" + probe.event + " " + probe.path + ":" + hex.str()) +
            " >> \"$t/uprobe_events\" || exit 1; echo \"$f\" > \"$i/events/" + group + "/" + probe.event +
            "/filter\" || exit 1; echo 1 > \"$i/events/" + group + "/" + probe.event + "/enable\" || exit 1; ";
    }
    script += identity + " || exit 1; echo 1 > \"$i/tracing_on\" || exit 1; sleep 0.2 || exit 1; echo 0 > \"$i/tracing_on\" || exit 1; "
              "head -c 196608 \"$i/trace\" || exit 1; " + identity + " || exit 1; echo TRACE_COMPLETE; ";
    /** @brief root shell 内部也设置时限，避免 su 服务脱离应用进程组后继续保留探针。 */
    auto capture = run("timeout -s TERM 2 sh -c " + Quote(script), 3000);
    /** @brief su 可能不转发取消信号；用独立有界 root 命令保证正常停止后的清理尝试。 */
    const auto settle = capture.success ? std::string{} : std::string{"sleep 2.1; "};
    auto cleaned = run(settle + locate + cleanup + "exit $cleanup_ok", 5000, true);
    if (!cleaned.success) return "N/A (trace cleanup failed; restart device before retrying)";
    if (!capture.success || capture.output.find("TRACE_COMPLETE") == std::string::npos)
        return "N/A (uprobes unavailable or denied; see kernel/SELinux support)";
    return GraphicsTraceResult(capture.output);
}
}
