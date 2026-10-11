#pragma once
#include <atomic>
#include <string>
#include <vector>
namespace vulkanscope::monitor {
struct CommandResult {
    bool success = false;
    std::string output, error;
};
/** @brief 有界执行并排空 stdout/stderr；取消与超时终止进程组并回收直接子进程。 */
CommandResult RunCommand(const std::vector<std::string> &args, const std::atomic<bool> &cancelled,
                         int timeoutMs, std::size_t limit = 32768);
}
