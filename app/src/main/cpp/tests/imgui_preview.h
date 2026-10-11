#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace test_preview {
/** @brief 将真实 ImGui 三角形与字体纹理光栅化，供无 Android 设备的布局审查使用。 */
inline void SavePreview(const char *path, const ImDrawData &data, const unsigned char *atlas, int aw, int ah,
                        int w = 960, int h = 600) {
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
    if (!out)
        throw std::runtime_error("Unable to save ImGui preview");
}
}
