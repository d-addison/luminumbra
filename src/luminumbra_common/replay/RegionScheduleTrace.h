#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Replay {
// Optional enabled-only replay companion. LREC1 records remain unchanged.
using RegionScheduleTrace = std::vector<std::pair<std::uint64_t, std::string>>;
bool WriteRegionScheduleTrace(const std::filesystem::path& replay,
                              const RegionScheduleTrace& trace,
                              std::string& error);
bool ReadRegionScheduleTrace(const std::filesystem::path& replay,
                             std::uint64_t expected_ticks,
                             RegionScheduleTrace& trace,
                             std::string& error);
} // namespace Luminumbra::Replay
