#pragma once

#include "plugin/runtime.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace vulkanscope {

/**
 * @brief 宿主管理的前台 HUD 会话能力；插件只同步读取快照和转发用户动作。
 * @note Android 服务拥有采集会话，插件或图形表面销毁不会停止用户启动的会话。
 */
class HostPerformanceHud final {
public:
    HostPerformanceHud(std::function<std::string()> readReport,
                       std::function<void()> configure,
                       std::function<void()> start,
                       std::function<void()> stop)
        : readReport_(std::move(readReport)), configure_(std::move(configure)),
          start_(std::move(start)), stop_(std::move(stop)) {}

    /**
     * @brief 返回最近的采集快照；不启动服务、采集任务或权限请求。
     */
    std::string report() const { return readReport_(); }

    /**
     * @brief 响应用户点击，打开宿主的 HUD 配置界面。
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
};

/**
 * @brief 登记 HUD 宿主服务和界面插件；登记与加载都不会访问 Android 能力。
 */
void RegisterPerformanceHudPlugin(plugin::Runtime& runtime,
                                  std::shared_ptr<HostPerformanceHud> host);

}
