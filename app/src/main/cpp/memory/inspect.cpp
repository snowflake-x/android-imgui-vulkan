#include "memory/inspect.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vulkanscope::memory {
namespace {
std::uint64_t integer(const unsigned char *p, unsigned n) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < n; ++i) result |= std::uint64_t(p[i]) << (i * 8);
    return result;
}
std::string hex(std::uint64_t n) { std::ostringstream out; out << "0x" << std::hex << n; return out.str(); }
std::string ascii(const unsigned char *p, std::size_t size) {
    std::string result;
    for (std::size_t i = 0; i < size && p[i]; ++i) {
        if (p[i] < 32 || p[i] > 126) return {};
        result += static_cast<char>(p[i]);
    }
    return result;
}
}
std::string Inspect(std::uint64_t address, std::size_t count, unsigned pointerBytes,
                    const std::vector<Region> &regions, const Reader &read, const Stop &stop, bool unity) {
    if (!address || count < 16 || count > 4096 || (pointerBytes != 4 && pointerBytes != 8) ||
        address > UINT64_MAX - count) throw std::invalid_argument("Invalid view address, size, or pointer width");
    auto mapping = [&](std::uint64_t pointer) -> const Region * {
        for (const auto &region : regions) if (pointer >= region.begin && pointer < region.end) return &region;
        return nullptr;
    };
    const auto *region = mapping(address);
    if (!region) throw std::runtime_error("Address is not in a readable RAM mapping");
    std::vector<unsigned char> data(std::min<std::uint64_t>(count, region->end - address));
    const auto size = read(address, data.data(), data.size());
    if (!size || size > data.size() || stop()) throw std::runtime_error("Memory view unavailable or cancelled");
    data.resize(size);
    std::ostringstream out;
    out << "View: " << hex(address) << " bytes: " << size << "/" << count
        << "\nMapping: " << hex(region->begin) << "-" << hex(region->end) << " " << region->path
        << "\nPointer width: " << pointerBytes * 8 << " bits (target ELF); little endian"
        << "\nSnapshot only; layout hints are candidates, not verified field declarations.\n";
    out << "VIEW\t" << hex(address) << '\t';
    for (auto b : data) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    out << std::dec << "\n\nHex / ASCII\n";
    for (std::size_t offset = 0; offset < size; offset += 16) {
        out << hex(address + offset) << "  ";
        for (std::size_t i = 0; i < 16; ++i) {
            if (offset + i < size) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[offset + i]) << ' ';
            else out << "   ";
        }
        out << " |";
        for (std::size_t i = offset; i < std::min(size, offset + 16); ++i)
            out << char(data[i] >= 32 && data[i] <= 126 ? data[i] : '.');
        out << "|\n" << std::dec;
    }
    out << "\nLayout candidates / alternative interpretations\n";
    auto readString = [&](std::uint64_t pointer) {
        const auto *map = mapping(pointer);
        if (!map) return std::string{};
        unsigned char bytes[129]{};
        auto n = read(pointer, bytes, std::min<std::uint64_t>(sizeof(bytes), map->end - pointer));
        auto end = std::find(bytes, bytes + n, 0);
        if (end == bytes + n) return std::string{};
        return ascii(bytes, static_cast<std::size_t>(end - bytes));
    };
    if (unity && size >= pointerBytes * 2) {
        const auto klass = integer(data.data(), pointerBytes), monitor = integer(data.data() + pointerBytes, pointerBytes);
        const auto *map = mapping(klass);
        unsigned char header[32]{};
        if (klass % pointerBytes == 0 && map && map->end - klass >= pointerBytes * 4 &&
            (!monitor || mapping(monitor)) && read(klass, header, pointerBytes * 4) == pointerBytes * 4) {
            const auto image = integer(header, pointerBytes);
            auto name = readString(integer(header + pointerBytes * 2, pointerBytes));
            const auto ns = readString(integer(header + pointerBytes * 3, pointerBytes));
            if (mapping(image) && !name.empty() && name.find(' ') == std::string::npos) {
                out << "Unity object-header candidate: " << (ns.empty() ? name : ns + "." + name)
                    << "; +" << hex(0) << " klass=" << hex(klass)
                    << "; +" << hex(pointerBytes) << " monitor=" << hex(monitor)
                    << "\nEvidence: readable image/class/name pointers and conventional Il2CppClass prefix. Version-dependent, heuristic.\n";
                /** @brief Infer FieldInfo placement by parent/token/name invariants instead of fixed class offsets. */
                std::vector<unsigned char> classBytes(std::min<std::uint64_t>(256, map->end - klass));
                const auto classSize = read(klass, classBytes.data(), classBytes.size());
                std::vector<std::uint64_t> visited;
                unsigned exported = 0;
                for (std::size_t slot = pointerBytes * 4; slot + pointerBytes <= classSize && exported < 64; slot += pointerBytes) {
                    const auto fields = integer(classBytes.data() + slot, pointerBytes);
                    if (!fields || !mapping(fields) || std::find(visited.begin(), visited.end(), fields) != visited.end()) continue;
                    visited.push_back(fields);
                    const unsigned recordSize = pointerBytes * 3 + 8;
                    for (unsigned index = 0; index < 64 && exported < 64; ++index) {
                        if (stop()) throw std::runtime_error("Field layout inference cancelled");
                        const auto entry = fields + std::uint64_t(index) * recordSize;
                        const auto *entryMap = mapping(entry);
                        unsigned char record[32]{};
                        if (entry < fields || !entryMap || entryMap->end - entry < recordSize ||
                            read(entry, record, recordSize) != recordSize ||
                            integer(record + pointerBytes * 2, pointerBytes) != klass) break;
                        const auto token = integer(record + pointerBytes * 3 + 4, 4);
                        const auto fieldName = readString(integer(record, pointerBytes));
                        const auto fieldType = integer(record + pointerBytes, pointerBytes);
                        const auto *typeMap = mapping(fieldType);
                        unsigned char type[16]{};
                        if ((token & 0xff000000) != 0x04000000 || !(token & 0xffffff) ||
                            fieldName.empty() || !typeMap || typeMap->end - fieldType < pointerBytes + 4 ||
                            read(fieldType, type, pointerBytes + 4) != pointerBytes + 4) break;
                        const auto bits = integer(type + pointerBytes, 4);
                        const auto code = (bits >> 16) & 255;
                        const bool isStatic = (bits & 0x10) != 0;
                        const auto offset = integer(record + pointerBytes * 3, 4);
                        if (!isStatic && (offset < pointerBytes * 2 || offset > (1u << 20))) break;
                        const char *typeName = code == 2 ? "bool" : code == 3 ? "char16" :
                            code == 4 ? "i8" : code == 5 ? "u8" : code == 6 ? "i16" : code == 7 ? "u16" :
                            code == 8 ? "i32" : code == 9 ? "u32" : code == 10 ? "i64" : code == 11 ? "u64" :
                            code == 12 ? "f32" : code == 13 ? "f64" : code == 14 ? "string-ref" :
                            code == 17 ? "value-type" : code == 18 ? "class-ref" : code == 29 ? "array-ref" : "type-code";
                        if (!index) out << "FieldInfo-chain candidate: klass+" << hex(slot) << " -> " << hex(fields) << '\n';
                        out << "  field +" << hex(offset) << " " << fieldName << " type=" << typeName
                            << "(" << code << ") token=" << hex(token) << (isStatic ? " static (not an instance offset)" : "");
                        if (!isStatic && offset < size) {
                            out << " bytes=";
                            for (auto i = offset; i < std::min<std::uint64_t>(size, offset + 8); ++i)
                                out << std::hex << std::setw(2) << std::setfill('0') << unsigned(data[i]);
                            out << std::dec;
                        }
                        out << '\n'; ++exported;
                    }
                }
                if (exported) out << "Field candidates validated parent, name, token and type pointers; count and full class layout remain unverified.\n";
                if (ns == "System" && name == "String" && size >= pointerBytes * 2 + 4) {
                    auto length = integer(data.data() + pointerBytes * 2, 4);
                    out << "String candidate: length=" << length << "; UTF-16 data begins +" << hex(pointerBytes * 2 + 4) << '\n';
                }
                if (name.find("[]") != std::string::npos && size >= pointerBytes * 4)
                    out << "Array candidate: bounds=" << hex(integer(data.data() + pointerBytes * 2, pointerBytes))
                        << "; length=" << integer(data.data() + pointerBytes * 3, pointerBytes)
                        << "; data begins +" << hex(pointerBytes * 4) << '\n';
            }
        }
    }
    for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
        if (stop()) throw std::runtime_error("Memory interpretation cancelled");
        auto raw = static_cast<std::uint32_t>(integer(data.data() + offset, 4));
        std::int32_t signedValue;
        float f;
        std::memcpy(&signedValue, &raw, 4); std::memcpy(&f, &raw, 4);
        out << "+" << hex(offset) << " u32=" << raw << " i32=" << signedValue;
        if (std::isfinite(f)) out << " f32=" << std::setprecision(7) << f;
        if (!raw) out << " [zero / padding candidate]";
        if (offset + 8 <= size && offset % 8 == 0) {
            auto raw64 = integer(data.data() + offset, 8);
            double d; std::int64_t signed64;
            std::memcpy(&d, &raw64, 8); std::memcpy(&signed64, &raw64, 8);
            out << " i64=" << signed64;
            if (std::isfinite(d)) out << " f64=" << d;
        }
        if (offset % pointerBytes == 0 && offset + pointerBytes <= size) {
            const auto pointer = integer(data.data() + offset, pointerBytes);
            if (pointer && mapping(pointer)) {
                out << " [readable pointer candidate -> " << hex(pointer) << "]";
                const auto text = readString(pointer);
                if (text.size() >= 4) out << " text=\"" << text << "\"";
                out << "\nPTR\t" << hex(pointer) << "\t+" << hex(offset);
            }
        }
        const auto text = ascii(data.data() + offset, std::min<std::size_t>(32, size - offset));
        if (text.size() >= 4) out << " [ASCII candidate: " << text << "]";
        if (offset + 8 <= size) {
            std::string utf16;
            for (auto i = offset; i + 1 < std::min(size, offset + 64); i += 2) {
                const auto code = unsigned(data[i]) | unsigned(data[i + 1]) << 8;
                if (code < 32 || code > 126) break;
                utf16 += static_cast<char>(code);
            }
            if (utf16.size() >= 4) out << " [UTF-16LE candidate: " << utf16 << "]";
        }
        out << '\n';
    }
    return out.str();
}
}
