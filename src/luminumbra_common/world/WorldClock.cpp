#include "WorldClock.h"

#include "../core/SimulationClock.h"

#include <limits>
#include <numeric>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Luminumbra::world {
namespace {
bool ValidCalendar(WorldCalendar calendar) {
    return calendar.dayLengthTicks > 0 && calendar.dayLengthTicks < (std::uint64_t{1} << 31) &&
           calendar.daysPerYear > 0 && calendar.daysPerYear <= 366 &&
           calendar.daylightNumerator > 0 &&
           calendar.daylightNumerator < calendar.daylightDenominator &&
           calendar.daylightDenominator <= 65535 &&
           std::gcd(calendar.daylightNumerator, calendar.daylightDenominator) == 1 &&
           calendar.lunarCycleDays >= 1 && calendar.lunarCycleDays <= 366 &&
           calendar.dayLengthTicks <=
               std::numeric_limits<std::uint64_t>::max() / calendar.daysPerYear;
}
bool DecodeCalendar(const nlohmann::json& value, WorldCalendar& out, bool allow_legacy) {
    if (!value.is_object())
        return false;
    const bool legacy = !value.contains("version");
    if (legacy && !allow_legacy)
        return false;
    if (value.size() != (legacy ? 2u : 6u))
        return false;
    const auto integer = [&value](const char* key, std::uint64_t low, std::uint64_t high) {
        if (!value.contains(key))
            return false;
        const auto& v = value.at(key);
        if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<std::int64_t>() < 0))
            return false;
        const auto number = v.get<std::uint64_t>();
        return number >= low && number <= high;
    };
    if (!integer("dayLengthTicks", 1, 2147483647) || !integer("daysPerYear", 1, 366))
        return false;
    WorldCalendar calendar = WorldCalendar::Legacy();
    calendar.dayLengthTicks = value.at("dayLengthTicks").get<std::uint32_t>();
    calendar.daysPerYear = value.at("daysPerYear").get<std::uint32_t>();
    if (!legacy) {
        if (!integer("version", 2, 2) || !integer("daylightNumerator", 1, 65534) ||
            !integer("daylightDenominator", 2, 65535) || !integer("lunarCycleDays", 1, 366))
            return false;
        calendar.daylightNumerator = value.at("daylightNumerator").get<std::uint32_t>();
        calendar.daylightDenominator = value.at("daylightDenominator").get<std::uint32_t>();
        calendar.lunarCycleDays = value.at("lunarCycleDays").get<std::uint32_t>();
    }
    if (!ValidCalendar(calendar))
        return false;
    out = calendar;
    return true;
}
bool ValidTick(std::uint64_t tick) {
    return tick < WorldClock::kTickLimit &&
           tick <= std::numeric_limits<std::uint64_t>::max() -
                       luminumbra::core::SimulationClock::kMaxCatchUpTicksPerFrame;
}
} // namespace

WorldClock::WorldClock(std::uint64_t tick, WorldCalendar calendar)
    : m_tick(tick)
    , m_calendar(calendar) {
    if (!ValidCalendar(calendar) || !ValidTick(tick))
        throw std::invalid_argument("Invalid world clock");
}

