#include "memory/scan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace vulkanscope::memory {
namespace {
std::uint32_t word(const unsigned char *p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
bool utf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 32 || c == 127) return false;
        if (c < 128) continue;
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!n || i + n > s.size()) return false;
        unsigned cp = c & ((1u << (6 - n)) - 1);
        for (unsigned j = 0; j < n; ++j) {
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if ((n == 1 && cp < 128) || (n == 2 && cp < 2048) || (n == 3 && cp < 65536) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}
}
std::vector<Region> ParseMaps(const std::string &maps) {
    std::vector<Region> result;
    std::istringstream lines(maps);
    std::string line;
    while (std::getline(lines, line)) {
        if (result.size() >= 16384) throw std::runtime_error("Too many memory mappings; scan not started");
        std::istringstream row(line);
        std::string range, perms, offset, device, inode, path;
        if (!(row >> range >> perms >> offset >> device >> inode) || perms.empty() || perms[0] != 'r') continue;
        std::getline(row >> std::ws, path);
        if (path.rfind("/dev/", 0) == 0 || path == "[vvar]" || path == "[vsyscall]") continue;
        const auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        try {
            std::size_t a, b;
            auto begin = std::stoull(range.substr(0, dash), &a, 16), end = std::stoull(range.substr(dash + 1), &b, 16);
            if (a != dash || b != range.size() - dash - 1 || end <= begin ||
                end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) continue;
            if (!result.empty() && begin < result.back().end) continue;
            result.push_back({begin, end, path});
        } catch (const std::exception &) {}
    }
    return result;
}
std::vector<unsigned char> Pattern(const std::string &kind, const std::string &input) {
    if (input.empty() || input.size() > (kind == "hex" ? 512u : 256u)) throw std::invalid_argument("Pattern must contain 1-256 bytes");
    if (kind == "utf8") return {input.begin(), input.end()};
    if (kind == "hex") {
        std::string digits;
        for (unsigned char c : input) if (c != ' ') digits += static_cast<char>(c);
        if (digits.empty() || digits.size() % 2) throw std::invalid_argument("Hex requires complete byte pairs");
        std::vector<unsigned char> result;
        auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::invalid_argument("Invalid hex byte");
        };
        for (std::size_t i = 0; i < digits.size(); i += 2)
            result.push_back(static_cast<unsigned char>(nibble(digits[i]) * 16 + nibble(digits[i + 1])));
        return result;
    }
    std::size_t end = 0;
    std::uint64_t bits = 0;
    unsigned size = 4;
    if (kind == "i32" || kind == "i64") {
        const auto n = std::stoll(input, &end, 10);
        if (kind == "i32" && (n < INT32_MIN || n > INT32_MAX)) throw std::invalid_argument("Int32 out of range");
        bits = static_cast<std::uint64_t>(n);
        size = kind == "i64" ? 8 : 4;
    } else if (kind == "f32") {
        const float value = std::stof(input, &end);
        if (!std::isfinite(value)) throw std::invalid_argument("Float must be finite");
        std::uint32_t raw;
        std::memcpy(&raw, &value, sizeof(raw));
        bits = raw;
    } else throw std::invalid_argument("Unknown search type");
    if (end != input.size()) throw std::invalid_argument("Invalid numeric value");
    std::vector<unsigned char> result;
    for (unsigned i = 0; i < size; ++i) result.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    return result;
}
ScanResult Scan(const std::vector<Region> &regions, const Reader &read, const Stop &stop,
                const std::vector<unsigned char> &pattern, std::uint64_t budget, std::size_t maximum) {
    if (pattern.empty() || pattern.size() > 256 || !maximum) throw std::invalid_argument("Invalid scan bounds");
    ScanResult result;
    constexpr std::size_t chunk = 64 * 1024;
    std::vector<unsigned char> data(chunk + pattern.size());
    std::size_t tail = 0;
    std::uint64_t previousEnd = 0;
    for (const auto &region : regions) {
        if (region.begin != previousEnd) tail = 0;
        for (auto cursor = region.begin; cursor < region.end;) {
            if (stop() || result.bytes >= budget) { result.partial = true; return result; }
            const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>({chunk, region.end - cursor, budget - result.bytes}));
            const auto got = read(cursor, data.data() + tail, wanted);
            if (got > wanted) throw std::runtime_error("Reader exceeded requested size");
            if (!got) {
                ++result.unreadable;
                tail = 0;
                cursor += std::min<std::uint64_t>(4096, region.end - cursor);
                continue;
            }
            result.bytes += got;
            const auto finish = data.begin() + tail + got;
            auto found = std::search(data.begin(), finish, pattern.begin(), pattern.end());
            while (found != finish) {
                result.addresses.push_back(cursor - tail + static_cast<std::uint64_t>(found - data.begin()));
                if (result.addresses.size() >= maximum) { result.partial = true; return result; }
                found = std::search(found + 1, finish, pattern.begin(), pattern.end());
            }
            tail = std::min(pattern.size() - 1, tail + got);
            std::memmove(data.data(), data.data() + (finish - data.begin()) - tail, tail);
            cursor += got;
        }
        previousEnd = region.end;
    }
    result.partial = result.unreadable != 0;
    return result;
}
MetadataResult ReadMetadata(std::uint64_t base, const Reader &read, const Stop &stop) {
    MetadataResult result;
    std::array<unsigned char, 168> header{};
    if (read(base, header.data(), header.size()) != header.size() || word(header.data()) != 0xfab11baf) {
        result.error = "No complete IL2CPP metadata header"; return result;
    }
    result.version = word(header.data() + 4);
    if (result.version != 24 && result.version != 27 && result.version != 29 && result.version != 31) {
        result.error = "Unsupported IL2CPP metadata version " + std::to_string(result.version); return result;
    }
    const auto stringsOffset = word(header.data() + 24), stringsSize = word(header.data() + 28);
    const auto typesOffset = word(header.data() + 160), typesSize = word(header.data() + 164);
    if (stringsOffset < header.size() || typesOffset < header.size() || !stringsSize || stringsSize > (32u << 20) ||
        !typesSize || typesSize > (32u << 20) || std::uint64_t(stringsOffset) + stringsSize > (256u << 20) ||
        std::uint64_t(typesOffset) + typesSize > (256u << 20) || base > UINT64_MAX - (256u << 20) ||
        !(std::uint64_t(stringsOffset) + stringsSize <= typesOffset ||
          std::uint64_t(typesOffset) + typesSize <= stringsOffset)) {
        result.error = "Invalid or oversized metadata tables"; return result;
    }
    auto table = [&](std::uint64_t offset, std::size_t size) {
        std::vector<unsigned char> data(size);
        for (std::size_t done = 0; done < size;) {
            if (stop()) return std::vector<unsigned char>{};
            const auto n = std::min<std::size_t>(65536, size - done);
            if (read(base + offset + done, data.data() + done, n) != n) return std::vector<unsigned char>{};
            done += n;
        }
        return data;
    };
    const auto strings = table(stringsOffset, stringsSize), types = table(typesOffset, typesSize);
    if (strings.empty() || types.empty()) { result.error = "Metadata unreadable, changed, or cancelled"; return result; }
    auto name = [&](unsigned index, std::string &out) {
        if (index >= strings.size()) return false;
        auto end = std::find(strings.begin() + index, strings.begin() + std::min(strings.size(), std::size_t(index) + 1025), 0);
        if (end == strings.begin() + std::min(strings.size(), std::size_t(index) + 1025)) return false;
        out.assign(strings.begin() + index, end);
        return utf8(out);
    };
    const std::vector<unsigned> strides = result.version == 24 ? std::vector<unsigned>{104, 100, 92} : std::vector<unsigned>{88};
    unsigned validated = 0;
    for (unsigned stride : strides) {
        if (types.size() % stride || types.size() / stride > 300000) continue;
        std::string lines;
        std::size_t emitted = 0;
        bool valid = true, partial = false;
        for (std::size_t i = 0; i < types.size(); i += stride) {
            if (stop()) { result.error = "Cancelled during metadata validation"; return result; }
            std::string type, ns;
            auto token = word(types.data() + i + stride - 4);
            if ((token & 0xff000000u) != 0x02000000u || !(token & 0xffffffu) ||
                !name(word(types.data() + i), type) || type.empty() || !name(word(types.data() + i + 4), ns)) {
                valid = false; break;
            }
            const auto line = (ns.empty() ? type : ns + "." + type) + "\n";
            if (emitted < 10000 && lines.size() + line.size() < (480u << 10)) { lines += line; ++emitted; }
            else partial = true;
        }
        if (!valid) continue;
        ++validated;
        result.stride = stride; result.total = types.size() / stride;
        result.emitted = emitted; result.partial = partial; result.report = std::move(lines);
    }
    result.valid = validated == 1;
    if (!result.valid) { result.report.clear(); result.error = validated ? "Ambiguous type layout" : "Unsupported or inconsistent type layout"; }
    return result;
}
std::uint64_t StartTicks(const std::string &stat) {
    const auto end = stat.rfind(')');
    if (end == std::string::npos) return 0;
    std::istringstream fields(stat.substr(end + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i) if (!(fields >> field)) return 0;
    try { std::size_t n; auto value = std::stoull(field, &n); return n == field.size() ? value : 0; }
    catch (const std::exception &) { return 0; }
}
}
