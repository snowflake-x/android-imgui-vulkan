#pragma once
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <android/native_window.h>
#include <functional>
#include <mutex>
#include <vulkan/vulkan.h>
namespace vulkanscope {
/** @brief 跨主界面与悬浮窗串行访问 ImGui 全局上下文，GPU 资源由各渲染器独立持有。 */
std::mutex &ImGuiMutex();
/** @brief 由悬浮窗线程创建及销毁；每次 Surface 生命周期拥有独立 Vulkan/ImGui 资源。 */
class OverlayRenderer {
  public:
    void initialize(ANativeWindow *window, float scale);
    void render(const std::function<void()> &draw);
    void cleanup();

  private:
    ANativeWindow *nativeWindow = nullptr;
    ImGuiContext *context = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    ImGui_ImplVulkanH_Window window;
    VkPhysicalDeviceProperties properties{};
    bool ready = false, rebuild = false, backend = false;
    double lastFrame = 0;
};
}
