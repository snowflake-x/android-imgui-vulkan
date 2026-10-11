#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>

namespace vulkanscope {
/**
 * @brief 单帧只读快照及 Android 服务入口，不向模块暴露 Vulkan 资源所有权。
 * @note 仅在 draw 调用期间有效；模块不得保存引用或跨线程调用服务。
 */
struct ModuleContext {
    const VkPhysicalDeviceProperties& gpu; ///< 当前 GPU 的只读属性。
    int surfaceWidth; ///< 渲染表面宽度，单位为像素。
    int surfaceHeight; ///< 渲染表面高度，单位为像素。
    uint32_t imageCount; ///< 交换链图像数量。
    std::string (*readActivityText)(const char*); ///< 同步读取 Activity 文本快照。
    void (*callActivity)(const char*); ///< 调用无参数 Activity 方法；耗时工作由平台层异步执行。
};

/** @brief 可扩展的信息面板；实例在 Surface 初始化时创建，销毁时释放。 */
class Module {
public:
    /** @brief 释放模块拥有的 CPU 状态，不依赖 ImGui 或 Vulkan 上下文。 */
    virtual ~Module() = default;
    /**
     * @brief 在已开始的 ImGui 窗口内更新并绘制模块，不得阻塞渲染线程。
     * @param context 当前帧的只读渲染信息和平台服务。
     * @note 仅由渲染线程调用，必须配对模块内部的 ImGui Begin/End 操作。
     */
    virtual void draw(const ModuleContext& context) = 0;
};

/** @brief 创建屏幕与设备信息模块。 */
std::unique_ptr<Module> createDeviceModule();
/** @brief 创建 Vulkan GPU 信息模块。 */
std::unique_ptr<Module> createGpuModule();
/** @brief 创建可选 root 诊断模块。 */
std::unique_ptr<Module> createRootModule();
}
