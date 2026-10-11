#include "imgui_internal.h"
#include "imgui_preview.h"
#include "monitor/hud_view.h"
#include "platform/system_fonts.h"
#include "ui/font_catalog.h"
#include "ui/navigation.h"
#include "ui/theme.h"
#include "features/builtin_plugins.h"
#include "features/performance_hud.h"
#include "ui/widgets.h"
#include <cmath>
#include <stdexcept>
#include <string>
using namespace vulkanscope;
namespace {
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
}
int main(int argc, char **argv) {
    using namespace ui;
    require(ResolveLanguage("system", "zh-CN,en-US") == Language::Chinese, "system Chinese locale");
    require(ResolveLanguage("en", "zh-CN") == Language::English, "explicit language override");
    require(ResolveLanguage("system", "fr-FR") == Language::English, "unsupported locale falls back");
    require(std::string(Text(Language::Chinese, "Home")) == "主页", "Chinese catalog");
    require(std::string(Text(Language::Chinese, "unknown")) == "unknown", "missing key falls back");
    require(TranslateReport(Language::Chinese, "Device: example\nCPU cores: 8") ==
                "设备: example\nCPU 核心数: 8",
            "values preserved");
    auto fonts = ParseFontConfig(R"(<familyset><alias name="sans-serif" to="custom"/>
      <family name="serif"><font>ignore.ttf</font></family>
      <family name="custom"><font weight="700">bold.ttf</font><font weight="400">regular.ttf</font></family>
      <family lang="ja"><font index="0">cjk.ttc</font></family>
      <family lang="zh-Hans"><font index="2">cjk.ttc</font></family>
      <family><font>../escape.ttf</font></family></familyset>)",
                                 "/system/fonts", "zh-Hans");
    require(fonts.size() == 3 && fonts[0].path == "/system/fonts/regular.ttf" && fonts[1].index == 2,
            "system family, weight, locale and TTC index");
    require(ParseFontConfig("<broken>", "/system/fonts", "en").empty(), "malformed XML rejected");
    auto *context = ImGui::CreateContext();
    ApplyTheme(2);
    const float scaledPadding = ImGui::GetStyle().WindowPadding.x;
    ApplyTheme(2);
    require(ImGui::GetStyle().WindowPadding.x == scaledPadding, "theme scaling is idempotent");
    ApplyTheme();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(420, 800);
    io.DeltaTime = 1.f / 60;
    SystemFonts fontState;
    require(fontState.load(20, "zh-Hans", TranslationCorpus(), {{TEST_LATIN_FONT, 0}, {TEST_CJK_FONT, 2}}),
            "load host system fonts");
    require(!fontState.needsGlyphs("主页插件列表"), "known glyphs do not reload");
    require(MergeGlyphCorpus("中文𠀀", "中文𠀀") == MergeGlyphCorpus("", "中文𠀀"),
            "Unicode corpus deduplicates supplementary glyphs");
    require(fontState.needsGlyphs("龘"), "unseen glyph schedules rebuild");
    require(io.Fonts->Build(), "build merged atlas");
    ImFontGlyphRangesBuilder required;
    required.AddText(TranslationCorpus().c_str());
    ImVector<ImWchar> ranges;
    required.BuildRanges(&ranges);
    for (int i = 0; ranges[i]; i += 2)
        for (unsigned c = ranges[i]; c <= ranges[i + 1]; ++c)
            require(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(c)) != nullptr,
                    "catalog must have complete glyph coverage");
    Navigation state;
    int homes = 0, plugins = 0;
    auto frame = [&](Language language) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("shell", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse);
        require(ImGui::GetID(Label(Language::English, "Home").c_str()) ==
                    ImGui::GetID(Label(Language::Chinese, "Home").c_str()),
                "locale-independent IDs");
        DrawPages(
            state, language,
            [&] {
                ++homes;
                for (int i = 0; i < 90; ++i)
                    ImGui::Text("row %d", i);
            },
            [&] {
                ++plugins;
                ImGui::TextUnformatted("plugin list");
            });
        require(state.tabMin[0].y > 0 && state.tabMax[1].y <= io.DisplaySize.y,
                "fixed bottom tabs stay inside viewport");
        ImGui::End();
        ImGui::Render();
    };
    frame(Language::English);
    frame(Language::Chinese);
    require(homes == 2 && plugins == 0, "only selected page renders");
    const auto p = state.tabMin[1], q = state.tabMax[1];
    io.AddMousePosEvent((p.x + q.x) / 2, (p.y + q.y) / 2);
    frame(Language::Chinese);
    io.AddMouseButtonEvent(0, true);
    frame(Language::Chinese);
    io.AddMouseButtonEvent(0, false);
    frame(Language::Chinese);
    frame(Language::Chinese);
    require(state.selected == Page::Plugins && plugins > 0, "bottom navigation responds to input");
    io.DisplaySize = ImVec2(800, 420);
    frame(Language::English);
    require(state.selected == Page::Plugins, "rotation and language preserve selection");
    if (argc > 1) {
        io.DisplaySize = ImVec2(420, 800);
        plugin::Runtime previewPlugins;
        RegisterBuiltinPlugins(previewPlugins, std::make_shared<HostReports>([] {
            return "Device: Android\nDisplay: 1440 x 3200\nRefresh: 120 Hz";
        }));
        RegisterPerformanceHudPlugin(previewPlugins, std::make_shared<HostPerformanceHud>(
            [] { return "Preview sample"; }, [] {}, [] {}, [] {}, [] {
                monitor::Config config;
                monitor::HudSnapshot sample;
                sample.values = {{"gpu_model", "Adreno 750"}, {"display_resolution", "1440 x 3200 px"},
                                 {"display_refresh", "120 Hz"}};
                sample.fps = {60, 55, 60};
                sample.present = {16.67, 16.67, 18.18};
                sample.status.clear();
                const auto size = monitor::MeasureHud(config, 18, ImGui::GetContentRegionAvail().x, 1000);
                monitor::DrawClassicHud(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size,
                                        config, sample);
                ImGui::Dummy(ImVec2(size.width, size.height));
            }));
        for (const char *id : {"frame-metrics", "device-info", "gpu-info", "performance-hud"})
            previewPlugins.load(id);
        auto preview = [&](Page page, const char *suffix) {
            state.selected = page;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("preview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar);
            DrawPages(
                state, Language::Chinese,
                [&] {
                    {
                        Card card("interface");
                        ImGui::TextUnformatted("界面");
                        ImGui::Separator();
                        const char *languages[] = {"跟随系统", "English", "简体中文"};
                        int choice = 0;
                        ImGui::Combo(FieldLabel(Language::Chinese, "Language").c_str(), &choice, languages, 3);
                        ImGui::TextDisabled("系统字体");
                    }
                    RenderFrame frame;
                    frame.language = Language::Chinese;
                    frame.deltaSeconds = 1.f / 60;
                    frame.framerate = 60;
                    frame.surfaceWidth = 1440;
                    frame.surfaceHeight = 3200;
                    frame.gpuName = "Adreno 750";
                    frame.apiVersion = (1u << 22) | (3u << 12);
                    frame.imageCount = 3;
                    previewPlugins.emit(frame);
                },
                [&] {
                    for (const char *key :
                         {"Frame metrics", "Screen & device", "Vulkan GPU", "Foreground performance HUD"}) {
                        bool enabled = true;
                        Card card(key);
                        Toggle(Label(Language::Chinese, key).c_str(), &enabled);
                        ImGui::TextWrapped(
                            "%s", Text(Language::Chinese, (std::string(key) + " description").c_str()));
                        ImGui::TextDisabled("%s", Text(Language::Chinese, "Enabled"));
                    }
                });
            ImGui::End();
            ImGui::Render();
            unsigned char *atlas;
            int aw, ah;
            io.Fonts->GetTexDataAsAlpha8(&atlas, &aw, &ah);
            test_preview::SavePreview((std::string(argv[1]) + suffix).c_str(), *ImGui::GetDrawData(), atlas,
                                      aw, ah, 420, 800);
        };
        preview(Page::Home, "-home.ppm");
        preview(Page::Plugins, "-plugins.ppm");
    }
    ImGui::DestroyContext(context);
}
