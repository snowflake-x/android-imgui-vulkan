#pragma once

#include "plugin/runtime.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace vulkanscope {

/**
 * @brief 宿主管理的原生 HUD 会话能力；插件同步读取快照并绘制原生配置。
 * @note C++ 会话由 Android 服务生命周期桥接持有，插件或图形表面销毁不会停止用户启动的会话。
 */
class HostPerformanceHud final {
public:
    HostPerformanceHud(std::function<std::string()> readReport,
                       std::function<void()> configure,
                       std::function<void()> start,
                       std::function<void()> stop,
                       std::function<void()> drawPreview = {})
        : readReport_(std::move(readReport)), configure_(std::move(configure)),
          start_(std::move(start)), stop_(std::move(stop)), drawPreview_(std::move(drawPreview)) {}

    /**
     * @brief 返回最近的采集快照；不启动服务、采集任务或权限请求。
     */
    std::string report() const { return readReport_(); }

    /** @brief 绘制宿主提供的实时预览；未提供时由插件显示文本报告。 */
    bool preview() const { if (!drawPreview_) return false; drawPreview_(); return true; }

    /**
     * @brief 在当前 ImGui 帧内绘制已展开的原生 HUD 配置。
     */
    void configure() const { configure_(); }

    /**
     * @brief 响应用户点击，由宿主检查权限并启动持久采集会话。
     */
    void start() const { start_(); }

    /**
     * @brief 响应用户点击，请求宿主停止采集会话及悬浮窗。
     */
    void stop() const { stop_(); }

private:
    const std::function<std::string()> readReport_;
    const std::function<void()> configure_;
    const std::function<void()> start_;
    const std::function<void()> stop_;
    const std::function<void()> drawPreview_;
};

/**
 * @brief 登记 HUD 宿主服务和界面插件；登记与加载都不会访问 Android 能力。
 */
void RegisterPerformanceHudPlugin(plugin::Runtime& runtime,
                                  std::shared_ptr<HostPerformanceHud> host);

}
