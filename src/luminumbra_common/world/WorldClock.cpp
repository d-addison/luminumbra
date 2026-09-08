#include "WorldClock.h"

#include "../core/SimulationClock.h"

#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Luminumbra::world {
namespace {
bool ValidCalendar(WorldCalendar calendar) {
    return calendar.dayLengthTicks > 0 && calendar.dayLengthTicks < (std::uint64_t{1} << 31) &&
           calendar.daysPerYear > 0 && calendar.daysPerYear <= 366 &&
           calendar.dayLengthTicks <=
               std::numeric_limits<std::uint64_t>::max() / calendar.daysPerYear;
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
    const double phase = year_phase(tick) + 0.75;
    return phase >= 1.0 ? phase - 1.0 : phase;
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
    std::string bytes = "world_clock:v1:";
    const auto append = [&bytes](std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    };
    append(m_tick, 8);
    append(m_calendar.dayLengthTicks, 4);
    append(m_calendar.daysPerYear, 4);
    return bytes;
}
void WorldClock::write_metadata(nlohmann::json& metadata) const {
    metadata["simulationTick"] = m_tick;
    metadata["calendar"] = {{"dayLengthTicks", m_calendar.dayLengthTicks},
                            {"daysPerYear", m_calendar.daysPerYear}};
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
    WorldCalendar calendar;
    if (metadata.contains("simulationTick")) {
        const auto& value = metadata.at("simulationTick");
        if (!value.is_number_unsigned() || !ValidTick(value.get<std::uint64_t>()))
            return reject("simulationTick");
        tick = value.get<std::uint64_t>();
    }
    if (metadata.contains("calendar")) {
        const auto& value = metadata.at("calendar");
        if (!value.is_object() || value.size() != 2 || !value.contains("dayLengthTicks") ||
            !value.contains("daysPerYear"))
            return reject("calendar");
        const auto& day = value.at("dayLengthTicks");
        const auto& year = value.at("daysPerYear");
        if (!day.is_number_integer() || day < 1 || day >= (std::uint64_t{1} << 31) ||
            !year.is_number_integer() || year < 1 || year > 366)
            return reject("calendar");
        calendar = {day.get<std::uint32_t>(), year.get<std::uint32_t>()};
    }
    if (!ValidCalendar(calendar))
        return reject("calendar product");
    out = WorldClock(tick, calendar);
    error.clear();
    return true;
}
} // namespace Luminumbra::world
