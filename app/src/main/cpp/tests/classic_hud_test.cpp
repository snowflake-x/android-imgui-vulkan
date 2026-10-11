#include "monitor/hud_view.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace vulkanscope::monitor;
namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("Classic HUD regression");
}
/** @brief 将真实 ImGui 三角形与字体纹理光栅化，供无 Android 设备的布局审查使用。 */
void screenshot(const char *path, const ImDrawData &data, const unsigned char *atlas, int aw, int ah) {
    constexpr int w = 960, h = 600;
    std::vector<unsigned char> pixels(w * h * 3, 0);
    auto edge = [](ImVec2 a, ImVec2 b, ImVec2 p) {
        return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    };
    for (int n = 0; n < data.CmdListsCount; ++n) {
        const auto &list = *data.CmdLists[n];
        for (const auto &cmd : list.CmdBuffer) {
            if (cmd.UserCallback)
                continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const auto &a = list.VtxBuffer[list.IdxBuffer[cmd.IdxOffset + i] + cmd.VtxOffset];
                const auto &b = list.VtxBuffer[list.IdxBuffer[cmd.IdxOffset + i + 1] + cmd.VtxOffset];
                const auto &c = list.VtxBuffer[list.IdxBuffer[cmd.IdxOffset + i + 2] + cmd.VtxOffset];
                const float area = edge(a.pos, b.pos, c.pos);
                if (std::abs(area) < 1e-6f)
                    continue;
                int x0 = std::max(0, static_cast<int>(std::floor(
                                         std::max(cmd.ClipRect.x, std::min({a.pos.x, b.pos.x, c.pos.x})))));
                int y0 = std::max(0, static_cast<int>(std::floor(
                                         std::max(cmd.ClipRect.y, std::min({a.pos.y, b.pos.y, c.pos.y})))));
                int x1 = std::min(w, static_cast<int>(std::ceil(
                                         std::min(cmd.ClipRect.z, std::max({a.pos.x, b.pos.x, c.pos.x})))));
                int y1 = std::min(h, static_cast<int>(std::ceil(
                                         std::min(cmd.ClipRect.w, std::max({a.pos.y, b.pos.y, c.pos.y})))));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        ImVec2 p(x + .5f, y + .5f);
                        float u = edge(b.pos, c.pos, p) / area, v = edge(c.pos, a.pos, p) / area,
                              t = 1 - u - v;
                        if (u < 0 || v < 0 || t < 0)
                            continue;
                        int tx = std::clamp(static_cast<int>((u * a.uv.x + v * b.uv.x + t * c.uv.x) * aw), 0,
                                            aw - 1);
                        int ty = std::clamp(static_cast<int>((u * a.uv.y + v * b.uv.y + t * c.uv.y) * ah), 0,
                                            ah - 1);
                        auto channel = [&](int shift) {
                            return u * ((a.col >> shift) & 255) + v * ((b.col >> shift) & 255) +
                                   t * ((c.col >> shift) & 255);
                        };
                        float alpha = channel(IM_COL32_A_SHIFT) / 255.0f * atlas[ty * aw + tx] / 255.0f;
                        const int shifts[] = {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT};
                        for (int k = 0; k < 3; ++k) {
                            auto &out = pixels[(y * w + x) * 3 + k];
                            out = static_cast<unsigned char>(channel(shifts[k]) * alpha + out * (1 - alpha));
                        }
                    }
            }
        }
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << ' ' << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
    require(static_cast<bool>(out));
}
}
int main(int argc, char **argv) {
    HudHistory history;
    Values values{{"foreground", "game"}, {"foreground_user", "0"}, {"surface", "layer"}};
    FrameSample frames;
    frames.resetHistory = false;
    frames.fps = 60;
    frames.intervals = {10, 20, 30};
    auto sample = history.update(values, frames, 1);
    require(sample.fps.current && *sample.fps.current == 50 && *sample.present.minimum == 10 &&
            *sample.present.maximum == 30);
    require(sample.presentGraph.size() == 3 && sample.presentGraph[0] == 10 && sample.presentGraph[2] == 30);
    frames.intervals.clear();
    frames.fps = 0;
    sample = history.update(values, frames, 2);
    require(sample.fps.current == 0 && !sample.present.current && sample.presentGraph.size() == 3);
    frames.truncated = true;
    frames.intervals = {40};
    frames.fps = 25;
    sample = history.update(values, frames, 3);
    require(sample.presentGraph.size() == 5 && !sample.presentGraph[3]);
    values["surface"] = "other";
    sample = history.update(values, frames, 4);
    require(sample.presentGraph.size() == 1 && sample.present.minimum == 40);
    frames.resetHistory = true;
    frames.intervals.clear();
    frames.fps.reset();
    sample = history.update(values, frames, 5);
    require(sample.presentGraph.empty() && !sample.fps.current && !sample.gpu.current && !sample.processMb);
    frames.resetHistory = false;
    frames.fps = 60;
    frames.truncated = false;
    frames.intervals.assign(1400, 16.67);
    sample = history.update(values, frames, 6);
    require(sample.presentGraph.size() == 200 && std::abs(*sample.present.current - 16.67) < .001);
    Config config;
    for (float width : {120.f, 320.f, 1080.f}) {
        auto size = MeasureHud(config, 40, width, 200);
        require(size.width <= width && size.height <= 200 && size.fontSize > 0);
    }
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(960, 600);
    io.DeltaTime = 1.f / 60;
    require(io.Fonts->AddFontFromFileTTF(HUD_FONT_PATH, 40) != nullptr);
    unsigned char *atlas;
    int aw, ah;
    io.Fonts->GetTexDataAsAlpha8(&atlas, &aw, &ah);
    ImGui::NewFrame();
    config.opacity = 100;
    sample.values = {{"gpu_model", "Adreno 750"},
                     {"display_resolution", "2940 x 1840 px (display mode)"},
                     {"display_refresh", "60 Hz"}};
    sample.fps = {56.25, 30, 60};
    sample.present = {17.78, 16.67, 33.33};
    sample.processMb = 160.35;
    sample.presentGraph.clear();
    sample.gpuGraph.clear();
    for (int i = 0; i < 200; ++i)
        sample.presentGraph.push_back(i % 28 == 20 ? 33.33 : 16.67);
    sample.status.clear();
    DrawClassicHud(*ImGui::GetForegroundDrawList(), ImVec2(104, 50), MeasureHud(config, 40, 752, 520), config,
                   sample);
    ImGui::Render();
    require(ImGui::GetDrawData()->TotalVtxCount > 0);
    if (argc > 1)
        screenshot(argv[1], *ImGui::GetDrawData(), atlas, aw, ah);
    ImGui::DestroyContext(context);
}
