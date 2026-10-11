#include "RegionScheduleTrace.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "world/WorldClock.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Luminumbra::Replay {
namespace {
constexpr std::uint64_t kMaxTraceTicks = 1000000;
constexpr std::uintmax_t kMaxTraceBytes = 96 * 1024 * 1024;
constexpr const char* kSchema = "luminumbra.region_schedule_trace.v1";
bool ValidDigest(const std::string& digest) {
    return digest.size() == 16 && digest.find_first_not_of("0123456789abcdef") == std::string::npos;
}
} // namespace
bool WriteRegionScheduleTrace(const std::filesystem::path& replay,
                              const RegionScheduleTrace& trace,
                              std::string& error) {
    error = "Corrupt region schedule trace.";
    if (trace.size() > kMaxTraceTicks)
        return false;
    nlohmann::json ticks = nlohmann::json::array();
    std::uint64_t expected = 1;
    for (const auto& [tick, digest] : trace) {
        if (tick != expected++ || !ValidDigest(digest))
            return false;
        ticks.push_back({{"tick", tick}, {"digest", digest}});
    }
    nlohmann::json value{{"schema", kSchema}, {"ticks", ticks}};
    value["checksum"] = Persistence::StableChecksum(value.dump());
    std::ofstream out(replay.string() + ".regions.json", std::ios::binary);
    out << value.dump() << '\n';
    out.close();
    if (!out) {
        error = "Cannot write region schedule trace.";
        return false;
    }
    error.clear();
    return true;
}
bool ReadRegionScheduleTrace(const std::filesystem::path& replay,
                             std::uint64_t expected_ticks,
                             RegionScheduleTrace& trace,
                             std::string& error) {
    error = "Corrupt region schedule trace.";
    try {
        const auto path = replay.string() + ".regions.json";
        if (!std::filesystem::exists(path)) {
            error = "Missing region schedule trace.";
            return false;
        }
        if (expected_ticks > kMaxTraceTicks || std::filesystem::file_size(path) > kMaxTraceBytes)
            return false;
        std::ifstream in(path, std::ios::binary);
        auto value = nlohmann::json::parse(in);
        if (!value.is_object() || !value.contains("schema") || !value["schema"].is_string())
            return false;
        const auto schema = value["schema"].get<std::string>();
        const std::string prefix = "luminumbra.region_schedule_trace.v";
        if (schema != kSchema) {
            if (schema.starts_with(prefix)) {
                const auto version = schema.substr(prefix.size());
                if (!version.empty() && version.front() != '0' &&
                    version.find_first_not_of("0123456789") == std::string::npos &&
                    (version.size() > 1 || version.front() > '1'))
                    error = "Unsupported future region schedule trace version.";
            }
            return false;
        }
        if (value.size() != 3 || !value.contains("ticks") || !value["ticks"].is_array() ||
            value["ticks"].size() != expected_ticks || !value.contains("checksum") ||
            !value["checksum"].is_string())
            return false;
        const auto checksum = value["checksum"].get<std::string>();
        value.erase("checksum");
        if (checksum != Persistence::StableChecksum(value.dump()))
            return false;
        RegionScheduleTrace result;
        for (const auto& row : value["ticks"]) {
            if (!row.is_object() || row.size() != 2 || !row.contains("tick") ||
                !row["tick"].is_number_unsigned() || !row.contains("digest") ||
                !row["digest"].is_string())
                return false;
            const auto tick = row["tick"].get<std::uint64_t>();
            const auto digest = row["digest"].get<std::string>();
            if (tick != result.size() + 1 || tick >= world::WorldClock::kTickLimit ||
                !ValidDigest(digest))
                return false;
            result.emplace_back(tick, digest);
        }
        trace = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace Luminumbra::Replay
