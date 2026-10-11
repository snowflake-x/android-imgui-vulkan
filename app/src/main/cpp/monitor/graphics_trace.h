#pragma once
#include "monitor/process.h"
#include <cstdint>
#include <functional>
#include <optional>
namespace vulkanscope::monitor {
/** @brief 从有界 ELF 文件解析已定义函数的文件偏移，供内核 uprobes 使用。 */
std::optional<std::uint64_t> GraphicsSymbolOffset(const std::string &file, const std::string &symbol);
/** @brief 解析专属 trace 实例中的呈现/提交事件；没有事件时不推测 API。 */
std::string GraphicsTraceResult(const std::string &trace);
/** @brief 在 root 工作线程中有界采样；取消后仍独立清理本次创建的实例和探针。 */
std::string TraceGraphics(const std::string &package, int user, const std::atomic<bool> &cancel,
                         const std::function<CommandResult(const std::string &, int)> &runner = {});
}
