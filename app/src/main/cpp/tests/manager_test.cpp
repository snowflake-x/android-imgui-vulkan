#include "manager/manager.h"
#include <filesystem>
#include <stdexcept>
#include <unistd.h>
using namespace vulkanscope;
namespace {
void require(bool value) { if (!value) throw std::runtime_error("manager contract failed"); }
}
int main() {
    char path[] = "/tmp/vulkanscope-manager-XXXXXX";
    require(mkdtemp(path) != nullptr);
    {
        monitor::Session session;
        session.initialize(path, {});
        Manager manager(session);
        require(JsonString("a\n\"\\\t") == "\"a\\u000a\\\"\\\\\\u0009\"");
        require(manager.command("language", "zh-Hans"));
        require(manager.command("autoRoot", "false"));
        require(session.config().rootDisplay);
        require(!session.config().liveUpdates);
        require(manager.command("liveUpdates", "true"));
        require(!manager.command("liveUpdates", "invalid"));
        session.collapse(true);
        require(manager.snapshot(ui::Language::English).find("\"hudCollapsed\":true") != std::string::npos);
        session.stop();
        require(!session.collapsed());
        require(manager.command("rootDisplay", "false"));
        require(manager.command("plugin:device-info", "false"));
        require((session.config().managerPlugins & 1u) == 0);
        require(!manager.command("plugin:unknown", "true"));
        require(!manager.command("opacity", "101"));
        require(!manager.command("opacity", "NaN"));
        require(!manager.command("root", "yes"));
        require(!manager.command("language", "invalid"));
        require(!manager.command("powerPath", "/tmp/not-sysfs"));
        require(manager.command("opacity", "75"));
        require(manager.command("surface", "游戏\"\\\n"));
        auto snapshot = manager.snapshot(ui::Language::English);
        require(snapshot.find("\\u000a") != std::string::npos);
        require(snapshot.find("\"opacity\":75") != std::string::npos);
        require(snapshot.find("\"fresh\":false") != std::string::npos);
        auto saved = monitor::LoadConfig(std::string(path) + "/native-hud.conf");
        require(saved.language == "zh-Hans" && !saved.autoRoot && saved.managerPlugins == 6 && !saved.rootDisplay);
        require(manager.command("resetPerformance", "true"));
        saved = monitor::LoadConfig(std::string(path) + "/native-hud.conf");
        require(saved.language == "zh-Hans" && !saved.autoRoot && saved.managerPlugins == 6);
        require(!saved.liveUpdates);
        require(saved.rootDisplay && saved.surface.empty() && saved.opacity == monitor::Config{}.opacity);
    }
    std::filesystem::remove_all(path);
}
