#include "RegionScheduleTrace.h"
#include "ReplayStream.h"

#include "persistence/ReplacementOutputFile.h"
#include "persistence/SavedJsonAdmission.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "world/WorldClock.h"

#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <system_error>

namespace Luminumbra::Replay {
namespace {
constexpr std::uint64_t kMaxTraceTicks = 1000000;
constexpr std::uintmax_t kMaxTraceBytes = 96 * 1024 * 1024;
constexpr const char* kSchema = "luminumbra.region_schedule_trace.v1";
bool ValidDigest(const std::string& digest) {
    return digest.size() == 16 && digest.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool EncodeTrace(const RegionScheduleTrace& trace, std::string& bytes) {
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
    if (!Persistence::BoundedJsonDump(value, kMaxTraceBytes - 1, bytes))
        return false;
    bytes += '\n';
    return true;
}
} // namespace

bool ReplayWriter::Finalize(std::uint64_t tick_count, const RegionScheduleTrace* trace) {
    if (!m_open)
        return false;
    try {
        std::string bytes;
        if (trace && (trace->size() != tick_count || !EncodeTrace(*trace, bytes))) {
            m_open = false;
            std::vector<std::uint8_t>{}.swap(m_buffer);
            return false;
        }
        return Finish(tick_count, trace ? &bytes : nullptr, true);
    } catch (const std::exception&) {
        m_open = false;
        std::vector<std::uint8_t>{}.swap(m_buffer);
        return false;
    }
}

bool WriteRegionScheduleTrace(const std::filesystem::path& replay,
                              const RegionScheduleTrace& trace,
                              std::string& error) {
    error = "Corrupt region schedule trace.";
    std::string bytes;
    if (!EncodeTrace(trace, bytes))
        return false;
    auto path = replay;
    path += ".regions.json";
    try {
        // The guard opens the replay's directory, so a fresh destination needs
        // its parent first, as the unguarded writer created it before.
        if (replay.has_parent_path()) {
            std::error_code parent_error;
            std::filesystem::create_directories(replay.parent_path(), parent_error);
            if (parent_error) {
                error = "Cannot write region schedule trace.";
                return false;
            }
        }
        // Paired publication and its recovery own the companion under this
        // directory guard; a standalone write must not replace it meanwhile.
        const Persistence::ReplayPairReadGuard ownership(replay);
        if (!ownership.is_open()) {
            error = "Cannot write region schedule trace.";
            return false;
        }
        Persistence::ReplacementOutputFile output(path);
        if (!output.write(bytes) || !output.commit()) {
            error = "Cannot write region schedule trace.";
            return false;
        }
    } catch (const std::exception&) {
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
        if (expected_ticks > kMaxTraceTicks)
            return false;
        auto path = replay;
        path += ".regions.json";
        nlohmann::json value;
        bool missing = false;
        if (!Persistence::ReadSavedJson(path, kMaxTraceBytes, value, missing, false))
            return false;
        if (missing) {
            error = "Missing region schedule trace.";
            return false;
        }
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
