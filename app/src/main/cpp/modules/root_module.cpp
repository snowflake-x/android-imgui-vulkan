#include "module.h"
#include "imgui.h"

namespace vulkanscope {
namespace {
/** @brief 绘制独立的 root 信息面板，状态仅属于当前模块实例。 */
class RootModule final : public Module {
public:
    /** @brief 在渲染线程更新数据并绘制面板。 */
    void draw(const ModuleContext& context) override {
        if (rootReport.empty() || ImGui::GetTime() >= nextRefresh) {
            rootReport = context.readActivityText("getRootReport");
            nextRefresh = ImGui::GetTime() + 1.0;
        }
        if (ImGui::CollapsingHeader("Optional root diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped("Requires an existing su provider (e.g. Magisk). Grant access on your device. Reads kernel, CPU, memory, SELinux and thermal data.");
            if (ImGui::Button("Request root / refresh", ImVec2(-1, ImGui::GetFontSize()*2.5f))) {
                context.callActivity("requestRootInfo"); rootReport = context.readActivityText("getRootReport");
            }
            ImGui::TextWrapped("%s", rootReport.c_str());
        }
    }
private:
    std::string rootReport;
    double nextRefresh = 0;
};
}
std::unique_ptr<Module> createRootModule() { return std::make_unique<RootModule>(); }
}
