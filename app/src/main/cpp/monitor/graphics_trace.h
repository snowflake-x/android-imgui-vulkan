#pragma once
#include "monitor/process.h"
#include "monitor/model.h"
#include <cstdint>
#include <functional>
#include <optional>
namespace vulkanscope::monitor {
/** @brief 判断可探测的系统加载器、厂商驱动与 ANGLE 库名称。 */
bool GraphicsProbeLibrary(const std::string &name);
/** @brief 返回进程 maps 中已加载的 API 库类别；不表示观察到实际调用。 */
std::string GraphicsMappedLibraries(const std::string &maps);
/** @brief 从有界 ELF 文件解析已定义函数的文件偏移，供内核 uprobes 使用。 */
std::optional<std::uint64_t> GraphicsSymbolOffset(const std::string &file, const std::string &symbol);
/** @brief 解析专属 trace 实例中的呈现/提交事件；没有事件时不推测 API。 */
std::string GraphicsTraceResult(const std::string &trace);
/**
 * @brief 在 root 工作线程中有界采样；取消后仍独立清理本次创建的实例和探针。
 * @param diagnostics 可选输出 graphics_api_status 和有界 graphics_api_details，不替代实际调用证据。
 */
std::string TraceGraphics(const std::string &package, int user, const std::atomic<bool> &cancel,
                         const std::function<CommandResult(const std::string &, int)> &runner = {},
                         Values *diagnostics = nullptr);
}
