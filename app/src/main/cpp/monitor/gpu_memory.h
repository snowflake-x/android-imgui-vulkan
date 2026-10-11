#pragma once
#include "monitor/model.h"
#include "monitor/process.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace vulkanscope::monitor {
using GpuCommandRunner = std::function<CommandResult(const std::string &, int)>;

/** @brief 以进程启动时间、UID 和包名限定一次 GPU 采集，防止跨用户误认或 PID 复用。 */
struct GpuProcess {
    int pid = 0;
    std::uint32_t uid = 0;
    std::uint64_t start = 0;
    std::string package;
    std::string identity() const;
};

/** @brief 有界查找指定 Android 用户的主进程；取消、截断或身份不完整时返回具体错误。 */
std::optional<GpuProcess> FindGpuProcess(const std::string &package, int user,
                                       const std::atomic<bool> &cancel, std::string &error,
                                       const GpuCommandRunner &runner = {});
/** @brief 生成只读身份校验 shell 表达式；采集前后均须成功，调用方不得省略后置校验。 */
std::string GpuIdentityScript(const GpuProcess &process);
/** @brief 从 KGSL 读取主进程分配及按驱动比例归属的导入缓冲；所有数值以字节核验后转为 MB。 */
Values ReadGpuMemory(const GpuProcess &process, const std::atomic<bool> &cancel,
                     const GpuCommandRunner &runner = {});
/** @brief 为单独使用内存采集器的调用方组合进程识别和内存读取。 */
Values ReadGpuMemory(const std::string &package, int user, const std::atomic<bool> &cancel,
                     const GpuCommandRunner &runner = {});
}
