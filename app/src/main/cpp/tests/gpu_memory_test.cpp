#include "monitor/gpu_memory.h"
#include <iostream>
#include <stdexcept>

using namespace vulkanscope::monitor;
namespace {
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
const GpuProcess process{123, 10001, 456, "com.game"};
const std::string source = "/sys/class/kgsl/kgsl/proc/123";
const std::string debug = "/sys/kernel/debug/kgsl/proc/123/mem";
std::atomic<bool> cancel{false};

std::string identity(const std::string &pid = "123", const std::string &uid = "10001",
                     const std::string &start = "456", const std::string &package = "com.game") {
    return "PID=" + pid + "\nUID=" + uid + "\nSTART=" + start + "\nCMD=" + package + "\nEND\n";
}
std::string scalar(const std::string &kernel, const std::string &imported) {
    return "SOURCE=" + source + "\nkernel=" + kernel + "\nimported_mem=" + imported + "\nEND\nMEMORY_COMPLETE\n";
}
const std::string tableHeader = "gpuaddr useraddr size id flags type usage sglen mapcnt eglsrf eglimg inode\n";

Values sample(const std::string &scalars, const std::string &table = "UNAVAILABLE\nMEMORY_COMPLETE\n") {
    int calls = 0;
    return ReadGpuMemory(process, cancel, [&](const std::string &script, int timeout) -> CommandResult {
        require(timeout == 3000, "GPU reads have a bounded timeout");
        const auto check = GpuIdentityScript(process);
        require(script.find(check) == 0 && script.rfind(check) > 0, "identity checked before and after reads");
        require(script.find("gpumem_mapped") == std::string::npos, "CPU mapped subset is never added");
        require(RunCommand({"sh", "-n", "-c", script}, cancel, 1000).success, "valid read shell syntax");
        return {true, ++calls == 1 ? scalars : table, {}};
    });
}

void processIdentity() {
    std::string error;
    auto find = [&](const std::string &output) {
        return FindGpuProcess("com.game", 0, cancel, error,
            [&](const std::string &script, int timeout) -> CommandResult {
                require(script.find("[ \"$n\" -le 16 ]") != std::string::npos, "bounded PID discovery");
                require(timeout == 3000, "bounded discovery timeout");
                require(RunCommand({"sh", "-n", "-c", script}, cancel, 1000).success, "valid discovery shell syntax");
                return {true, output, {}};
            });
    };
    auto found = find(identity("124", "110001") + identity() + "PROCESS_COMPLETE\n");
    require(found && found->identity() == "com.game:10001:123:456", "choose main process from the correct Android user");
    for (const auto &output : {identity("0"), identity("-1"), identity("2147483648"), identity("123", "4294967296"),
                              identity("123", "10001", "18446744073709551616"),
                              identity("123", "10001", "1x"), identity("123", "10001", "0"),
                              identity("123", "10001", "456", "com.game:worker")})
        require(!find(output + "PROCESS_COMPLETE\n"), "reject malformed, recycled, and non-main process identities");
    require(!find(identity()), "identity completion marker required");
    require(!find("PID=123\nPID=123\nUID=10001\nSTART=456\nCMD=com.game\nEND\nPROCESS_COMPLETE\n"),
            "duplicate identity fields rejected");
    bool called = false;
    require(!FindGpuProcess("com.game;id", 0, cancel, error,
        [&](const std::string &, int) { called = true; return CommandResult{}; }) && !called,
        "invalid package never reaches shell");
    auto bad = process;
    bad.package = "$(id)";
    require(GpuIdentityScript(bad) == "false", "identity script rejects unsafe caller input");
}

void scalarMemory() {
    auto value = sample(scalar("3000000", "1250000"));
    require(value.at("gpu_memory_mb") == "4.25", "bytes convert to decimal MB without multiplying shared memory");
    require(value.at("gpu_identity") == process.identity(), "GPU memory carries stable process identity");
    require(value.at("gpu_memory_source").find("proportional DMA-BUF") != std::string::npos,
            "measurement scope identifies proportional imported buffers");
    require(sample(scalar("0", "0")).at("gpu_memory_mb") == "0.00", "zero allocations are valid measurements");
    for (const auto &invalid : {"-1", "+1", "1 MB", "1.5", "1e6", "nan", "18446744073709551616", "", "1\n2"})
        require(!sample(scalar("1", invalid)).count("gpu_memory_mb"), "invalid counter is not interpreted as a measurement");
    require(!sample(scalar("18446744073709551615", "1")).count("gpu_memory_mb"), "sum overflow rejected");
    auto partial = sample("SOURCE=" + source + "\nkernel=4000000\nEND\nMEMORY_COMPLETE\n");
    require(!partial.count("gpu_memory_mb") && partial.at("gpu_memory").find("allocation-only 4.00 MB") != std::string::npos,
            "allocation-only counter is explicitly partial and cannot feed the total HUD number");
    const auto duplicate = "SOURCE=" + source + "\nkernel=1\nkernel=2\nimported_mem=3\nEND\nMEMORY_COMPLETE\n";
    require(!sample(duplicate).count("gpu_memory_mb"), "duplicate scalar fields rejected");
    require(!sample(scalar("100", "200").substr(0, 40)).count("gpu_memory_mb"), "truncated sysfs results rejected");
    const auto alias = "SOURCE=/sys/class/kgsl/kgsl-3d0/proc/123\nkernel=1000000\nimported_mem=2000000\nEND\n";
    require(sample(alias + scalar("9000000", "9000000")).at("gpu_memory_mb") == "3.00",
            "alternative KGSL paths are alternatives, never summed");
}

void debugfsMemory() {
    const auto wrap = [&](const std::string &rows) { return "SOURCE=" + debug + "\n" + tableHeader + rows + "MEMORY_COMPLETE\n"; };
    const std::string rows =
        "0 0 4000000 1 --wl-pY--- gpumem texture 1 2 0 0 0\n"
        "0 0 6000000 2 --wl-pN--- ion texture 1 0 1 2 22\n"
        "0 0 9000000 3 --wl-pN--- usermem texture 1 0 0 0 0\n"
        "0 0 800000000 4 --wl-pN--v gpumem texture 1 0 0 0 0\n";
    auto value = sample("MEMORY_COMPLETE\n", wrap(rows));
    require(value.at("gpu_memory_mb") == "6.00", "debugfs counts allocations and proportional imports, excludes CPU imports and virtual reservations");
    require(value.at("gpu_memory_source").find(debug) == 0, "fallback identifies actual source");
    require(sample("MEMORY_COMPLETE\n", wrap("")).at("gpu_memory_mb") == "0.00", "valid empty allocation table is zero");
    require(!sample("MEMORY_COMPLETE\n", wrap(rows + rows)).count("gpu_memory_mb"), "duplicate entry IDs rejected");
    require(!sample("MEMORY_COMPLETE\n", wrap("0 0 4 1 --wl-pY--- unknown texture 1 0 0 0 0\n")).count("gpu_memory_mb"),
            "unknown allocation type cannot silently undercount");
    require(!sample("MEMORY_COMPLETE\n", wrap("0 0 4 1 --wl-pY--- ion texture 1 0 18446744073709551615 1 0\n")).count("gpu_memory_mb"),
            "import reference count overflow rejected");
    require(!sample("MEMORY_COMPLETE\n", wrap("0 0 4 1 --wl-pY--- gpumem\n")).count("gpu_memory_mb"), "partial allocation row rejected");
    require(!sample("MEMORY_COMPLETE\n", wrap(std::string(196609, ' '))).count("gpu_memory_mb"), "bounded table cannot silently drop allocations");
}

void lifecycle() {
    for (const auto &result : {CommandResult{false, scalar("1", "2"), "process changed"},
                               CommandResult{true, scalar("1", "2"), {}, true},
                               CommandResult{true, std::string(262145, ' '), {}}}) {
        auto value = ReadGpuMemory(process, cancel, [&](const std::string &, int) { return result; });
        require(!value.count("gpu_memory_mb"), "identity failure and output truncation invalidate all counters");
    }
    cancel = true;
    bool called = false;
    auto value = ReadGpuMemory(process, cancel, [&](const std::string &, int) { called = true; return CommandResult{}; });
    require(!called && !value.count("gpu_memory_mb"), "cancelled collection never launches root");
    cancel = false;
    value = ReadGpuMemory(process, cancel, [&](const std::string &, int) {
        cancel = true;
        return CommandResult{true, scalar("1", "2"), {}};
    });
    require(!value.count("gpu_memory_mb"), "cancellation during read drops the result");
    cancel = false;
}
}

int main() {
    try {
        processIdentity(); scalarMemory(); debugfsMemory(); lifecycle();
        std::cout << "GPU memory tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
