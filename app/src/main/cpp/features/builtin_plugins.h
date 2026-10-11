#pragma once

#include "plugin/runtime.h"
#include "ui/i18n.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace vulkanscope {

/** @brief 宿主按帧提供的值快照；插件不持有 Android、JNI 或 Vulkan 句柄。 */
struct RenderFrame {
    float deltaSeconds = 0;
    float framerate = 0;
    double time = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    std::string gpuName;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t imageCount = 0;
    ui::Language language = ui::Language::English;
};

/** @brief 只读报告能力；JNI 实现和生命周期均归宿主所有。 */
class HostReports final {
public:
    explicit HostReports(std::function<std::string()> readSystemReport)
        : readSystemReport_(std::move(readSystemReport)) {}

    /** @brief 仅在宿主渲染线程同步读取系统报告，不触发 root 请求。 */
    std::string systemReport() const { return readSystemReport_(); }

private:
    const std::function<std::string()> readSystemReport_;
};

/** @brief 登记内置插件定义；登记不会加载插件或访问宿主服务。 */
void RegisterBuiltinPlugins(plugin::Runtime& runtime, std::shared_ptr<HostReports> reports);

}
