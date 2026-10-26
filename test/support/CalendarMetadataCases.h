#pragma once

#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <filesystem>
#include <system_error>

#include <nlohmann/json.hpp>
#include <vector>

namespace Luminumbra::TestSupport {

inline nlohmann::json DefaultCalendarMetadata() {
    return {{"version", 2},
            {"dayLengthTicks", 162000},
            {"daysPerYear", 8},
            {"daylightNumerator", 2},
            {"daylightDenominator", 3},
            {"lunarCycleDays", 8}};
}

// The same refusal corpus exercises parsing, catalogs, explicit snapshots and writers.
inline std::vector<nlohmann::json> InvalidCalendarMetadata() {
    using nlohmann::json;
    std::vector<json> invalid_calendars;
    const auto valid = DefaultCalendarMetadata();
    for (const auto& [field, value] : valid.items()) {
        auto missing = valid;
        missing.erase(field);
        invalid_calendars.push_back(missing);
        for (const auto& bad : {json(nullptr),
                                json(false),
                                json("2"),
                                json::array(),
                                json::object(),
                                json(-1),
                                json(0),
                                json(2.0),
                                json::parse("2e0"),
                                json::parse("18446744073709551615"),
                                json::parse("18446744073709551616")}) {
            auto mutated = valid;
            mutated[field] = bad;
            invalid_calendars.push_back(mutated);
        }
    }
    auto extra = valid;
    extra["extra"] = 1;
    invalid_calendars.push_back(extra);
    for (const auto& patch : {json{{"version", 1}},
                              json{{"version", 3}},
                              json{{"dayLengthTicks", 2147483648ull}},
                              json{{"daysPerYear", 367}},
                              json{{"daylightNumerator", 3}},
                              json{{"daylightNumerator", 4}, {"daylightDenominator", 6}},
                              json{{"daylightDenominator", 65536}},
                              json{{"lunarCycleDays", 367}}}) {
        auto mutated = valid;
        mutated.update(patch);
        invalid_calendars.push_back(mutated);
    }
    std::vector<json> result;
    for (const auto* key : {"calendar", "renderCalendar"}) {
        for (const auto& invalid : invalid_calendars)
            result.push_back({{key, invalid}});
        for (const auto& bad : {json(nullptr), json(false), json("calendar"), json::array()})
            result.push_back({{key, bad}});
    }
    result.push_back({{"renderCalendar", {{"dayLengthTicks", 216000}, {"daysPerYear", 8}}}});
    result.push_back({{"calendar", valid}, {"renderCalendar", valid}});
    result.push_back({{"simulationTick", 0u}, {"renderCalendar", valid}});
    return result;
}

} // namespace Luminumbra::TestSupport

// Convert only disposable fixture directories to the pre-index save representation.
namespace Luminumbra::TestSupport {
inline bool ForgetSnapshotIndex(const std::filesystem::path& save_dir) {
    namespace P = Luminumbra::Persistence;
    std::error_code error;
    std::filesystem::remove(P::snapshot_index_path(save_dir), error);
    if (error)
        return false;
    std::filesystem::remove_all(P::journal_directory(save_dir), error);
    if (error)
        return false;
    const auto path = save_dir / "world_info.json";
    std::string bytes;
    bool present = false;
    if (!P::read_snapshot_bytes(path, P::kMaxSnapshotIndexBytes, bytes, present, nullptr))
        return false;
    if (!present)
        return true;
    auto metadata = nlohmann::json::parse(bytes, nullptr, false);
    if (!metadata.is_object())
        return false;
    metadata.erase("snapshotFormat");
    return P::WorldSaveService::durable_write_file(path, metadata.dump(2) + "\n");
}
} // namespace Luminumbra::TestSupport
