#pragma once

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
