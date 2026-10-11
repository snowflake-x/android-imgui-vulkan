#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vulkanscope::memory {
struct Region { std::uint64_t begin, end; std::string path; };
using Reader = std::function<std::size_t(std::uint64_t, void *, std::size_t)>;
using Stop = std::function<bool()>;
/** @brief Only readable RAM mappings are eligible; device mappings and malformed ranges are excluded. */
std::vector<Region> ParseMaps(const std::string &maps);
/** @brief Exact little-endian UTF-8, hex, signed i32/i64 or finite f32 search; invalid input throws. */
std::vector<unsigned char> Pattern(const std::string &kind, const std::string &input);
struct ScanResult {
    std::vector<std::uint64_t> addresses;
    std::uint64_t bytes = 0, unreadable = 0;
    bool partial = false;
};
/** @brief Chunked scan with overlap, bounded reads/results and cooperative cancellation. */
ScanResult Scan(const std::vector<Region> &regions, const Reader &read, const Stop &stop,
                const std::vector<unsigned char> &pattern, std::uint64_t budget = 512ULL << 20,
                std::size_t maximum = 1024);
struct MetadataResult {
    bool valid = false, partial = false;
    unsigned version = 0, stride = 0;
    std::size_t total = 0, emitted = 0;
    std::string report, error;
};
/** @brief Validate live IL2CPP type tables before reporting metadata-defined names, not instantiated objects. */
MetadataResult ReadMetadata(std::uint64_t base, const Reader &read, const Stop &stop);
struct Process { int pid = 0; std::uint64_t start = 0; unsigned uid = 0; std::string name; };
std::uint64_t StartTicks(const std::string &stat);
/** @brief Isolated root helper entry; reads only, with its own deadline and stdout-disconnect cancellation. */
std::string Probe(const std::string &operation, int pid, std::uint64_t start,
                  const std::string &kind, const std::string &input);
}
