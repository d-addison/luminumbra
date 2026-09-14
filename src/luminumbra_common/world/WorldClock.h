#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Luminumbra::world {

struct WorldCalendar {
    std::uint32_t dayLengthTicks = 36000;
    std::uint32_t daysPerYear = 8;
    bool operator==(const WorldCalendar&) const = default;
};

// Absolute completed simulation tick, independent of frame time. Tick zero is
// midnight on day zero in midwinter. Calendar arithmetic is integer-first.
class WorldClock {
public:
    static constexpr std::uint64_t kTickLimit = std::uint64_t{1} << 62;
    explicit WorldClock(std::uint64_t tick = 0, WorldCalendar calendar = {});

    [[nodiscard]] std::uint64_t tick() const {
        return m_tick;
    }
    [[nodiscard]] const WorldCalendar& calendar() const {
        return m_calendar;
    }
    [[nodiscard]] std::uint64_t year_length_ticks() const;
    [[nodiscard]] double day_phase(std::uint64_t tick) const;
    [[nodiscard]] double year_phase(std::uint64_t tick) const;
    [[nodiscard]] std::uint64_t day_index(std::uint64_t tick) const;
    [[nodiscard]] std::uint64_t year_index(std::uint64_t tick) const;
    // Existing seasonal consumers use spring equinox as zero; midwinter is .75.
    [[nodiscard]] double spring_phase(std::uint64_t tick) const;
    void set_tick(std::uint64_t tick);
    [[nodiscard]] bool can_advance(std::uint64_t ticks) const;

    // Tagged canonical bytes: ASCII world_clock:v1:, then LE u64 tick,
    // LE u32 dayLengthTicks, LE u32 daysPerYear. No frame accumulator.
    [[nodiscard]] std::string canonical_bytes() const;
    void write_metadata(nlohmann::json& metadata) const;
    // Absent top-level keys use pinned defaults; a present calendar is complete.
    // Failure leaves out unchanged and supplies a corrupt-metadata diagnostic.
    static bool from_metadata(const nlohmann::json& metadata, WorldClock& out, std::string& error);

private:
    std::uint64_t m_tick;
    WorldCalendar m_calendar;
};

} // namespace Luminumbra::world
