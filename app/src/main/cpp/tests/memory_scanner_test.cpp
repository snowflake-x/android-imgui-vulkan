#include "features/memory_scanner.h"
#include "memory/inspect.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

using namespace vulkanscope;
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("memory scanner contract failed"); }
void put(std::vector<unsigned char> &v, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) v.at(at + i) = static_cast<unsigned char>(value >> (i * 8));
}
memory::Reader reader(const std::vector<unsigned char> &v) {
    return [&v](std::uint64_t at, void *out, std::size_t n) -> std::size_t {
        if (at >= v.size()) return 0;
        n = std::min(n, v.size() - static_cast<std::size_t>(at));
        std::memcpy(out, v.data() + at, n);
        return n;
    };
}
void patternsAndScan() {
    require(memory::Pattern("i32", "-1") == std::vector<unsigned char>({255, 255, 255, 255}));
    require(memory::Pattern("f32", "1.0") == std::vector<unsigned char>({0, 0, 128, 63}));
    require(memory::Pattern("i64", "4294967296") == std::vector<unsigned char>({0, 0, 0, 0, 1, 0, 0, 0}));
    require(memory::Pattern("hex", "00 aF FF") == std::vector<unsigned char>({0, 175, 255}));
    for (auto p : {std::pair{"hex", "A"}, {"hex", "GG"}, {"i32", "2147483648"}, {"f32", "NaN"}, {"i64", "1x"}}) {
        bool rejected = false;
        try { memory::Pattern(p.first, p.second); } catch (const std::exception &) { rejected = true; }
        require(rejected);
    }
    auto maps = memory::ParseMaps("1000-2000 r--p 0000 00:00 0 /a\n2000-3000 rw-p 0000 00:00 0\n"
        "3000-4000 ---p 0000 00:00 0\n4000-5000 rw-p 0000 00:00 0 /dev/kgsl-3d0\nbad r--p 0 00 0\n");
    require(maps.size() == 2 && maps[0].path == "/a");
    std::vector<unsigned char> data(131072, 0);
    const auto pattern = memory::Pattern("utf8", "needle");
    std::copy(pattern.begin(), pattern.end(), data.begin() + 65533);
    const memory::Stop no = [] { return false; };
    auto r = memory::Scan({{0, data.size(), ""}}, reader(data), no, pattern);
    require(r.addresses == std::vector<std::uint64_t>{65533} && !r.partial && r.bytes == data.size());
    r = memory::Scan({{0, 65536, ""}, {65536, data.size(), ""}}, reader(data), no, pattern);
    require(r.addresses == std::vector<std::uint64_t>{65533});
    r = memory::Scan({{0, 65536, ""}, {65537, data.size(), ""}}, reader(data), no, pattern);
    require(r.addresses.empty());
    r = memory::Scan({{0, data.size(), ""}}, reader(data), no, {0}, 4096, 3);
    require(r.partial && r.addresses.size() == 3);
    r = memory::Scan({{0, data.size(), ""}}, reader(data), no, pattern, 32768);
    require(r.partial && r.bytes == 32768 && r.addresses.empty());
    r = memory::Scan({{0, 4096, ""}}, [](auto, auto, auto) { return 0; }, no, pattern);
    require(r.partial && r.unreadable == 1);
    r = memory::Scan({{0, data.size(), ""}}, reader(data), [] { return true; }, pattern);
    require(r.partial && r.bytes == 0);
    std::string stat = "12 (name with ) parentheses) S";
    for (int i = 4; i < 22; ++i) stat += " 0";
    require(memory::StartTicks(stat + " 12345") == 12345);
}
void metadata() {
    for (unsigned version : {24, 27, 29, 31}) {
        for (unsigned stride : (version == 24 ? std::vector<unsigned>{92, 100, 104} : std::vector<unsigned>{88})) {
            std::vector<unsigned char> data(8192);
            put(data, 0, 0xfab11baf); put(data, 4, version);
            put(data, 24, 256); put(data, 28, 128);
            put(data, 160, 4096); put(data, 164, stride * 2);
            const char names[] = "Player\0Game\0Enemy\0";
            std::memcpy(data.data() + 256, names, sizeof(names));
            put(data, 4096, 0); put(data, 4100, 7); put(data, 4096 + stride - 4, 0x02000001);
            put(data, 4096 + stride, 12); put(data, 4100 + stride, 7); put(data, 4096 + stride * 2 - 4, 0x02000002);
            auto result = memory::ReadMetadata(0, reader(data), [] { return false; });
            require(result.valid && result.stride == stride && result.total == 2 && result.emitted == 2);
            require(result.report == "Game.Player\nGame.Enemy\n");
            put(data, 4096, 128);
            require(!memory::ReadMetadata(0, reader(data), [] { return false; }).valid);
            put(data, 4096, 0); put(data, 4096 + stride - 4, 0x06000001);
            require(!memory::ReadMetadata(0, reader(data), [] { return false; }).valid);
            put(data, 4096 + stride - 4, 0x02000001); put(data, 4, 99);
            require(!memory::ReadMetadata(0, reader(data), [] { return false; }).valid);
            put(data, 4, version); put(data, 28, 0xffffffff);
            require(!memory::ReadMetadata(0, reader(data), [] { return false; }).valid);
        }
    }
}
void inspection() {
    std::vector<unsigned char> data(8192);
    put(data, 256, 512);
    put(data, 512, 1024);
    put(data, 528, 2048);
    put(data, 536, 2080);
    std::memcpy(data.data() + 2048, "Player", 7);
    std::memcpy(data.data() + 2080, "Game", 5);
    put(data, 592, 4096);
    put(data, 4096, 4200); put(data, 4104, 4304); put(data, 4112, 512);
    put(data, 4120, 16); put(data, 4124, 0x04000001);
    std::memcpy(data.data() + 4200, "speed", 6); put(data, 4312, 12 << 16);
    put(data, 272, 0x3f800000);
    std::memcpy(data.data() + 280, "hello", 6);
    const auto report = memory::Inspect(256, 64, 8, {{0, data.size(), "/heap"}}, reader(data), [] { return false; }, true);
    require(report.find("Unity object-header candidate: Game.Player") != std::string::npos);
    require(report.find("f32=1") != std::string::npos && report.find("PTR\t0x200\t+0x0") != std::string::npos);
    require(report.find("ASCII candidate: hello") != std::string::npos);
    require(report.find("field +0x10 speed type=f32") != std::string::npos);
    const auto tail = memory::Inspect(8190, 64, 8, {{0, data.size(), "/heap"}}, reader(data), [] { return false; }, false);
    require(tail.find("bytes: 2/64") != std::string::npos);
    auto narrow = memory::Inspect(256, 64, 4, {{0, data.size(), "/heap"}}, reader(data), [] { return false; }, false);
    require(narrow.find("32 bits") != std::string::npos);
    bool rejected = false;
    try { memory::Inspect(9000, 64, 8, {{0, data.size(), ""}}, reader(data), [] { return false; }, false); }
    catch (const std::exception &) { rejected = true; }
    require(rejected);
}
void viewLifecycle() {
    plugin::Runtime runtime;
    int views = 0;
    RegisterMemoryScanner(runtime, [&](const MemoryJob &job, const auto &) {
        if (job.operation == "list") return monitor::CommandResult{true, "P\t123\t456\t10123\tcom.example.game\nMEMSCAN_DONE\n", "", false};
        ++views;
        if (views == 3) return monitor::CommandResult{false, "", "Denied", false};
        return monitor::CommandResult{true, views == 1 ?
            "VIEW\t0x1000\t01020304\nPTR\t0x2000\t+0x0\nMEMSCAN_DONE\n" :
            "VIEW\t0x1000\t05020304\nPTR\t0x2000\t+0x0\nMEMSCAN_DONE\n", "", false};
    });
    runtime.load("memory-scanner");
    auto run = [&](const MemoryJob &job) {
        bool accepted = false;
        runtime.emit(MemoryAction{job, &accepted});
        require(accepted);
        MemoryState result;
        for (int i = 0; i < 1000; ++i) {
            runtime.emit(MemoryQuery{&result});
            if (!result.busy) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Worker did not complete");
    };
    run({"list", "", "", 0, 0});
    auto view = run({"view", "0x1000", "256", 123, 456});
    require(view.viewHex == "01020304" && view.changed.empty() && view.pointers.size() == 1);
    view = run({"view", "0x1000", "256", 123, 456});
    require(view.changed == std::vector<unsigned>{0} && view.targetIdentity == "123:456");
    view = run({"view", "0x2000", "256", 123, 456});
    require(view.status == "failed" && view.viewHex.empty() && view.pointers.empty());
}
void lifecycle() {
    plugin::Runtime runtime;
    std::atomic<int> calls{0};
    RegisterMemoryScanner(runtime, [&](const MemoryJob &job, const std::atomic<bool> &cancel) {
        ++calls;
        if (job.operation == "list") return monitor::CommandResult{true, "P\t123\t456\t10123\tcom.example.game\nMEMSCAN_DONE\n", "", false};
        while (!cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return monitor::CommandResult{false, "", "Cancelled", false};
    });
    runtime.load("memory-scanner");
    require(calls == 0);
    bool accepted = false;
    runtime.emit(MemoryAction{{"list", "", "", 0, 0}, &accepted});
    require(accepted);
    MemoryState state;
    for (int i = 0; i < 1000; ++i) {
        runtime.emit(MemoryQuery{&state});
        if (!state.busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!state.busy && state.processes.size() == 1 && state.status == "finished");
    runtime.emit(MemoryAction{{"scan", "utf8", "needle", 123, 999}, &accepted});
    require(!accepted);
    runtime.emit(MemoryAction{{"scan", "utf8", "needle", 123, 456}, &accepted});
    require(accepted);
    runtime.emit(MemoryAction{{"list", "", "", 0, 0}, &accepted});
    require(!accepted);
    runtime.unload("memory-scanner");
    runtime.load("memory-scanner");
    runtime.emit(MemoryQuery{&state});
    require(!state.busy && state.processes.empty() && state.report.empty());
    runtime.clear();

    plugin::Runtime failed;
    RegisterMemoryScanner(failed, [](const auto &, const auto &) {
        return monitor::CommandResult{true, "P\t123\t456\t10123\tcom.example.game\n", "", false};
    });
    failed.load("memory-scanner");
    failed.emit(MemoryAction{{"list", "", "", 0, 0}, &accepted});
    for (int i = 0; i < 1000; ++i) {
        failed.emit(MemoryQuery{&state});
        if (!state.busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(state.status == "failed" && state.processes.empty());
}
}
int main() { patternsAndScan(); metadata(); inspection(); viewLifecycle(); lifecycle(); }
