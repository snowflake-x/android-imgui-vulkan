#include "monitor/graphics_trace.h"
#include "monitor/model.h"
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fstream>
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
void lifecycle() {
    std::atomic<bool> cancel{false};
    struct stat st{};
    require(stat(TRACE_FIXTURE, &st) == 0, "fixture stat");
    std::ostringstream map;
    map << "PID=123\nSTART=456\nUID=10001\n1000-2000 r-xp 0000 " << std::hex << major(st.st_dev)
        << ':' << minor(st.st_dev) << std::dec << ' ' << st.st_ino << ' ' << TRACE_FIXTURE << '\n';
    for (int failure = 0; failure < 3; ++failure) {
        int calls = 0;
        auto runner = [&](const std::string &script, int timeout) -> CommandResult {
            require(timeout > 0 && timeout <= 5000, "bounded root commands");
            const std::atomic<bool> active{false};
            require(RunCommand({"sh", "-n", "-c", script}, active, 1000).success, "valid shell syntax");
            ++calls;
            if (calls == 1) return {true, map.str(), {}};
            if (calls == 2) {
                require(script.find("common_pid == $tid") != std::string::npos, "thread filters present");
                require(script.find("/instances/vscope_") != std::string::npos, "isolated trace instance");
                require(script.find("/stat | cut") != std::string::npos && script.find("/cmdline") != std::string::npos,
                        "validate process generation and package");
                require(script.find("trap ") != std::string::npos, "cleanup on shell exit");
                require(script.find("timeout -s TERM 2 sh -c ") == 0, "root-side timeout survives su detachment");
                if (failure) { cancel = true; return {false, {}, "Cancelled"}; }
                return {true, " thread-25 [000] .... 1.2: vk_present: (0x1)\nTRACE_COMPLETE\n", {}};
            }
            require(calls == 3 && script.find("exit $cleanup_ok") != std::string::npos,
                    "independent cleanup even after cancellation");
            return {failure != 2, {}, failure == 2 ? "denied" : ""};
        };
        const auto result = TraceGraphics("com.example.game", 0, cancel, runner);
        require(calls == 3, "capture is followed by cleanup");
        if (failure == 0) require(result == "Vulkan (calls observed)", "confirmed Vulkan");
        if (failure == 1) require(result.find("unavailable or denied") != std::string::npos, "failed captures unknown");
        if (failure == 2) require(result.find("cleanup failed") != std::string::npos, "cleanup failures visible");
        cancel = false;
    }
    int calls = 0;
    require(TraceGraphics("bad;id", 0, cancel, [&](const std::string &, int) {
        ++calls; return CommandResult{};
    }).find("invalid target") != std::string::npos && calls == 0, "reject shell metacharacters before execution");
}
}
int main() {
    try { symbols(); elf32(); evidence(); lifecycle(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "graphics tracing tests passed\n";
}
