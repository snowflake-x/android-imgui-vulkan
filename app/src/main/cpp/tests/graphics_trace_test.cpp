#include "monitor/graphics_trace.h"
#include "monitor/model.h"
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
using namespace vulkanscope::monitor;
namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void symbols() {
    require(GraphicsSymbolOffset(TRACE_FIXTURE, "eglSwapBuffers").has_value(), "resolve defined EGL function");
    require(GraphicsSymbolOffset(TRACE_FIXTURE, "vkQueuePresentKHR").has_value(), "resolve defined Vulkan function");
    require(!GraphicsSymbolOffset(TRACE_FIXTURE, "vkMissing"), "missing symbols are not guessed");
    const auto path = "/tmp/vscope-elf-" + std::to_string(getpid());
    std::ifstream input(TRACE_FIXTURE, std::ios::binary);
    std::string data{std::istreambuf_iterator<char>(input), {}};
    for (std::size_t size : {0U, 16U, 32U, 64U, 128U}) {
        { std::ofstream output(path, std::ios::binary); output.write(data.data(), size); }
        require(!GraphicsSymbolOffset(path, "eglSwapBuffers"), "reject truncated ELF tables");
    }
    data[5] = 2;
    { std::ofstream output(path, std::ios::binary); output.write(data.data(), data.size()); }
    require(!GraphicsSymbolOffset(path, "eglSwapBuffers"), "reject unsupported endian");
    std::remove(path.c_str());
}
void elf32() {
    std::string bytes(2048, '\0');
    Elf32_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_machine = EM_ARM;
    header.e_phoff = 0x80; header.e_phnum = 1; header.e_phentsize = sizeof(Elf32_Phdr);
    header.e_shoff = 0x100; header.e_shnum = 2; header.e_shentsize = sizeof(Elf32_Shdr);
    Elf32_Phdr segment{};
    segment.p_type = PT_LOAD; segment.p_flags = PF_X;
    segment.p_offset = 0x400; segment.p_vaddr = 0x1000; segment.p_filesz = 0x100;
    Elf32_Shdr symbols{}, strings{};
    symbols.sh_type = SHT_DYNSYM; symbols.sh_link = 1; symbols.sh_offset = 0x300;
    symbols.sh_size = sizeof(Elf32_Sym); symbols.sh_entsize = sizeof(Elf32_Sym);
    strings.sh_offset = 0x350; strings.sh_size = 32;
    Elf32_Sym symbol{};
    symbol.st_name = 1; symbol.st_shndx = 1; symbol.st_info = STT_FUNC; symbol.st_value = 0x1011;
    std::memcpy(bytes.data() + 0x351, "eglSwapBuffers", 15);
    const auto path = "/tmp/vscope-elf32-" + std::to_string(getpid());
    auto save = [&] {
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + 0x80, &segment, sizeof(segment));
        std::memcpy(bytes.data() + 0x100, &symbols, sizeof(symbols));
        std::memcpy(bytes.data() + 0x100 + sizeof(symbols), &strings, sizeof(strings));
        std::memcpy(bytes.data() + 0x300, &symbol, sizeof(symbol));
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), bytes.size());
    };
    save();
    require(GraphicsSymbolOffset(path, "eglSwapBuffers") == 0x410, "ARM Thumb virtual address becomes file offset");
    symbol.st_shndx = SHN_UNDEF; save();
    require(!GraphicsSymbolOffset(path, "eglSwapBuffers"), "undefined imports cannot become uprobes");
    symbol.st_shndx = 1; segment.p_flags = PF_R; save();
    require(!GraphicsSymbolOffset(path, "eglSwapBuffers"), "non-executable segments cannot become uprobes");
    segment.p_flags = PF_X; header.e_shoff = 0xfffffff0U; save();
    require(!GraphicsSymbolOffset(path, "eglSwapBuffers"), "out-of-range section offsets are rejected");
    std::remove(path.c_str());
}
void evidence() {
    require(GraphicsTraceResult(" thread-25 [000] .... 1.2: gl_draw_2: (0x1)\n") == "OpenGL ES (calls observed)",
            "driver draw events bypass loader dispatch");
    require(GraphicsTraceResult(" thread-25 [000] .... 1.2: vk_present_3: (0x1)\n") == "Vulkan (calls observed)",
            "driver Vulkan events are recognized");
    require(GraphicsProbeLibrary("libGLESv2_adreno.so") && GraphicsProbeLibrary("vulkan.adreno.so"), "Adreno driver candidates");
    require(!GraphicsProbeLibrary("libunity.so"), "engines are not graphics drivers");
    require(GraphicsMappedLibraries("1000-2000 r-xp 0000 00:01 1 /vendor/lib64/egl/libGLESv2_adreno.so\n") == "OpenGL ES", "parse driver maps");
    require(GraphicsMappedLibraries("1000-2000 r--p 0000 00:01 1 /vendor/lib64/egl/libGLESv2_adreno.so\n").empty(), "require executable mapping");
    require(GraphicsMappedLibraries("1000-2000 r-xp 0000 00:01 1 /system/lib64/libEGL.so\n1000-2000 r-xp 0000 00:01 2 /vendor/lib64/hw/vulkan.adreno.so\n") == "OpenGL ES + Vulkan", "coexisting libraries are ambiguous");
    require(GraphicsTraceResult("libvulkan.so eglSwapBuffers").rfind("N/A", 0) == 0,
            "library names are not evidence of calls");
    require(GraphicsTraceResult("# ignored: vk_present:\n").rfind("N/A", 0) == 0, "ignore trace comments");
    require(GraphicsTraceResult(" thread-25 [000] .... 1.2: egl_swap: (0x1)\n").find("OpenGL ES") == 0,
            "recognize actual EGL event");
    require(GraphicsTraceResult(" thread-25 [000] .... 1.2: vk_submit2: (0x1)\n").find("Vulkan") == 0,
            "recognize synchronization2 submit");
    require(GraphicsTraceResult(" thread-25 [000] .... 1.2: egl_damage_ext: (0x1)\n thread-25 [000] .... 1.2: vk_present: (0x1)\n") ==
            "OpenGL ES + Vulkan (calls observed)", "report mixed evidence without choosing an API");
}
std::string mappedTarget(const std::string &path, const struct stat &st) {
    std::ostringstream map;
    map << "PID=123\nSTART=456\nUID=10001\n1000-2000 r-xp 0000 " << std::hex << major(st.st_dev)
        << ':' << minor(st.st_dev) << std::dec << ' ' << st.st_ino << ' ' << path
        << "\nVSCOPE_GRAPHICS_TARGET_COMPLETE\n";
    return map.str();
}
std::string elfTransfer(const std::string &bytes, const struct stat &st) {
    return "VSCOPE_ELF_BEGIN " + std::to_string(st.st_dev) + " " + std::to_string(st.st_ino) + " " +
        std::to_string(bytes.size()) + "\n" + bytes + "\nVSCOPE_ELF_END\nVSCOPE_ELF_COMPLETE\n";
}
void syntax(const std::string &script, int timeout) {
    require(timeout > 0 && timeout <= 5000, "bounded root commands");
    require(script.find("awk ") == std::string::npos, "stock Android tracing does not require BusyBox awk");
    const std::atomic<bool> active{false};
    require(RunCommand({"sh", "-n", "-c", script}, active, 1000).success, "valid shell syntax");
}
std::string decodedScript(const std::string &command) {
    const auto start = command.find(" sh -c ");
    require(start != std::string::npos, "root capture command wraps one quoted shell script");
    const std::atomic<bool> active{false};
    const auto decoded = RunCommand({"sh", "-c", "printf '%s' " + command.substr(start + 7)}, active, 1000);
    require(decoded.success, "decode generated script without executing it");
    return decoded.output;
}
/** @brief 使用实际 shell 验证 trace 字节上限，尾部换行和多字节 comm 都不能掩盖截断。 */
void traceBoundaries(const std::string &capture) {
    const auto script = decodedScript(capture);
    const auto begin = script.find("trace_text=$(head");
    const auto end = script.find("[ \"$(sed", begin);
    require(begin != std::string::npos && end != std::string::npos, "locate generated bounded trace reader");
    require(script.find("export LC_ALL=C;") == 0, "trace length is counted in bytes");
    auto reader = script.substr(begin, end - begin);
    const auto path = "/tmp/vscope-graphics-trace-" + std::to_string(getpid());
    const auto source = reader.find("\"$i/trace\"");
    require(source != std::string::npos, "trace reader uses isolated instance");
    reader.replace(source, std::strlen("\"$i/trace\""), Quote(path));
    const std::atomic<bool> active{false};
    for (const std::size_t size : {196608U, 196609U, 196610U}) {
        std::string text;
        while (text.size() < size) text += "线程\n";
        text.resize(size);
        text.back() = '\n';
        { std::ofstream out(path, std::ios::binary); out.write(text.data(), text.size()); }
        const auto captured = RunCommand({"sh", "-c",
            "export LC_ALL=C; fail() { echo FAILURE=$1; exit 1; }; " + reader}, active, 1000, 262144);
        if (size < 196609) require(captured.success, "complete trace below byte limit is accepted");
        else require(!captured.success && captured.output.find("FAILURE=TRACE_TRUNCATED") != std::string::npos,
                     "trace hitting byte limit remains truncated even with a newline at the boundary");
    }
    std::remove(path.c_str());
}
void lifecycle() {
    std::atomic<bool> cancel{false};
    struct stat st{};
    require(stat(TRACE_FIXTURE, &st) == 0, "fixture stat");
    for (int failure = 0; failure < 3; ++failure) {
        int calls = 0;
        Values diagnostics;
        auto runner = [&](const std::string &script, int timeout) -> CommandResult {
            syntax(script, timeout);
            ++calls;
            if (calls == 1) return {true, mappedTarget(TRACE_FIXTURE, st), {}};
            if (calls == 2) {
                require(script.find("common_pid == $tid") != std::string::npos, "thread filters present");
                require(script.find("/instances/vscope_") != std::string::npos, "isolated trace instance");
                require(script.find("/stat | cut") != std::string::npos && script.find("/cmdline") != std::string::npos,
                        "validate process generation and package");
                require(script.find("/proc/123/root" TRACE_FIXTURE) != std::string::npos,
                        "even local ELF offsets install probes in target mount namespace");
                require(script.find("stat -L -c") != std::string::npos && script.find("LIBRARY_IDENTITY") != std::string::npos,
                        "mapped device/inode is checked again before installing probes");
                require(script.find("trap ") != std::string::npos, "cleanup on shell exit");
                require(script.find("timeout -s TERM 2 sh -c ") == 0, "root-side timeout survives su detachment");
                if (failure) { cancel = true; return {false, {}, "Cancelled"}; }
                traceBoundaries(script);
                return {true, " thread-25 [000] .... 1.2: vk_present: (0x1)\nTRACE_COMPLETE\n", {}};
            }
            require(calls == 3 && script.find("exit $cleanup_ok") != std::string::npos,
                    "independent cleanup even after cancellation");
            require(script.find("for t in /sys/kernel/tracing /sys/kernel/debug/tracing") != std::string::npos,
                    "cleanup checks both tracefs locations");
            return {failure != 2, {}, failure == 2 ? "denied" : ""};
        };
        const auto result = TraceGraphics("com.example.game", 0, cancel, runner, &diagnostics);
        require(calls == 3, "capture is followed by cleanup");
        if (failure == 0) require(result == "Vulkan (calls observed)" && diagnostics.at("graphics_api_status") == "observed",
                                  "confirmed Vulkan");
        if (failure == 1) require(diagnostics.at("graphics_api_status") == "cancelled", "cancelled captures unknown");
        if (failure == 2) require(result.find("cleanup failed") != std::string::npos &&
                                  diagnostics.at("graphics_api_status") == "cleanup_failed", "cleanup failures visible");
        cancel = false;
    }
    int calls = 0;
    require(TraceGraphics("bad;id", 0, cancel, [&](const std::string &, int) {
        ++calls; return CommandResult{};
    }).find("invalid target") != std::string::npos && calls == 0, "reject shell metacharacters before execution");
}
/** @brief 主应用不可读或命名空间不一致时，通过 root 读取的原始 ELF 仍须通过映射身份与完整性校验。 */
void rootFallback() {
    std::atomic<bool> cancel{false};
    struct stat st{};
    require(stat(TRACE_FIXTURE, &st) == 0, "fixture stat");
    std::ifstream input(TRACE_FIXTURE, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    const std::string inaccessible = "/vscope-inaccessible-mount/libEGL.so";
    for (int failure = 0; failure < 9; ++failure) {
        Values diagnostics;
        int calls = 0;
        auto runner = [&](const std::string &script, int timeout) -> CommandResult {
            syntax(script, timeout);
            ++calls;
            if (calls == 1) return {true, mappedTarget(inaccessible, st), {}};
            if (calls == 2) {
                require(script.find("VSCOPE_ELF_BEGIN") != std::string::npos &&
                        script.find("/proc/123/root" + inaccessible) != std::string::npos,
                        "root fallback addresses the target namespace ELF");
                require(script.find("16777216") != std::string::npos &&
                        script.find("LIBRARY_CHANGED") != std::string::npos &&
                        script.find("MAPPING_CHANGED") != std::string::npos &&
                        script.find("/cmdline") != std::string::npos,
                        "root transfer bounds size and rechecks mapping, file and process identities");
                auto metadata = st;
                if (failure == 1) ++metadata.st_ino;
                if (failure == 2) ++metadata.st_dev;
                CommandResult transfer{true, elfTransfer(bytes, metadata), {}};
                if (failure == 3) transfer.truncated = true;
                if (failure == 4) transfer.output.erase(transfer.output.size() - 8);
                if (failure == 5) transfer.output = elfTransfer(std::string(128, '\0'), metadata);
                if (failure == 6) { transfer.success = false; transfer.output = "VSCOPE_GRAPHICS_ERROR=LIBRARY_IDENTITY\n"; }
                if (failure == 7) transfer.output = "VSCOPE_ELF_BEGIN " + std::to_string(st.st_dev) + " " +
                    std::to_string(st.st_ino) + " 16777217\n";
                if (failure == 8) cancel = true;
                return transfer;
            }
            require(failure == 0, "incomplete or mismatched root files never reach probe installation");
            if (calls == 3) return {true, " thread-25 [000] .... 1.2: egl_swap: (0x1)\nTRACE_COMPLETE\n", {}};
            require(calls == 4 && script.find("exit $cleanup_ok") != std::string::npos, "root fallback still cleans probes");
            return {true, {}, {}};
        };
        const auto result = TraceGraphics("com.example.game", 0, cancel, runner, &diagnostics);
        if (!failure) {
            require(calls == 4 && result == "OpenGL ES (calls observed)", "root-only readable library yields observed API");
            require(diagnostics.at("graphics_api_details").find("read through root target namespace") != std::string::npos,
                    "successful fallback is diagnosable");
        } else {
            require(calls == 2 && result.rfind("N/A", 0) == 0, "root ELF rejection does not infer API from library presence");
            require(diagnostics.at("graphics_api_status") == (failure == 8 ? "cancelled" : "symbols_unavailable"),
                    "root fallback failure category");
            if (failure == 1 || failure == 2)
                require(diagnostics.at("graphics_api_details").find("device/inode differs") != std::string::npos,
                        "root metadata mismatch has explicit diagnostics");
            if (failure == 6)
                require(diagnostics.at("graphics_api_details").find("LIBRARY_IDENTITY") != std::string::npos,
                        "root shell failure stage is preserved without logging ELF bytes");
            require(diagnostics.at("graphics_api_details").find('\0') == std::string::npos,
                    "binary ELF data never reaches diagnostic output");
        }
        cancel = false;
    }
}
/** @brief 真实过滤命令能发现大型 maps 末尾的 loader，不会先截断无关 Unity 映射。 */
void largeMappings() {
    std::atomic<bool> cancel{false};
    struct stat st{};
    require(stat(TRACE_FIXTURE, &st) == 0, "fixture stat");
    const auto path = "/tmp/vscope-graphics-maps-" + std::to_string(getpid());
    const auto mapping = mappedTarget(TRACE_FIXTURE, st);
    const auto begin = mapping.find("1000-2000");
    const auto end = mapping.find("VSCOPE_GRAPHICS_TARGET_COMPLETE");
    const auto loaderLine = mapping.substr(begin, end - begin);
    const std::string drivers =
        "7000-8000 r-xp 0000 00:01 9 /vendor/lib64/egl/libGLESv2_adreno.so\n"
        "8000-9000 r-xp 0000 00:01 10 /vendor/lib64/hw/vulkan.adreno.so\n"
        "9000-a000 r-xp 0000 00:01 11 /system/lib64/libEGL.so (deleted)\n"
        "a000-b000 r-xp 0000 00:01 12 /vendor/lib64/egl/libGLES_mali.so\n";
    {
        std::ofstream maps(path);
        for (int i = 0; i < 6000; ++i)
            maps << "3000-4000 r-xp 0000 08:01 1024 /data/app/com.example.game/lib/arm64/libUnity.so\n";
        maps << "1000-2000 r--p 0000 08:01 1024 /system/lib64/libEGL.so\n";
        maps << loaderLine << drivers;
    }
    int calls = 0;
    Values diagnostics;
    const auto result = TraceGraphics("com.example.game", 0, cancel, [&](const std::string &script, int timeout) {
        syntax(script, timeout);
        if (++calls == 1) {
            const auto start = script.find("sed -n -E '/^");
            require(start != std::string::npos, "filter maps before bounded output capture using explicit POSIX ERE");
            require(script.find("\\|") == std::string::npos, "do not use GNU BRE alternation on Android");
            const auto stop = script.find(" || fail TARGET_MAPS_READ", start);
            require(stop != std::string::npos, "mapping read failure propagates");
            auto filter = script.substr(start, stop - start);
            const auto fileAt = filter.find("/proc/$p/maps");
            require(fileAt != std::string::npos, "filter reads target process mappings");
            filter.replace(fileAt, std::strlen("/proc/$p/maps"), Quote(path));
            const auto filtered = RunCommand({"sh", "-c", filter}, cancel, 1000);
            require(filtered.success && filtered.output == loaderLine + drivers,
                    "real sed filters a loader beyond 256 KiB and excludes nonexecutable/unrelated mappings");
            return CommandResult{true, "PID=123\nSTART=456\nUID=10001\n" + loaderLine +
                "VSCOPE_GRAPHICS_TARGET_COMPLETE\n", {}};
        }
        if (calls == 2) return CommandResult{true, " thread-25 [000] .... 1.2: egl_swap: (0x1)\nTRACE_COMPLETE\n", {}};
        return CommandResult{true, {}, {}};
    }, &diagnostics);
    std::remove(path.c_str());
    require(calls == 3 && result == "OpenGL ES (calls observed)", "large target maps still yield an observed API");
}

/** @brief 在临时 proc 镜像执行真实 root 读取脚本，覆盖 stat、grep、进程身份和二进制传输。 */
void realRootProtocol() {
    std::atomic<bool> cancel{false};
    const auto fixture = std::filesystem::path("/tmp/vscope-graphics-proc-" + std::to_string(getpid()));
    const std::string mapped = "/apex/com.android.graphics+test/lib64/libEGL.so";
    const auto file = fixture.string() + "/root" + mapped;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path());
    std::filesystem::copy_file(TRACE_FIXTURE, file, std::filesystem::copy_options::overwrite_existing);
    struct stat st{};
    require(stat(file.c_str(), &st) == 0, "target namespace ELF fixture stat");
    { std::ofstream status(fixture / "status"); status << "Uid:\t10001\t10001\t10001\t10001\n"; }
    {
        std::ofstream identity(fixture / "stat");
        identity << "123 (com.example.game) S";
        for (int token = 2; token < 20; ++token) identity << " 0";
        identity << " 456\n";
    }
    const auto target = mappedTarget(mapped, st);
    const auto begin = target.find("1000-2000");
    const auto end = target.find("VSCOPE_GRAPHICS_TARGET_COMPLETE");
    { std::ofstream maps(fixture / "maps"); maps << target.substr(begin, end - begin); }
    for (int changed = 0; changed < 3; ++changed) {
        {
            std::ofstream command(fixture / "cmdline", std::ios::binary);
            const std::string package = changed == 1 ? "com.other.game" : "com.example.game";
            command.write(package.c_str(), package.size() + 1);
        }
        if (changed == 2) {
            auto badPath = target.substr(begin, end - begin);
            const auto literal = badPath.find("graphics+test");
            require(literal != std::string::npos, "ERE literal test path");
            badPath.replace(literal, std::strlen("graphics+test"), "graphicstest");
            { std::ofstream maps(fixture / "maps"); maps << badPath; }
        }
        int calls = 0;
        Values diagnostics;
        const auto result = TraceGraphics("com.example.game", 0, cancel, [&](const std::string &script, int timeout) {
            syntax(script, timeout);
            if (++calls == 1) return CommandResult{true, target, {}};
            if (calls == 2) {
                auto actual = decodedScript(script);
                std::size_t at = 0;
                while ((at = actual.find("/proc/123", at)) != std::string::npos) {
                    actual.replace(at, 9, fixture.string());
                    at += fixture.string().size();
                }
                const auto transfer = RunCommand({"sh", "-c", actual}, cancel, timeout, 16 * 1024 * 1024 + 4096);
                require(transfer.success == (changed == 0), "real root reader verifies target process and exact mapped path");
                return transfer;
            }
            require(changed == 0, "mismatched target does not install probes");
            if (calls == 3) return CommandResult{true, " thread-25 [000] .... 1.2: egl_swap: (0x1)\nTRACE_COMPLETE\n", {}};
            return CommandResult{true, {}, {}};
        }, &diagnostics);
        require(calls == (changed ? 2 : 4), "root ELF command lifecycle");
        if (!changed) require(result == "OpenGL ES (calls observed)", "real root ELF bytes resolve working probes");
        else require(diagnostics.at("graphics_api_details").find(changed == 1 ? "TARGET_CHANGED" : "MAPPING_CHANGED") != std::string::npos,
                     "identity and literal mapping failures retain their exact stage");
    }
    std::filesystem::remove_all(fixture);
}

