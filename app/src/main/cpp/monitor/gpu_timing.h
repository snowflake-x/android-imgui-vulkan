#pragma once
#include "monitor/gpu_memory.h"

namespace vulkanscope::monitor {
/** @brief 硬件耗时采集结果；清理失败时宿主必须停止后续跟踪，避免遗留实例继续运行。 */
struct GpuTimingResult {
    Values values;
    bool cleanupFailed = false;
};
/** @brief 验证有界采样协议、事件格式、进程归属和丢样计数，拒绝不完整的硬件数据。 */
GpuTimingResult ParseGpuTimingCapture(const GpuProcess &process, const CommandResult &capture);
/**
 * @brief 在专属 tracefs 实例采集目标进程的 KGSL 批次耗时，或同 UID 的 GPU 工作窗口。
 * @note 批次单位为 ms/batch，工作窗口为 ms/window；两者均不是逐帧 GPU 耗时。
 */
GpuTimingResult ReadGpuTiming(const GpuProcess &process, const std::atomic<bool> &cancel,
                              const GpuCommandRunner &runner = {});
}