std::uint64_t WorldClock::year_length_ticks() const {
    return std::uint64_t{m_calendar.dayLengthTicks} * m_calendar.daysPerYear;
}
double WorldClock::day_phase(std::uint64_t tick) const {
    return static_cast<double>(tick % m_calendar.dayLengthTicks) / m_calendar.dayLengthTicks;
}
double WorldClock::solar_phase(std::uint64_t tick) const {
    if (m_calendar.daylightNumerator == 1 && m_calendar.daylightDenominator == 2)
        return day_phase(tick); // Preserve the symmetric arithmetic exactly.
    const std::uint64_t day = m_calendar.dayLengthTicks;
    const std::uint64_t n = m_calendar.daylightNumerator;
    const std::uint64_t d = m_calendar.daylightDenominator;
    const std::uint64_t x = 2 * d * (tick % day);
    const std::uint64_t dawn = day * (d - n);
    const std::uint64_t dusk = day * (d + n);
    std::uint64_t numerator;
    std::uint64_t denominator;
    if (x < dawn) {
        numerator = x;
        denominator = 4 * day * (d - n);
    } else if (x <= dusk) {
        numerator = day * n + x - dawn;
        denominator = 4 * day * n;
    } else {
        numerator = 3 * day * (d - n) + x - dusk;
        denominator = 4 * day * (d - n);
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}
std::uint64_t WorldClock::lunar_period_ticks() const {
    return std::uint64_t{m_calendar.dayLengthTicks} * m_calendar.lunarCycleDays;
}
double WorldClock::lunar_phase(std::uint64_t tick) const {
    return static_cast<double>(tick % lunar_period_ticks()) /
           static_cast<double>(lunar_period_ticks());
}
double WorldClock::year_phase(std::uint64_t tick) const {
    return static_cast<double>(tick % year_length_ticks()) /
           static_cast<double>(year_length_ticks());
}
std::uint64_t WorldClock::day_index(std::uint64_t tick) const {
    return tick / m_calendar.dayLengthTicks;
}
std::uint64_t WorldClock::year_index(std::uint64_t tick) const {
    return tick / year_length_ticks();
}
double WorldClock::spring_phase(std::uint64_t tick) const {
    const double shifted = year_phase(tick) + 0.75;
    return shifted >= 1.0 ? shifted - 1.0 : shifted;
}
void WorldClock::set_tick(std::uint64_t tick) {
    if (!ValidTick(tick))
        throw std::overflow_error("World clock tick limit reached");
    m_tick = tick;
}
bool WorldClock::can_advance(std::uint64_t ticks) const {
    return ticks < kTickLimit - m_tick;
}
std::string WorldClock::canonical_bytes() const {
    std::string bytes = "world_clock:v2:";
    const auto append = [&bytes](std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    };
    append(m_tick, 8);
    append(m_calendar.dayLengthTicks, 4);
    append(m_calendar.daysPerYear, 4);
    append(m_calendar.daylightNumerator, 4);
    append(m_calendar.daylightDenominator, 4);
    append(m_calendar.lunarCycleDays, 4);
    return bytes;
}
nlohmann::json WorldClock::calendar_metadata() const {
    return {{"version", 2},
            {"dayLengthTicks", m_calendar.dayLengthTicks},
            {"daysPerYear", m_calendar.daysPerYear},
            {"daylightNumerator", m_calendar.daylightNumerator},
            {"daylightDenominator", m_calendar.daylightDenominator},
            {"lunarCycleDays", m_calendar.lunarCycleDays}};
}
void WorldClock::write_metadata(nlohmann::json& metadata) const {
    metadata["simulationTick"] = m_tick;
    metadata["calendar"] = calendar_metadata();
}
bool WorldClock::read_render_calendar(const nlohmann::json& metadata,
                                      std::optional<WorldCalendar>& out,
                                      std::string& error) {
    if (!metadata.is_object()) {
        error = "Corrupt world metadata: invalid clock metadata object.";
        return false;
    }
    std::optional<WorldCalendar> resolved;
    if (metadata.contains("renderCalendar")) {
        WorldCalendar calendar;
        if (metadata.contains("calendar") || metadata.contains("simulationTick") ||
            !DecodeCalendar(metadata.at("renderCalendar"), calendar, false)) {
            error =
                "Corrupt world metadata: invalid renderCalendar or competing clock authorities.";
            return false;
        }
        resolved = calendar;
    }
    out = resolved;
    error.clear();
    return true;
}
bool WorldClock::from_metadata(const nlohmann::json& metadata,
                               WorldClock& out,
                               std::string& error) {
    const auto reject = [&error](const char* field) {
        error = std::string("Corrupt world metadata: invalid ") + field + ".";
        return false;
    };
    if (!metadata.is_object())
        return reject("clock metadata object");
    std::uint64_t tick = 0;
    WorldCalendar calendar = WorldCalendar::Legacy();
    std::optional<WorldCalendar> render_calendar;
    if (!read_render_calendar(metadata, render_calendar, error))
        return false;
    if (metadata.contains("simulationTick")) {
        const auto& value = metadata.at("simulationTick");
        if (!value.is_number_unsigned() || !ValidTick(value.get<std::uint64_t>()))
            return reject("simulationTick");
        tick = value.get<std::uint64_t>();
    }
    if (metadata.contains("calendar") && !DecodeCalendar(metadata.at("calendar"), calendar, true))
        return reject("calendar");
    out = WorldClock(tick, calendar);
    error.clear();
    return true;
}
} // namespace Luminumbra::world
