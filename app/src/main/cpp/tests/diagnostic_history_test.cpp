#include "monitor/diagnostics.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace vulkanscope::monitor;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
Values foreground(const std::string &package, const std::string &user = "0") {
    return {{"foreground", package}, {"foreground_user", user}};
}
void desktopTransition() {
    DiagnosticHistory history;
    const auto game = foreground("com.tencent.tmgp.sgame");
    history.record(game, "Game: Surface latency unavailable");
    history.record(foreground("com.android.launcher"), "Desktop sample");
    history.record(foreground("dev.snowflake.vulkan"), "Monitor sample");
    require(history.entries().size() == 2, "returning to the monitor must retain separate game and desktop records");
    require(history.entries()[0].package == "com.android.launcher" &&
                history.entries()[1].package == "com.tencent.tmgp.sgame" &&
                history.entries()[1].text == "Game: Surface latency unavailable",
            "a desktop sample cannot overwrite the previous game's diagnostics");
    history.record(game, "Game: new diagnostics");
    require(history.entries().size() == 2 && history.entries()[0].text == "Game: new diagnostics",
            "resampling updates the existing package and makes it most recent");
    auto snapshot = history.entries();
    history.record(game, "Game: another sample");
    require(snapshot[0].text == "Game: new diagnostics", "copied history remains stable while sampling continues");
    history.record(foreground("com.tencent.tmgp.sgame", "10"), "Game: work-profile sample");
    require(history.entries().size() == 3 && history.entries()[0].user == 10 &&
                history.entries()[1].user == 0 && history.entries()[1].text == "Game: another sample",
            "the same package in different Android users retains independent records");
}
void boundedHistory() {
    DiagnosticHistory history;
    for (std::size_t i = 0; i < DiagnosticHistory::Capacity + 2; ++i)
        history.record(foreground("com.game.app" + std::to_string(i)), std::to_string(i));
    require(history.entries().size() == DiagnosticHistory::Capacity &&
                history.entries().front().text == std::to_string(DiagnosticHistory::Capacity + 1) &&
                history.entries().back().text == "2", "only the eight most recently sampled applications are retained");
    history.record(foreground("com.game.app2"), "renewed");
    history.record(foreground("com.game.other"), "new");
    require(history.entries()[1].text == "renewed" &&
                std::none_of(history.entries().begin(), history.entries().end(), [](const auto &entry) {
                    return entry.package == "com.game.app3";
                }), "updating a record renews its retention before the next eviction");
    std::string oversized;
    while (oversized.size() <= DiagnosticHistory::MaxTextBytes)
        oversized += "诊断";
    history.record(foreground("com.game.large"), oversized);
    const auto &text = history.entries().front().text;
    const auto marker = text.find("\n[Diagnostics truncated at 64 KiB]\n");
    require(text.size() <= DiagnosticHistory::MaxTextBytes && marker != std::string::npos && marker % 3 == 0 &&
                text.substr(0, marker) == oversized.substr(0, marker),
            "large diagnostics have a marked byte bound without splitting UTF-8 characters");
}
void invalidIdentity() {
    DiagnosticHistory history;
    history.record({{"foreground", "com.game.app"}}, "No Android user");
    history.record({{"foreground_user", "0"}}, "No package");
    for (const auto *package : {"", "N/A (Root disabled)", "invalid", "com.game/app"})
        history.record(foreground(package), "Invalid package");
    for (const auto *user : {"", "-1", "1suffix", "1.0", "999999999999999999"})
        history.record(foreground("com.game.app", user), "Invalid user");
    history.record(foreground("com.game.app"), "");
    require(history.entries().empty(), "incomplete or unavailable foreground identities cannot replace diagnostics");
}
}
int main() {
    try {
        desktopTransition();
        boundedHistory();
        invalidIdentity();
        std::cout << "Per-application diagnostic history tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
