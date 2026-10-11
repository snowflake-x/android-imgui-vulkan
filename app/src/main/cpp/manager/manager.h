#pragma once
#include "monitor/session.h"
#include "features/memory_scanner.h"
#include "plugin/runtime.h"
#include "ui/i18n.h"

namespace vulkanscope {
/** @brief Kotlin 宿主的原生插件和配置入口；所有方法只由宿主的同一个串行工作线程调用。 */
class Manager {
public:
    explicit Manager(monitor::Session &session, MemoryExecutor memory = {});
    void pauseMemory();
    std::string snapshot(ui::Language language);
    bool command(const std::string &key, const std::string &value);
    std::string diagnostic(const std::string &package, int user) const;
private:
    void synchronizePlugins();
    monitor::Session &session_;
    plugin::Runtime runtime_;
};
/** @brief 将 UTF-8 文本编码为 JSON 字符串，保留诊断中的换行和控制字符边界。 */
std::string JsonString(const std::string &value);
}
