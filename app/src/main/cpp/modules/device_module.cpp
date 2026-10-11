#include "module.h"
#include "imgui.h"

namespace vulkanscope {
namespace {
/** @brief 绘制独立的 device 信息面板，状态仅属于当前模块实例。 */
class DeviceModule final : public Module {
public:
    /** @brief 在渲染线程更新数据并绘制面板。 */
    void draw(const ModuleContext& context) override {
        if (systemReport.empty() || ImGui::GetTime() >= nextRefresh) {
            systemReport = context.readActivityText("getSystemReport");
            nextRefresh = ImGui::GetTime() + 1.0;
        }
        if (ImGui::CollapsingHeader("Screen & device", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped("%s", systemReport.c_str());
            ImGui::Text("Render surface: %d x %d px", context.surfaceWidth, context.surfaceHeight);
        }
    }
private:
    std::string systemReport;
    double nextRefresh = 0;
};
}
std::unique_ptr<Module> createDeviceModule() { return std::make_unique<DeviceModule>(); }
}