void driverDispatch() {
    const auto directory = std::filesystem::path("/tmp/vscope-driver-" + std::to_string(getpid()));
    std::filesystem::create_directory(directory);
    const auto driver = (directory / "vulkan.adreno.so").string();
    std::filesystem::copy_file(TRACE_FIXTURE, driver, std::filesystem::copy_options::overwrite_existing);
    struct stat loaderStat{}, driverStat{};
    require(stat(TRACE_FIXTURE, &loaderStat) == 0 && stat(driver.c_str(), &driverStat) == 0, "driver fixture identity");
    auto target = mappedTarget(TRACE_FIXTURE, loaderStat);
    auto second = mappedTarget(driver, driverStat);
    second.erase(0, second.find("1000-2000"));
    target.replace(target.find("VSCOPE_GRAPHICS_TARGET_COMPLETE"), std::string::npos, second);
    std::atomic<bool> cancel{false};
    Values diagnostics;
    int calls = 0;
    auto result = TraceGraphics("com.example.game", 0, cancel, [&](const std::string &script, int timeout) {
        syntax(script, timeout);
        if (++calls == 1) return CommandResult{true, target, {}};
        if (calls == 2) {
            require(script.find("/vk_present_1") != std::string::npos && script.find("/vk_present_2") != std::string::npos,
                    "same-named driver function gets its own probe alongside loader");
            require(script.find("/proc/123/root" + driver) != std::string::npos, "driver inode used for direct dispatch probe");
            return CommandResult{true, " thread-25 [000] .... 1.2: vk_present_2: (0x1)\nTRACE_COMPLETE\n", {}};
        }
        require(script.find("/vk_present_2") != std::string::npos, "driver probe included in cleanup");
        return CommandResult{true, {}, {}};
    }, &diagnostics);
    require(calls == 3 && result == "Vulkan (calls observed)" && diagnostics.at("graphics_api_status") == "observed",
            "driver-only calls identify Vulkan without loader calls");
    std::filesystem::remove_all(directory);
}

