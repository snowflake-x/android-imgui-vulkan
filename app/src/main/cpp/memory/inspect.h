#pragma once
#include "memory/scan.h"

namespace vulkanscope::memory {
/** @brief Read bounded RAM and infer candidate layout; every numeric interpretation remains non-authoritative. */
std::string Inspect(std::uint64_t address, std::size_t count, unsigned pointerBytes,
                    const std::vector<Region> &regions, const Reader &read, const Stop &stop, bool unity);
}
