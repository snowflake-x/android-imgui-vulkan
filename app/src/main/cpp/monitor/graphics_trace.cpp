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
namespace {
constexpr std::size_t MaxRootLibraryBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxMapsBytes = 245760;
constexpr const char *TargetComplete = "VSCOPE_GRAPHICS_TARGET_COMPLETE\n";
constexpr const char *ElfComplete = "\nVSCOPE_ELF_END\nVSCOPE_ELF_COMPLETE\n";
constexpr const char *TraceComplete = "TRACE_COMPLETE\n";
/** @brief 根读取和本地读取使用同一有界 ELF 解析器，不从动态库名称推断 API。 */
std::optional<std::uint64_t> symbolOffset(const std::string &data, const std::string &symbol) {
    if (data.size() < EI_NIDENT || std::memcmp(data.data(), ELFMAG, SELFMAG) || data[EI_DATA] != ELFDATA2LSB)
        return {};
    if (data[EI_CLASS] == ELFCLASS64) return offset<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym, Elf64_Phdr>(data, symbol);
    if (data[EI_CLASS] == ELFCLASS32) return offset<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym, Elf32_Phdr>(data, symbol);
    return {};
}
std::optional<std::string> readLibrary(const std::string &file, std::size_t limit) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size < EI_NIDENT || static_cast<std::uint64_t>(size) > limit) return {};
    std::string data(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(data.data(), size)) return {};
    return data;
}
/** @brief maps 路径作为 ERE 字面量匹配，避免库名中的点或加号扩大 inode 验证范围。 */
std::string regexLiteral(const std::string &value) {
    std::string result;
    for (const char c : value) {
        if (std::strchr(".[](){}*+?^$\\|", c)) result += '\\';
        result += c;
    }
    return result;
}
bool endsWith(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
/** @brief 只接受单行协议错误码，二进制库内容不会进入诊断文字。 */
std::string failureStage(const std::string &output) {
    const std::string prefix = "VSCOPE_GRAPHICS_ERROR=";
    const auto at = output.rfind("\n" + prefix);
    const auto begin = at != std::string::npos ? at + 1 : output.rfind(prefix, 0) == 0 ? 0 : std::string::npos;
    if (begin == std::string::npos) return {};
    const auto end = output.find('\n', begin);
    const auto result = output.substr(begin + prefix.size(), end - begin - prefix.size());
    if (result.empty() || result.size() > 96 ||
        result.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_:") != std::string::npos)
        return {};
    return result;
}
std::string commandDetail(const CommandResult &command, bool binary = false) {
    std::string result = command.error;
    if (command.truncated) result += " output truncated";
    const auto stage = failureStage(command.output);
    if (!stage.empty()) result += " stage=" + stage;
    if (!binary && !command.output.empty()) {
        result += "\n" + command.output.substr(command.output.size() > 2048 ? command.output.size() - 2048 : 0);
    }
    return result;
}
std::optional<std::string> rootLibrary(const CommandResult &command, std::uint64_t expectedDevice,
                                       std::uint64_t expectedInode, std::string &reason) {
    auto reject = [&](const char *why) -> std::optional<std::string> { reason = why; return {}; };
    if (!command.success) return reject("root command failed");
    if (command.truncated || command.output.size() > MaxRootLibraryBytes + 4096)
        return reject("root ELF transfer truncated");
    const auto headerEnd = command.output.find('\n');
    if (headerEnd == std::string::npos || headerEnd > 128) return reject("missing root ELF metadata");
    std::istringstream header(command.output.substr(0, headerEnd));
    std::string marker, extra;
    std::uint64_t device = 0, inode = 0, size = 0;
    if (!(header >> marker >> device >> inode >> size) || header >> extra || marker != "VSCOPE_ELF_BEGIN")
        return reject("invalid root ELF metadata");
    if (device != expectedDevice || inode != expectedInode) return reject("root ELF device/inode differs from mapping");
    if (size < EI_NIDENT || size > MaxRootLibraryBytes) return reject("root ELF exceeds size limit");
    const auto bytes = headerEnd + 1;
    const std::string footer = ElfComplete;
    if (command.output.size() != bytes + size + footer.size() ||
        command.output.compare(bytes + size, footer.size(), footer) != 0) return reject("root ELF transfer incomplete or identity changed");
    return command.output.substr(bytes, size);
}
}
bool GraphicsProbeLibrary(const std::string &name) {
    return name == "libEGL.so" || name == "libvulkan.so" || name == "libGLESv2.so" ||
           name == "libEGL_adreno.so" || name == "libGLESv2_adreno.so" ||
           name == "libGLES_mali.so" || name == "libGLESv2_angle.so" ||
           name == "libEGL_angle.so" ||
           (name.rfind("vulkan.", 0) == 0 && endsWith(name, ".so"));
}
std::string GraphicsMappedLibraries(const std::string &text) {
    bool egl = false, vk = false;
    std::istringstream rows(text);
    std::string line;
    while (std::getline(rows, line)) {
        std::istringstream row(line);
        std::string range, permissions, offset, device, inode, path;
        if (!(row >> range >> permissions >> offset >> device >> inode >> path) ||
            permissions.find('x') == std::string::npos || path.empty() || path.front() != '/') continue;
        const auto name = path.substr(path.rfind('/') + 1);
        if (!GraphicsProbeLibrary(name)) continue;
        egl |= name.find("GLES") != std::string::npos || name.find("EGL") != std::string::npos;
        vk |= name.find("vulkan") != std::string::npos;
    }
    return egl && vk ? "OpenGL ES + Vulkan" : vk ? "Vulkan" : egl ? "OpenGL ES" : "";
}
std::optional<std::uint64_t> GraphicsSymbolOffset(const std::string &file, const std::string &symbol) {
    const auto data = readLibrary(file, 64 * 1024 * 1024);
    return data ? symbolOffset(*data, symbol) : std::nullopt;
}
std::string GraphicsTraceResult(const std::string &trace) {
    bool egl = false, vk = false;
    std::istringstream lines(trace);
    std::string line;
    static const std::regex eventPattern(
        R"(\s[0-9]+\.[0-9]+: (egl_swap|egl_damage|egl_damage_ext|gl_draw|gl_elements|vk_present|vk_submit|vk_submit2|vk_submit2_khr)(?:_[0-9]+)?: )");
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::smatch match;
        if (!std::regex_search(line, match, eventPattern)) continue;
        egl |= match[1].str().rfind("egl_", 0) == 0 || match[1].str().rfind("gl_", 0) == 0;
        vk |= match[1].str().rfind("vk_", 0) == 0;
    }
    if (egl && vk) return "OpenGL ES + Vulkan (calls observed)";
    if (vk) return "Vulkan (calls observed)";
    if (egl) return "OpenGL ES (calls observed)";
    return "N/A (no graphics calls observed in 200 ms)";
}
std::string TraceGraphics(const std::string &package, int user, const std::atomic<bool> &cancel,
                         const std::function<CommandResult(const std::string &, int)> &runner,
                         Values *diagnostics) {
    std::string details;
    std::string mappedEvidence;
    auto note = [&](const std::string &text) {
        if (details.size() < 4096) details += text.substr(0, 4096 - details.size());
    };
    auto finish = [&](const std::string &status, const std::string &result, const std::string &detail = "") {
        note(detail);
        const bool fallback = !mappedEvidence.empty() &&
            (status == "no_calls" || status == "trace_unavailable" || status == "probe_failed");
        if (diagnostics) {
            (*diagnostics)["graphics_api_trace_status"] = status;
            (*diagnostics)["graphics_api_status"] = fallback ? "mapped" : status;
            (*diagnostics)["graphics_api_details"] = details;
        }
        return fallback ? mappedEvidence + " (libraries loaded; calls unconfirmed)" : result;
    };
    if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
    if (user < 0 || package.empty() || package.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != std::string::npos)
        return finish("target_unavailable", "N/A (invalid target)");
    auto run = [&](const std::string &script, int timeout, bool cleanup = false, std::size_t limit = 262144) {
        if (runner) return runner(script, timeout);
        const std::atomic<bool> active{false};
        return RunCommand({"su", "-c", script}, cleanup ? active : cancel, timeout, limit);
    };
    const std::string fail = "export LC_ALL=C; fail() { printf '\\nVSCOPE_GRAPHICS_ERROR=%s\\n' \"$1\"; exit 1; }; ";
    auto target = run(fail + "for p in $(pidof " + Quote(package) + "); do "
        "u=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/$p/status); "
        "case \"$u\" in ''|*[!0-9]*) continue;; esac; [ $((u / 100000)) -eq " + std::to_string(user) +
        " ] || continue; echo PID=$p; "
        "echo START=$(sed 's/.*) //' /proc/$p/stat | cut -d' ' -f20); "
        "echo UID=$u; sed -n -E '/^[^[:space:]]+[[:space:]]+[^[:space:]]*x[^[:space:]]*[[:space:]].*\\/(libEGL[^[:space:]]*|libGLES[^[:space:]]*|libvulkan|vulkan\\.[^[:space:]]*)\\.so([[:space:]]+\\(deleted\\))?$/p' /proc/$p/maps || fail TARGET_MAPS_READ; "
        "echo VSCOPE_GRAPHICS_TARGET_COMPLETE; exit 0; done; fail TARGET_INACCESSIBLE", 2000);
    if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
    if (!target.success || target.truncated || !endsWith(target.output, TargetComplete))
        return finish("target_unavailable", "N/A (target process inaccessible)", commandDetail(target));
    const auto targetText = target.output.substr(0, target.output.size() - std::strlen(TargetComplete));
    std::istringstream maps(targetText);
    std::string line;
    int pid = 0;
    if (!std::getline(maps, line) || line.rfind("PID=", 0) != 0)
        return finish("target_unavailable", "N/A (target process inaccessible)", "Malformed target metadata");
    if (line.size() <= 4 || line.substr(4).find_first_not_of("0123456789") != std::string::npos)
        return finish("target_unavailable", "N/A (invalid PID)");
    try { pid = std::stoi(line.substr(4)); } catch (...) {
        return finish("target_unavailable", "N/A (invalid PID)");
    }
    if (pid <= 0) return finish("target_unavailable", "N/A (invalid PID)");
    std::string start, uid;
    if (!std::getline(maps, start) || start.rfind("START=", 0) != 0 ||
        !std::getline(maps, uid) || uid.rfind("UID=", 0) != 0)
        return finish("target_unavailable", "N/A (target identity unavailable)");
    start.erase(0, 6); uid.erase(0, 4);
    if (start.empty() || uid.empty() || start.find_first_not_of("0123456789") != std::string::npos ||
        uid.find_first_not_of("0123456789") != std::string::npos)
        return finish("target_unavailable", "N/A (invalid process identity)");
    try {
        if (std::stoull(uid) / 100000 != static_cast<std::uint64_t>(user))
            return finish("target_unavailable", "N/A (target Android user changed)");
    } catch (...) { return finish("target_unavailable", "N/A (invalid process identity)"); }
    const auto mapsAt = maps.tellg();
    if (mapsAt < 0 || targetText.size() - static_cast<std::size_t>(mapsAt) >= MaxMapsBytes)
        return finish("target_unavailable", "N/A (target mappings truncated)");
    const auto loaded = GraphicsMappedLibraries(targetText.substr(static_cast<std::size_t>(mapsAt)));
    mappedEvidence = loaded;
    if (diagnostics) (*diagnostics)["graphics_api_libraries"] = loaded.empty() ? "unavailable" : loaded;
    const auto process = "/proc/" + std::to_string(pid);
    const auto identity = "[ \"$(sed 's/.*) //' " + process + "/stat | cut -d' ' -f20)\" = " + Quote(start) +
        " ] && [ \"$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' " + process + "/status)\" = " + Quote(uid) +
        " ] && [ \"$(tr '\\000' '\\n' < " + process + "/cmdline | head -n 1)\" = " + Quote(package) + " ]";
    struct Probe { std::string event, path, validation; std::uint64_t offset; };
    std::vector<Probe> probes;
    std::set<std::string> seen;
    unsigned candidates = 0, unreadable = 0;
    while (std::getline(maps, line)) {
        if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
        std::istringstream row(line);
        std::string range, permissions, address, device, inode, path;
        if (!(row >> range >> permissions >> address >> device >> inode >> path) ||
            permissions.find('x') == std::string::npos || !safePath(path) || !seen.insert(path).second) continue;
        const auto name = path.substr(path.rfind('/') + 1);
        if (!GraphicsProbeLibrary(name)) continue;
        if (++candidates > 8) {
            note("Additional library mappings skipped after bounded candidate limit\n");
            break;
        }
        unsigned majorNumber = 0, minorNumber = 0;
        char trailing = 0;
        if (std::sscanf(device.c_str(), "%x:%x%c", &majorNumber, &minorNumber, &trailing) != 2 ||
            inode.empty() || inode.find_first_not_of("0123456789") != std::string::npos) continue;
        std::uint64_t expectedInode = 0;
        try { expectedInode = std::stoull(inode); } catch (...) { continue; }
        const std::uint64_t expectedDevice = makedev(majorNumber, minorNumber);
        const auto targetPath = process + "/root" + path;
        const auto metadata = std::to_string(expectedDevice) + " " + std::to_string(expectedInode);
        const auto mappingPattern = "^[^ ]+ +[^ ]*x[^ ]* +[^ ]+ +" + regexLiteral(device) + " +" + inode +
            " +" + regexLiteral(path) + "$";
        const auto mapping = "grep -E -q " + Quote(mappingPattern) + " " + process + "/maps";
        const auto validation = identity + " && " + mapping + " && [ \"$(stat -L -c '%d %i' " + Quote(targetPath) +
            ")\" = " + Quote(metadata) + " ]";
        std::optional<std::string> data;
        struct stat before{}, after{};
        if (stat(path.c_str(), &before) == 0 && static_cast<std::uint64_t>(before.st_dev) == expectedDevice &&
            static_cast<std::uint64_t>(before.st_ino) == expectedInode) {
            data = readLibrary(path, 64 * 1024 * 1024);
            if (data && (stat(path.c_str(), &after) != 0 || before.st_dev != after.st_dev ||
                         before.st_ino != after.st_ino || before.st_size != after.st_size)) data.reset();
        }
        if (!data) {
            /** @brief root 只传送有界文件字节；解析仍在 C++，探针始终定位到目标进程的挂载命名空间。 */
            const auto script = fail + identity + " || fail TARGET_CHANGED; " + mapping +
                " || fail MAPPING_CHANGED; p=" + Quote(targetPath) + "; "
                "before=$(stat -L -c '%d %i %s' \"$p\") || fail LIBRARY_STAT; "
                "set -- $before; [ \"$#\" -eq 3 ] || fail LIBRARY_METADATA; "
                "[ \"$1 $2\" = " + Quote(metadata) + " ] || fail LIBRARY_IDENTITY; "
                "case \"$3\" in ''|*[!0-9]*) fail LIBRARY_SIZE;; esac; "
                "[ \"$3\" -ge 16 ] && [ \"$3\" -le " + std::to_string(MaxRootLibraryBytes) +
                " ] || fail LIBRARY_SIZE; printf 'VSCOPE_ELF_BEGIN %s\\n' \"$before\"; "
                "head -c \"$3\" \"$p\" || fail LIBRARY_READ; "
                "[ \"$(stat -L -c '%d %i %s' \"$p\")\" = \"$before\" ] || fail LIBRARY_CHANGED; " +
                identity + " || fail TARGET_CHANGED; " + mapping + " || fail MAPPING_CHANGED; "
                "printf '\\nVSCOPE_ELF_END\\nVSCOPE_ELF_COMPLETE\\n'";
            const auto rootRead = run("timeout -s TERM 3 sh -c " + Quote(script), 4000, false, MaxRootLibraryBytes + 4096);
            if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
            std::string readReason;
            data = rootLibrary(rootRead, expectedDevice, expectedInode, readReason);
            note(path + (data ? ": read through root target namespace\n" : ": root library read failed: ") +
                 (data ? std::string{} : readReason + " " + commandDetail(rootRead, true) + "\n"));
        } else {
            note(path + ": verified local ELF; probe uses target namespace\n");
        }
        if (!data) { ++unreadable; continue; }
        const auto symbolsBefore = probes.size();
        for (const auto &symbol : {std::pair{"eglSwapBuffers", "egl_swap"},
                                  {"eglSwapBuffersWithDamageKHR", "egl_damage"},
                                  {"eglSwapBuffersWithDamageEXT", "egl_damage_ext"},
                                  {"vkQueuePresentKHR", "vk_present"}, {"vkQueueSubmit", "vk_submit"},
                                  {"vkQueueSubmit2", "vk_submit2"}, {"vkQueueSubmit2KHR", "vk_submit2_khr"},
                                  {"glDrawArrays", "gl_draw"}, {"glDrawElements", "gl_elements"}}) {
            const auto at = symbolOffset(*data, symbol.first);
            if (at) probes.push_back({std::string(symbol.second) + "_" + std::to_string(candidates), targetPath, validation, *at});
        }
        if (probes.size() == symbolsBefore) note(path + ": no additional supported dynamic function symbols\n");
    }
    if (probes.empty()) {
        note("Target pid=" + std::to_string(pid) + "; matching library candidates=" + std::to_string(candidates) + "\n");
        if (!candidates && !cancel) {
            const auto sample = run(identity + " || exit 1; printf 'GRAPHICS_PATH_MATCHES\\n'; "
                "sed -n '/[Ee][Gg][Ll]/p; /[Gg][Ll][Ee][Ss]/p; /[Vv][Uu][Ll][Kk][Aa][Nn]/p' " +
                process + "/maps | head -c 8192; printf '\\nEXECUTABLE_MAPS_SAMPLE\\n'; "
                "sed -n '/^[^ ]* [^ ]*x[^ ]* /p' " + process + "/maps | head -c 4096", 1500);
            if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
            if (diagnostics) (*diagnostics)["graphics_api_maps_sample"] = sample.success
                ? sample.output.substr(0, 12400) : "Maps diagnostic read failed: " + sample.error;
            note("No supported mappings matched; executable maps sample is in graphics_api_maps_sample\n");
        }
        const auto why = unreadable ? "EGL/Vulkan libraries unreadable or mapping identity changed" :
                         candidates ? "no supported EGL/Vulkan function symbols" : "no mapped EGL/Vulkan loader libraries";
        return finish(candidates ? "symbols_unavailable" : "libraries_unavailable", "N/A (" + std::string(why) + ")");
    }
    note("Probe count: " + std::to_string(probes.size()) + "\n");
    const auto group = "vscope_" + std::to_string(getpid()) + "_" + std::to_string(MonotonicNs());
    /** @brief 两处 tracefs 均按唯一事件组清理，不依赖采样和后续 su 的挂载路径选择相同。 */
    std::string cleanup = "cleanup_ok=0; for t in /sys/kernel/tracing /sys/kernel/debug/tracing; do "
        "[ ! -d \"$t/instances/" + group + "\" ] || { "
        "echo 0 > \"$t/instances/" + group + "/tracing_on\" || cleanup_ok=1; "
        "echo 0 > \"$t/instances/" + group + "/events/enable\" || cleanup_ok=1; "
        "rmdir \"$t/instances/" + group + "\" || cleanup_ok=1; }; ";
    for (const auto &probe : probes)
        cleanup += "[ ! -d \"$t/events/" + group + "/" + probe.event + "\" ] || { echo '-:" + group +
                   "/" + probe.event + "' >> \"$t/uprobe_events\" || cleanup_ok=1; }; ";
    cleanup += "done; ";
    const std::string locate = "t=''; for trace_root in /sys/kernel/tracing /sys/kernel/debug/tracing; do "
        "[ ! -e \"$trace_root/uprobe_events\" ] || { t=$trace_root; break; }; done; "
        "[ -n \"$t\" ] || fail TRACEFS_MISSING; [ -w \"$t/uprobe_events\" ] || fail TRACEFS_DENIED; "
        "[ -d \"$t/instances\" ] || fail TRACE_INSTANCES_MISSING; ";
    std::string script = fail + identity + " || fail TARGET_CHANGED; " + locate +
        "mkdir \"$t/instances/" + group + "\" || fail TRACE_INSTANCE_CREATE; "
        "trap " + Quote(cleanup) + " EXIT; trap 'exit 1' TERM INT HUP; "
        "i=\"$t/instances/" + group + "\"; echo 0 > \"$i/tracing_on\" || fail TRACE_SETUP; "
        "echo 16 > \"$i/buffer_size_kb\" || fail TRACE_SETUP; "
        "f=''; for task in /proc/" + std::to_string(pid) + "/task/*; do tid=${task##*/}; "
        "case \"$tid\" in ''|*[!0-9]*) continue;; esac; "
        "[ -z \"$f\" ] || f=\"$f || \"; f=\"${f}common_pid == $tid\"; done; "
        "[ -n \"$f\" ] || fail TARGET_THREADS; "
        "pid_filter=0; pid_count=0; if [ -e \"$i/set_event_pid\" ]; then "
        ": > \"$i/set_event_pid\" || fail PROBE_PID_FILTER; "
        "for task in /proc/" + std::to_string(pid) + "/task/*; do tid=${task##*/}; "
        "case \"$tid\" in ''|*[!0-9]*) continue;; esac; "
        "echo \"$tid\" >> \"$i/set_event_pid\" || fail PROBE_PID_FILTER; pid_count=$((pid_count + 1)); done; "
        "[ \"$pid_count\" -gt 0 ] || fail TARGET_THREADS; pid_filter=1; fi; "
        "installed=0; add_probe() { event=$1; definition=$2; "
        "if ! (printf '%s\\n' \"$definition\" >> \"$t/uprobe_events\"); then "
        "echo VSCOPE_PROBE_SKIPPED=CREATE:$event; return; fi; "
        "if [ \"$pid_filter\" -eq 0 ] && ! (echo \"$f\" > \"$i/events/" + group + "/$event/filter\"); then "
        "echo VSCOPE_PROBE_SKIPPED=FILTER:$event; return; fi; "
        "if ! (echo 1 > \"$i/events/" + group + "/$event/enable\"); then "
        "echo 0 > \"$i/events/" + group + "/$event/enable\" || fail PROBE_DISABLE; "
        "echo VSCOPE_PROBE_SKIPPED=ENABLE:$event; return; fi; "
        "installed=$((installed + 1)); echo VSCOPE_PROBE_ENABLED=$event; }; ";
    std::set<std::string> validated;
    for (const auto &probe : probes) {
        if (validated.insert(probe.path).second) script += probe.validation + " || fail LIBRARY_IDENTITY; ";
        std::ostringstream hex;
        hex << "0x" << std::hex << probe.offset;
        script += "add_probe " + Quote(probe.event) + " " +
            Quote("p:" + group + "/" + probe.event + " " + probe.path + ":" + hex.str()) + "; ";
    }
    script += "[ \"$installed\" -gt 0 ] || fail PROBE_NONE_ENABLED; " + identity + " || fail TARGET_CHANGED; echo 1 > \"$i/tracing_on\" || fail TRACE_START; "
        "sleep 0.2 || fail TRACE_WAIT; echo 0 > \"$i/tracing_on\" || fail TRACE_STOP; "
        "trace_text=$(head -c 196609 \"$i/trace\" && printf x) || fail TRACE_READ; "
        "trace_text=${trace_text%x}; [ ${#trace_text} -lt 196609 ] || fail TRACE_TRUNCATED; printf '%s\\n' \"$trace_text\"; " +
        identity + " || fail TARGET_CHANGED; echo TRACE_COMPLETE; ";
    /** @brief root shell 内部也设置时限，避免 su 服务脱离应用进程组后继续保留探针。 */
    auto capture = run("timeout -s TERM 2 sh -c " + Quote(script), 3000);
    std::istringstream probeLines(capture.output);
    std::string probeLine, probeReport;
    while (std::getline(probeLines, probeLine)) {
        if (probeLine.rfind("VSCOPE_PROBE_", 0) == 0 && probeReport.size() < 2048)
            probeReport += probeLine.substr(0, 2048 - probeReport.size()) + "\n";
    }
    if (diagnostics) (*diagnostics)["graphics_api_probe_report"] = probeReport;
    note(probeReport);
    /** @brief su 可能不转发取消信号；用独立有界 root 命令保证正常停止后的清理尝试。 */
    const auto settle = capture.success ? std::string{} : std::string{"sleep 2.1; "};
    auto cleaned = run(settle + cleanup + "exit $cleanup_ok", 5000, true);
    if (!cleaned.success || cleaned.truncated)
        return finish("cleanup_failed", "N/A (trace cleanup failed; restart device before retrying)", commandDetail(cleaned));
    if (cancel) return finish("cancelled", "N/A (graphics tracing cancelled)");
    if (!capture.success || capture.truncated || !endsWith(capture.output, TraceComplete)) {
        const auto stage = failureStage(capture.output);
        const auto status = stage.rfind("TRACEFS_", 0) == 0 || stage.rfind("TRACE_INSTANCE", 0) == 0 ? "trace_unavailable" :
                            stage.rfind("PROBE_", 0) == 0 ? "probe_failed" :
                            stage.rfind("TARGET_", 0) == 0 ? "target_unavailable" :
                            stage.rfind("LIBRARY_", 0) == 0 ? "symbols_unavailable" : "capture_failed";
        const auto why = stage.empty() ? "graphics capture failed or incomplete" : "graphics tracing failed: " + stage;
        return finish(status, "N/A (" + why + ")", commandDetail(capture));
    }
    const auto result = GraphicsTraceResult(capture.output);
    return finish(result.rfind("N/A", 0) == 0 ? "no_calls" : "observed", result,
                  result.rfind("N/A", 0) == 0 ? "No calls observed at accessible loader/driver symbols in 200 ms; loaded libraries do not confirm active rendering" : "");
}
}
