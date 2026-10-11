#include "module.h"
#include "imgui.h"

namespace vulkanscope {
namespace {
/** @brief 绘制独立的 gpu 信息面板，状态仅属于当前模块实例。 */
class GpuModule final : public Module {
public:
    /** @brief 在渲染线程更新数据并绘制面板。 */
    void draw(const ModuleContext& context) override {
        if (ImGui::CollapsingHeader("Vulkan GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped("%s", context.gpu.deviceName);
            ImGui::Text("API: %u.%u.%u", VK_VERSION_MAJOR(context.gpu.apiVersion), VK_VERSION_MINOR(context.gpu.apiVersion), VK_VERSION_PATCH(context.gpu.apiVersion));
            ImGui::Text("Driver (raw): 0x%08x", context.gpu.driverVersion);
            ImGui::Text("Vendor / device: %04x / %04x", context.gpu.vendorID, context.gpu.deviceID);
            ImGui::Text("Swapchain: %u images / FIFO", context.imageCount);
        }
    }
};
}
std::unique_ptr<Module> createGpuModule() { return std::make_unique<GpuModule>(); }
}
