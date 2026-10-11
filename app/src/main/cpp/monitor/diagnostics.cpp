#include "monitor/diagnostics.h"
#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

namespace vulkanscope::monitor {
void DiagnosticHistory::record(const Values &values, const std::string &text) {
    const auto package = values.find("foreground"), userText = values.find("foreground_user");
    if (package == values.end() || userText == values.end() || text.empty())
        return;
    const auto &name = package->second;
    if (name.empty() || name.size() > 256 || name == "dev.snowflake.vulkan" ||
        name.find('.') == std::string::npos ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '.';
        }))
        return;
    int user = -1;
    const auto &id = userText->second;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), user);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || user < 0)
        return;
    std::string bounded(text, 0, MaxTextBytes);
    if (text.size() > MaxTextBytes) {
        constexpr std::string_view suffix = "\n[Diagnostics truncated at 64 KiB]\n";
        auto end = MaxTextBytes - suffix.size();
        while (end && (static_cast<unsigned char>(bounded[end]) & 0xc0) == 0x80)
            --end;
        bounded.resize(end);
        bounded += suffix;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto &entry) {
        return entry.package == name && entry.user == user;
    }), entries_.end());
    entries_.insert(entries_.begin(), {name, user, std::move(bounded)});
    if (entries_.size() > Capacity)
        entries_.resize(Capacity);
}
}