void classifications() {
    std::atomic<bool> cancel{false};
    struct stat st{};
    require(stat(TRACE_FIXTURE, &st) == 0, "fixture stat");
    struct Case { CommandResult result; const char *status; };
    const std::string observed = " thread-25 [000] .... 1.2: vk_present: (0x1)\nTRACE_COMPLETE\n";
    for (const auto &test : {
            Case{{true, "# no events\nTRACE_COMPLETE\n", {}}, "no_calls"},
            Case{{true, observed, {}, true}, "capture_failed"},
            Case{{true, " thread-25 [000] .... 1.2: vk_present: (0x1)\n", {}}, "capture_failed"},
            Case{{false, "VSCOPE_GRAPHICS_ERROR=TRACEFS_MISSING\n", "failed"}, "trace_unavailable"},
            Case{{false, "VSCOPE_GRAPHICS_ERROR=TRACEFS_DENIED\n", "failed"}, "trace_unavailable"},
            Case{{false, "VSCOPE_GRAPHICS_ERROR=PROBE_CREATE:egl_swap\n", "failed"}, "probe_failed"},
            Case{{false, "VSCOPE_GRAPHICS_ERROR=TARGET_CHANGED\n", "failed"}, "target_unavailable"},
            Case{{false, "VSCOPE_GRAPHICS_ERROR=LIBRARY_IDENTITY\n", "failed"}, "symbols_unavailable"},
            Case{{false, std::string(8192, 'x'), "Command timed out"}, "capture_failed"}}) {
        int calls = 0;
        Values diagnostics;
        const auto result = TraceGraphics("com.example.game", 0, cancel, [&](const std::string &, int) {
            if (++calls == 1) return CommandResult{true, mappedTarget(TRACE_FIXTURE, st), {}};
            if (calls == 2) return test.result;
            return CommandResult{true, {}, {}};
        }, &diagnostics);
        const bool mapped = std::string(test.status) == "no_calls" || std::string(test.status) == "trace_unavailable";
        require(calls == 3 && (mapped ? result.find("calls unconfirmed") != std::string::npos : result.rfind("N/A", 0) == 0),
                "failed or event-free captures never claim calls and are cleaned");
        require(diagnostics.at("graphics_api_status") == (mapped ? "mapped" : test.status), "loaded evidence distinguished from calls");
        require(diagnostics.at("graphics_api_trace_status") == test.status, "failure stage has stable status code");
        require(diagnostics.at("graphics_api_details").size() <= 4096, "diagnostics are bounded");
    }
    int calls = 0;
    Values diagnostics;
    TraceGraphics("com.example.game", 0, cancel, [&](const std::string &, int) {
        ++calls;
        return CommandResult{true, mappedTarget("/system/lib64/libUnrelated.so", st), {}};
    }, &diagnostics);
    require(calls == 2 && diagnostics.at("graphics_api_status") == "libraries_unavailable" &&
            diagnostics.count("graphics_api_maps_sample") == 1,
            "no matching graphics mappings retain a diagnostic map sample separately from symbol failure");
    calls = 0;
    TraceGraphics("com.example.game", 0, cancel, [&](const std::string &, int) {
        ++calls;
        return CommandResult{true, mappedTarget(TRACE_FIXTURE, st), {}, true};
    }, &diagnostics);
    require(calls == 1 && diagnostics.at("graphics_api_status") == "target_unavailable",
            "truncated process mappings cannot produce a confirmed API");
    cancel = true;
    calls = 0;
    TraceGraphics("com.example.game", 0, cancel, [&](const std::string &, int) {
        ++calls;
        return CommandResult{};
    }, &diagnostics);
    require(calls == 0 && diagnostics.at("graphics_api_status") == "cancelled", "pre-cancelled traces do not spawn commands");
}
}
int main() {
    try { symbols(); elf32(); evidence(); lifecycle(); rootFallback(); largeMappings(); realRootProtocol(); driverDispatch(); classifications(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "graphics tracing tests passed\n";
}
