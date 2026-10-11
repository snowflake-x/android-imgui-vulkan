#include "manager/manager.h"
#include "features/builtin_plugins.h"
#include "features/performance_hud.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace vulkanscope {
namespace {
constexpr std::array<const char *, 3> ids{"device-info", "gpu-info", "performance-hud"};
constexpr std::array<const char *, 3> labels{"Screen & device", "Vulkan GPU", "Foreground performance HUD"};
}
std::string JsonString(const std::string &value) {
    const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
Manager::Manager(monitor::Session &session) : session_(session) {
    RegisterBuiltinPlugins(runtime_, std::make_shared<HostReports>([&session] { return session.system(); }));
    RegisterPerformanceHudPlugin(runtime_, std::make_shared<HostPerformanceHud>(
        [&session] { return session.report(); }, [] {}, [] {}, [&session] { session.stop(); }));
    synchronizePlugins();
}
void Manager::synchronizePlugins() {
    const auto c = session_.config();
    for (unsigned i = 0; i < ids.size(); ++i) {
        if (c.managerPlugins & (1u << i)) runtime_.load(ids[i]);
        else if (runtime_.isLoaded(ids[i])) runtime_.unload(ids[i]);
    }
}
std::string Manager::snapshot(ui::Language language) {
    synchronizePlugins();
    const auto c = session_.config();
    const auto hud = session_.hud();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    auto text = [&](const std::string &s) { return JsonString(s); };
    const char *rootStates[] = {"unknown", "requesting", "granted", "unavailable"};
    out << "{\"rootAccess\":" << text(rootStates[static_cast<unsigned>(session_.rootAccess())])
        << ",\"rootReport\":" << text(session_.rootReport()) << ",\"running\":" << (session_.running() ? "true" : "false")
        << ",\"rootDisplayActive\":" << (session_.rootDisplayActive() ? "true" : "false")
        << ",\"report\":" << text(session_.report()) << ",\"system\":" << text(ui::TranslateReport(language, session_.system()))
        << ",\"fresh\":" << (hud.capturedNs > 0 ? "true" : "false")
        << ",\"apiCaption\":" << text(monitor::GraphicsApiCaption(c, hud))
        << ",\"config\":{\"root\":" << (c.root ? "true" : "false") << ",\"autoRoot\":" << (c.autoRoot ? "true" : "false")
        << ",\"rootDisplay\":" << (c.rootDisplay ? "true" : "false")
        << ",\"hudGraph\":" << (c.hudGraph ? "true" : "false") << ",\"hudExtras\":" << (c.hudExtras ? "true" : "false")
        << ",\"interval\":" << c.interval << ",\"textSize\":" << c.textSize << ",\"opacity\":" << c.opacity
        << ",\"corner\":" << c.corner << ",\"surface\":" << text(c.surface) << ",\"powerPath\":" << text(c.powerPath)
        << ",\"powerScale\":" << c.powerScale << ",\"language\":" << text(c.language) << "},\"plugins\":[";
    for (unsigned i = 0; i < ids.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << text(ids[i]) << ",\"label\":" << text(ui::Text(language, labels[i]))
            << ",\"description\":" << text(ui::Text(language, (std::string(labels[i]) + " description").c_str()))
            << ",\"enabled\":" << (runtime_.isLoaded(ids[i]) ? "true" : "false") << '}';
    }
    out << "],\"metrics\":[";
    bool first = true;
    for (const auto &id : c.order) {
        const auto metric = std::find_if(monitor::Metrics().begin(), monitor::Metrics().end(), [&](const auto &m) { return id == m.id; });
        if (metric == monitor::Metrics().end()) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << text(id) << ",\"label\":" << text(ui::Text(language, metric->label))
            << ",\"enabled\":" << (c.has(id) ? "true" : "false") << '}';
    }
    out << "],\"values\":{";
    first = true;
    for (const auto &[key, value] : hud.values) {
        if (!first) out << ',';
        first = false;
        out << text(key) << ':' << text(value);
    }
    out << "},\"history\":[";
    first = true;
    for (const auto &entry : session_.diagnosticHistory()) {
        if (!first) out << ',';
        first = false;
        out << "{\"package\":" << text(entry.package) << ",\"user\":" << entry.user << '}';
    }
    out << "]}";
    return out.str();
}
bool Manager::command(const std::string &key, const std::string &value) {
    if (key.size() > 128 || value.size() > 512) return false;
    if (key == "retryRoot") { session_.requestRoot(); return true; }
    bool valid = true;
    const auto saved = session_.updateConfig([&](monitor::Config &c) {
        auto boolean = [&](bool &field) {
            if (value == "true") field = true;
            else if (value == "false") field = false;
            else valid = false;
        };
        auto integer = [&](int &field, int minimum, int maximum) {
            const auto n = monitor::Number(value);
            if (!n || *n < minimum || *n > maximum || std::floor(*n) != *n) valid = false;
            else field = static_cast<int>(*n);
        };
        if (key == "root") boolean(c.root);
        else if (key == "autoRoot") boolean(c.autoRoot);
        else if (key == "rootDisplay") boolean(c.rootDisplay);
        else if (key == "hudGraph") boolean(c.hudGraph);
        else if (key == "hudExtras") boolean(c.hudExtras);
        else if (key == "interval") {
            if (value != "500" && value != "1000" && value != "2000") valid = false;
            else c.interval = std::stoi(value);
        } else if (key == "textSize") integer(c.textSize, 10, 24);
        else if (key == "opacity") integer(c.opacity, 20, 100);
        else if (key == "corner") integer(c.corner, 0, 3);
        else if (key == "surface") c.surface = value;
        else if (key == "powerPath") {
            if (value.empty() || monitor::ValidPowerPath(value)) c.powerPath = value;
            else valid = false;
        } else if (key == "powerScale") {
            const auto n = monitor::Number(value);
            if (n && *n > 0) c.powerScale = *n;
            else valid = false;
        } else if (key == "language") {
            if (value == "en" || value == "zh-Hans" || value == "system") c.language = value;
            else valid = false;
        } else if (key == "preset") {
            if (value == "game" || value == "hardware") monitor::ApplyMetricPreset(c, value == "game" ? monitor::MetricPreset::Game : monitor::MetricPreset::Hardware);
            else valid = false;
        } else if (key == "reset" && value == "true") c = monitor::Config{};
        else if (key.rfind("metric:", 0) == 0) {
            const auto found = c.enabled.find(key.substr(7));
            if (found == c.enabled.end()) valid = false;
            else boolean(found->second);
        } else if (key.rfind("plugin:", 0) == 0) {
            const auto found = std::find(ids.begin(), ids.end(), key.substr(7));
            if (found == ids.end() || (value != "true" && value != "false")) valid = false;
            else {
                const auto mask = 1u << std::distance(ids.begin(), found);
                c.managerPlugins = value == "true" ? c.managerPlugins | mask : c.managerPlugins & ~mask;
            }
        } else if (key == "up" || key == "down") {
            const auto found = std::find(c.order.begin(), c.order.end(), value);
            if (found == c.order.end()) valid = false;
            else if (key == "up" && found != c.order.begin()) std::iter_swap(found, found - 1);
            else if (key == "down" && found + 1 != c.order.end()) std::iter_swap(found, found + 1);
        } else valid = false;
    });
    if (!valid || !saved) return false;
    if (key == "plugin:performance-hud" && value == "false") session_.stop();
    synchronizePlugins();
    return true;
}
std::string Manager::diagnostic(const std::string &package, int user) const {
    if (package.empty()) return session_.diagnostics();
    for (const auto &entry : session_.diagnosticHistory())
        if (entry.package == package && entry.user == user) return entry.text;
    return {};
}
}
