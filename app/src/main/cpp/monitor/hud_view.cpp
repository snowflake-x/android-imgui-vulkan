#include "monitor/hud_view.h"
#include <algorithm>
#include <cmath>

namespace vulkanscope::monitor {
namespace {
const ImU32 white = IM_COL32(246, 246, 246, 255);
const ImU32 blue = IM_COL32(97, 158, 211, 255);
const ImU32 green = IM_COL32(151, 216, 170, 255);
const ImU32 red = IM_COL32(244, 153, 147, 255);
const ImU32 dim = IM_COL32(155, 157, 163, 255);
std::string value(const HudSnapshot &s, const char *id) {
    auto it = s.values.find(id);
    return it == s.values.end() ? std::string{} : it->second;
}
std::string number(const std::optional<double> &n) { return n ? Format(*n, 2) : "--"; }
}
void DrawClassicHud(ImDrawList &draw, ImVec2 origin, const HudSize &size, const Config &config,
                    const HudSnapshot &s, ImFont *font) {
    if (!font)
        font = ImGui::GetFont();
    const float f = size.fontSize, pad = .36f * f, left = origin.x + pad, right = origin.x + size.width - pad;
    const float line = 1.23f * f;
    draw.PushClipRect(origin, ImVec2(origin.x + size.width, origin.y + size.height), true);
    draw.AddRectFilled(origin, ImVec2(origin.x + size.width, origin.y + size.height),
                       IM_COL32(0, 0, 0, static_cast<int>(config.opacity * 2.55f)));
    auto width = [&](const std::string &text, float scale = 1.0f) {
        return font->CalcTextSizeA(f * scale, 1e6f, 0, text.c_str()).x;
    };
    auto text = [&](float x, float y, const std::string &string, ImU32 color, float scale = 1.0f) {
        draw.AddText(font, f * scale, ImVec2(x, y), color, string.c_str());
    };
    auto fit = [&](std::string string, float available, float scale = 1.0f) {
        if (width(string, scale) <= available)
            return string;
        while (!string.empty() && width(string + "...", scale) > available)
            string.pop_back();
        return string + "...";
    };
    float y = origin.y + pad;
    std::string gpuName = config.has("gpu_model") ? value(s, "gpu_model") : "GPU: OFF";
    if (gpuName.empty())
        gpuName = "GPU unavailable";
    std::string dimensions = value(s, "display_resolution");
    const auto end = dimensions.find(" px");
    if (end != std::string::npos)
        dimensions.resize(end);
    dimensions.erase(std::remove(dimensions.begin(), dimensions.end(), ' '), dimensions.end());
    if (dimensions.empty() || dimensions.rfind("N/A", 0) == 0)
        dimensions = "--x--";
    dimensions = "[" + dimensions + "]";
    const float headerRight = right - width(dimensions);
    text(left, y, fit(gpuName, std::max(f, headerRight - left - f)), white);
    text(headerRight, y, dimensions, white);
    y += line;
    auto hz = value(s, "display_refresh");
    if (hz.empty())
        hz = "-- Hz";
    hz.erase(std::remove(hz.begin(), hz.end(), ' '), hz.end());
    const auto api = GraphicsApiCaption(config, s);
    text(left, y, fit(api, right - width(hz) - left - f), white);
    text(right - width(hz), y, hz, white);
    y += line;
    /** @brief 固定单元格右对齐，与数值长度无关；仅异常的边界值变红。 */
    auto row = [&](const char *label, const HudRange &values, ImU32 color, bool fps, bool enabled) {
        const float currentRight = left + 7.5f * f;
        const float bracket = left + 8.3f * f;
        const float minRight = left + 13.0f * f;
        const float maxRight = right - .6f * f;
        const auto current = enabled ? number(values.current) : "OFF", minimum = number(values.minimum),
                   maximum = number(values.maximum);
        text(left, y, label, color);
        text(currentRight - width(current), y, current, color);
        text(bracket, y, "[", color);
        const bool low = fps && values.minimum && values.current && *values.minimum < *values.current * .8;
        const bool high = !fps && values.maximum && values.current && *values.maximum > *values.current * 1.2;
        text(minRight - width(minimum), y, minimum, low ? red : color);
        text(maxRight - width(maximum), y, maximum, high ? red : color);
        text(right - width("]"), y, "]", color);
        y += line;
    };
    row("FPS:", config.has("fps") ? s.fps : HudRange{}, white, true, config.has("fps"));
    row("Pre:", config.has("frame_time") ? s.present : HudRange{}, blue, false, config.has("frame_time"));
    row("GPU:", config.has("gpu_time") ? s.gpu : HudRange{}, green, false, config.has("gpu_time"));
    text(left, y, "Mem:", white);
    const auto memory = config.has("app_memory") ? number(s.processMb) + "MB" : "OFF";
    text(left + 8.8f * f - width(memory), y, memory, white);
    const auto gpuMemory = config.has("gpu_memory") ? "[" + number(s.gpuMb) + "MB]" : "[OFF]";
    text(right - width(gpuMemory), y, gpuMemory, green);
    y += line + .24f * f;
    if (config.hudGraph) {
        const float top = y, bottom = y + 3.35f * f, height = bottom - top;
        const float thickness = std::max(1.0f, .065f * f);
        for (int i = 0; i <= 10; ++i) {
            const float x = left + (right - left) * i / 10;
            draw.AddLine(ImVec2(x, top), ImVec2(x, bottom), white,
                         i == 0 || i == 10 ? thickness : std::max(1.0f, .035f * f));
        }
        draw.AddLine(ImVec2(left, bottom), ImVec2(right, bottom), white, thickness);
        double ceiling = 40;
        if (config.has("frame_time"))
            for (const auto &n : s.presentGraph)
                if (n && std::isfinite(*n))
                    ceiling = std::max(ceiling, *n * 1.2);
        auto plot = [&](const std::vector<std::optional<double>> &graph, ImU32 color) {
            std::optional<ImVec2> previous;
            const auto count = std::min<std::size_t>(graph.size(), 200);
            for (std::size_t i = 0; i < count; ++i) {
                const auto &n = graph[graph.size() - count + i];
                if (!n || !std::isfinite(*n) || *n < 0) {
                    previous.reset();
                    continue;
                }
                ImVec2 p(left + (right - left) * (200 - count + i) / 199.0f,
                         bottom - static_cast<float>(std::min(*n / ceiling, 1.0)) * height);
                if (previous)
                    draw.AddLine(*previous, p, color, std::max(1.0f, .085f * f));
                previous = p;
            }
        };
        if (config.has("frame_time"))
            plot(s.presentGraph, blue);
        y = bottom + .18f * f;
    }
    for (const auto &caption : HudCaptions(config, s)) {
        text(left, y, fit(caption, right - left, .5f), dim, .5f);
        y += .62f * f;
    }
    if (config.hudExtras) {
        for (const auto &id : config.order) {
            if (!config.has(id) || id == "fps" || id == "frame_time" || id == "gpu_model" ||
                id == "app_memory" || id == "gpu_time" || id == "gpu_memory" || id == "engine_name")
                continue;
            const auto metric =
                std::find_if(Metrics().begin(), Metrics().end(), [&](const auto &m) { return id == m.id; });
            if (metric == Metrics().end())
                continue;
            auto field = value(s, id.c_str());
            if (field.empty())
                field = "N/A";
            text(left, y, fit(std::string(metric->label) + ": " + field, right - left, .55f), dim, .55f);
            y += .7f * f;
        }
    }
    draw.PopClipRect();
}
void DrawHud(ImDrawList &draw, ImVec2 origin, const HudSize &size, const Config &config,
             const HudSnapshot &snapshot, ImFont *font) {
    if (config.hudStyle != "rtss") {
        DrawClassicHud(draw, origin, size, config, snapshot, font);
        return;
    }
    if (!font) font = ImGui::GetFont();
    const float f = size.fontSize, pad = .4f * f;
    const float left = origin.x + pad, right = origin.x + size.width - pad;
    float y = origin.y + pad;
    const ImU32 orange = IM_COL32(255, 173, 65, 255);
    const ImU32 lime = IM_COL32(148, 237, 107, 255);
    const ImU32 cyan = IM_COL32(89, 218, 242, 255);
    const ImU32 purple = IM_COL32(224, 149, 244, 255);
    draw.PushClipRect(origin, ImVec2(origin.x + size.width, origin.y + size.height), true);
    draw.AddRectFilled(origin, ImVec2(origin.x + size.width, origin.y + size.height),
                       IM_COL32(0, 0, 0, static_cast<int>(config.opacity * 2.55f)));
    auto text = [&](float x, std::string content, ImU32 color, float scale) {
        const float pixels = f * scale;
        auto width = [&](const std::string &s) { return font->CalcTextSizeA(pixels, 1e6f, 0, s.c_str()).x; };
        if (width(content) > right - x) {
            /** @brief 按完整 UTF-8 码点截断，避免国际化系统字段产生无效字节序列。 */
            while (!content.empty() && width(content + "...") > right - x) {
                auto end = content.size() - 1;
                while (end > 0 && (static_cast<unsigned char>(content[end]) & 0xc0) == 0x80) --end;
                content.resize(end);
            }
            content += "...";
        }
        const float shadow = std::max(1.0f, .045f * f);
        draw.AddText(font, pixels, ImVec2(x + shadow, y + shadow), IM_COL32(0, 0, 0, 230), content.c_str());
        draw.AddText(font, pixels, ImVec2(x, y), color, content.c_str());
    };
    for (const auto &row : RtssRows(config, snapshot)) {
        const ImU32 color = row.group == "gpu" || row.group == "frame" ? orange :
            row.group == "cpu" ? lime : row.group == "memory" ? cyan : row.group == "api" ? purple : dim;
        if (row.group == "detail") text(left, row.label + ": " + row.text, color, row.scale);
        else {
            text(left, row.label, color, row.scale);
            text(row.label.empty() ? left : left + 5.6f * f, row.text, color, row.scale);
        }
        y += row.scale * 1.2f * f;
    }
    if (config.hudGraph) {
        const float top = y + .15f * f, bottom = y + 2.35f * f;
        draw.AddRect(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(150, 110, 60, 150));
        double ceiling = 33.33;
        if (config.has("frame_time"))
            for (const auto &n : snapshot.presentGraph)
                if (n && std::isfinite(*n)) ceiling = std::max(ceiling, *n * 1.1);
        std::optional<ImVec2> previous;
        const auto count = std::min<std::size_t>(200, snapshot.presentGraph.size());
        if (config.has("frame_time")) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto &n = snapshot.presentGraph[snapshot.presentGraph.size() - count + i];
                if (!n || !std::isfinite(*n) || *n < 0) { previous.reset(); continue; }
                const ImVec2 point(left + (right - left) * (200 - count + i) / 199.0f,
                    bottom - static_cast<float>(std::min(*n / ceiling, 1.0)) * (bottom - top));
                if (previous) draw.AddLine(*previous, point, orange, std::max(1.0f, .06f * f));
                previous = point;
            }
        }
        y += 2.6f * f;
    }
    const auto captions = HudCaptions(config, snapshot);
    for (std::size_t i = 0; i < captions.size(); ++i) {
        if (i == 2) continue; /** @brief GPU 单位和共享内存含义已在 RTSS 主行显示。 */
        text(left, captions[i], dim, .55f);
        y += .65f * f;
    }
    draw.PopClipRect();
}

}
