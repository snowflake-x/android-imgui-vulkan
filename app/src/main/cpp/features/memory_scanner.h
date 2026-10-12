#pragma once
#include "memory/scan.h"
#include "monitor/process.h"
#include "plugin/runtime.h"
#include <atomic>
#include <functional>

namespace vulkanscope {
struct MemoryJob { std::string operation, kind, input; int pid = 0; std::uint64_t start = 0; };
using MemoryExecutor = std::function<monitor::CommandResult(const MemoryJob &, const std::atomic<bool> &)>;
struct MemoryPointer { std::string address, label; };
struct MemoryState {
    bool busy = false;
    std::string status = "idle", report;
    std::string runtimeReport;
    std::vector<memory::Process> processes;
    std::string viewAddress, viewHex, viewIdentity, targetIdentity;
    std::vector<std::string> matches;
    std::vector<unsigned> changed;
    std::vector<MemoryPointer> pointers;
};
struct MemoryAction { MemoryJob job; bool *accepted; };
struct MemoryQuery { MemoryState *snapshot; };
/** @brief Register a plugin-owned cancellable worker; loading alone never scans or requests root. */
void RegisterMemoryScanner(plugin::Runtime &runtime, MemoryExecutor execute);
}
