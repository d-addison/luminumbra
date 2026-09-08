#include <gtest/gtest.h>

#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/core/SimulationClock.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/WorldClock.h"

#include <limits>
#include <nlohmann/json.hpp>

namespace {
using Luminumbra::world::WorldCalendar;
using Luminumbra::world::WorldClock;
using nlohmann::json;

TEST(WorldClockTest, MidnightMidwinterPhasesAndIndicesWrapExactly) {
    const WorldClock clock;
    EXPECT_EQ(clock.calendar(), WorldCalendar{});
    EXPECT_EQ(clock.year_length_ticks(), 288000u);
    EXPECT_EQ(clock.day_phase(0), 0.0);
    EXPECT_EQ(clock.day_phase(9000), 0.25);
    EXPECT_EQ(clock.day_phase(18000), 0.5);
    EXPECT_EQ(clock.day_phase(27000), 0.75);
    EXPECT_EQ(clock.day_phase(36000), 0.0);
    EXPECT_EQ(clock.year_phase(72000), 0.25);
    EXPECT_EQ(clock.year_phase(144000), 0.5);
    EXPECT_EQ(clock.year_phase(216000), 0.75);
    EXPECT_EQ(clock.year_phase(288000), 0.0);
    EXPECT_EQ(clock.day_index(35999), 0u);
    EXPECT_EQ(clock.day_index(36000), 1u);
    EXPECT_EQ(clock.day_index(288000), 8u);
    EXPECT_EQ(clock.year_index(287999), 0u);
    EXPECT_EQ(clock.year_index(288000), 1u);
    const auto last = WorldClock::kTickLimit - 1;
    EXPECT_EQ(clock.day_phase(last), static_cast<double>(last % 36000) / 36000);
    EXPECT_EQ(clock.year_index(last), last / 288000);
}

TEST(WorldClockTest, CustomCalendarAndCheckedTickLimits) {
    WorldClock clock(WorldClock::kTickLimit - 5, {1, 366});
    EXPECT_EQ(clock.day_phase(123), 0.0);
    EXPECT_EQ(clock.year_phase(183), 0.5);
    EXPECT_EQ(clock.year_index(732), 2u);
    EXPECT_TRUE(clock.can_advance(4));
    EXPECT_FALSE(clock.can_advance(5));
    EXPECT_FALSE(clock.can_advance(std::numeric_limits<std::uint64_t>::max()));
    EXPECT_THROW(clock.set_tick(WorldClock::kTickLimit), std::overflow_error);
    EXPECT_THROW(WorldClock(0, (WorldCalendar{0, 8})), std::invalid_argument);
    const WorldClock largest(0, {2147483647u, 366});
    EXPECT_EQ(largest.year_length_ticks(), 785979014802ull);
}

TEST(WorldClockTest, CanonicalBytesAreTaggedLittleEndianAndIgnoreFrameTime) {
    const WorldClock clock(0x0102030405060708ull, {36000, 8});
    const std::string expected =
        std::string("world_clock:v1:") +
        std::string("\x08\x07\x06\x05\x04\x03\x02\x01\xa0\x8c\x00\x00\x08\x00\x00\x00", 16);
    EXPECT_EQ(clock.canonical_bytes(), expected);
    json metadata;
    clock.write_metadata(metadata);
    WorldClock restored;
    std::string error;
    ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
    EXPECT_EQ(restored.canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick() + 1).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36001, 8}).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36000, 9}).canonical_bytes(), expected);
}

TEST(WorldClockTest, MissingMetadataUsesPinnedDefaults) {
    WorldClock clock(42, {100, 3});
    std::string error;
    ASSERT_TRUE(WorldClock::from_metadata(json::object(), clock, error));
    EXPECT_EQ(clock.tick(), 0u);
    EXPECT_EQ(clock.calendar(), WorldCalendar{});
    ASSERT_TRUE(WorldClock::from_metadata(json::parse(R"({"simulationTick":13})"), clock, error));
    EXPECT_EQ(clock.tick(), 13u);
    EXPECT_EQ(clock.calendar(), WorldCalendar{});
    ASSERT_TRUE(WorldClock::from_metadata(
        json::parse(R"({"calendar":{"dayLengthTicks":1,"daysPerYear":1}})"), clock, error));
    EXPECT_EQ(clock.tick(), 0u);
    EXPECT_EQ(clock.year_length_ticks(), 1u);
    ASSERT_TRUE(WorldClock::from_metadata(
        json::parse(
            R"({"simulationTick":4611686018427387903,"calendar":{"dayLengthTicks":2147483647,"daysPerYear":366}})"),
        clock,
        error));
    EXPECT_EQ(clock.tick(), WorldClock::kTickLimit - 1);
}

class WorldClockInvalidMetadata : public testing::TestWithParam<const char*> {};
TEST_P(WorldClockInvalidMetadata, RefusesWithoutChangingClock) {
    WorldClock clock(123);
    const auto before = clock.canonical_bytes();
    std::string error;
    EXPECT_FALSE(WorldClock::from_metadata(json::parse(GetParam()), clock, error));
    EXPECT_NE(error.find("Corrupt world metadata"), std::string::npos);
    EXPECT_EQ(clock.canonical_bytes(), before);
}
INSTANTIATE_TEST_SUITE_P(
    ValidationMatrix,
    WorldClockInvalidMetadata,
    testing::Values("[]",
                    "null",
                    "true",
                    R"({"simulationTick":-1})",
                    R"({"simulationTick":1.0})",
                    R"({"simulationTick":1e2})",
                    R"({"simulationTick":"1"})",
                    R"({"simulationTick":null})",
                    R"({"simulationTick":false})",
                    R"({"simulationTick":[]})",
                    R"({"simulationTick":{}})",
                    R"({"simulationTick":4611686018427387904})",
                    R"({"simulationTick":18446744073709551615})",
                    R"({"simulationTick":18446744073709551616})",
                    R"({"calendar":null})",
                    R"({"calendar":[]})",
                    R"({"calendar":true})",
                    R"({"calendar":"8"})",
                    R"({"calendar":{}})",
                    R"({"calendar":{"dayLengthTicks":36000}})",
                    R"({"calendar":{"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":8,"extra":1}})",
                    R"({"calendar":{"dayLengthTicks":0,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":-1,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":2147483648,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":18446744073709551616,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":36000.0,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":"36000","daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":true,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":null,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":0}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":-1}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":367}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":8.0}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":"8"}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":false}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":null}})"));

TEST(WorldClockTest, SimulationClockResetRestoresBaseAndClearsFrameRemainder) {
    luminumbra::core::SimulationClock simulation;
    simulation.advance(0.5);
    simulation.reset(123456);
    EXPECT_EQ(simulation.tick_count(), 123456u);
    EXPECT_EQ(simulation.accumulator(), 0.0);
    EXPECT_EQ(simulation.dropped_frame_count(), 0u);
    EXPECT_EQ(simulation.dropped_time_seconds(), 0.0);
    EXPECT_EQ(simulation.advance(simulation.fixed_dt()), 1u);
    EXPECT_EQ(simulation.tick_count(), 123457u);
    simulation.reset();
    EXPECT_EQ(simulation.tick_count(), 0u);
    simulation.reset(std::numeric_limits<std::uint64_t>::max());
    EXPECT_THROW(simulation.advance(simulation.fixed_dt()), std::overflow_error);
}

TEST(WorldClockTest, ActiveRegionsIsGeneratedHashedAndDefaultOff) {
    using namespace luminumbra::core;
    const SystemConfig defaults;
    EXPECT_FALSE(defaults.enabled(SysKey::SimActiveRegions));
    EXPECT_TRUE(defaults.ComputeConfigSubHash().empty());
    const auto enabled =
        SystemConfig::FromJsonString(R"({"sim":{"active_regions":{"enabled":true}}})");
    EXPECT_TRUE(enabled.enabled(SysKey::SimActiveRegions));
    EXPECT_FALSE(enabled.ComputeConfigSubHash().empty());
}

TEST(WorldClockTest, SkyCircadianAndStimulusSharePinnedCalendarPhases) {
    namespace ai = luminumbra::ai;
    namespace sky = Luminumbra::Rendering;
    for (const auto tick :
         {0ull, 9000ull, 18000ull, 27000ull, 36000ull, 72000ull, 144000ull, 216000ull, 288000ull}) {
        SCOPED_TRACE(tick);
        const WorldClock clock(tick);
        const float day = static_cast<float>(clock.day_phase(tick));
        ai::StimulusContext context;
        context.tick = tick;
        context.world_clock = &clock;
        const ai::StimulusChannelRegistry stimulus(context);
        EXPECT_FLOAT_EQ(stimulus.Sample(ai::StimulusChannel::TimeOfDay),
                        ai::CircadianActivity(day, false));
        const auto season = sky::ComputeSeason(clock);
        EXPECT_FLOAT_EQ(season.phase, static_cast<float>(clock.spring_phase(tick)));
        EXPECT_NEAR(
            stimulus.Sample(ai::StimulusChannel::Season), (season.wave + 1.0f) * 0.5f, 1e-6f);
        const float sky_day = sky::TimeOfDayFromWorldClock(clock);
        EXPECT_FLOAT_EQ(sky_day, day >= 0.5f ? day - 0.5f : day + 0.5f);
        const auto sun = sky::ComputeSunGeometry(sky_day, season.sunDeclination);
        if (day == 0.0f)
            EXPECT_LT(sun.upFactor, 0.0f);
        if (day == 0.5f)
            EXPECT_GT(sun.upFactor, 0.0f);
    }
    EXPECT_NEAR(sky::ComputeSeason(WorldClock(0)).wave, -1.0f, 1e-6f);
    EXPECT_NEAR(sky::ComputeSeason(WorldClock(144000)).wave, 1.0f, 1e-6f);
}

TEST(WorldClockTest, BoundedWeatherRestorationMatchesStormMotionAndStrikes) {
    using namespace Luminumbra::Systems;
    const Luminumbra::Vec3 anchor(8, 12, 8);
    WindFieldSystem wind(1337), restored_wind(1337);
    WeatherSystem weather(1337), restored(1337);
    weather.UpdateFromClock(0, anchor, wind);
    std::uint64_t previous = 0;
    for (const auto tick : {0ull, 45ull, 90ull, 239ull, 240ull, 241ull, 317ull, 360ull, 720ull}) {
        SCOPED_TRACE(tick);
        for (auto t = previous + 1; t <= tick; ++t)
            weather.UpdateFromClock(t, anchor, wind);
        restored.RestoreAtTick(tick, anchor, restored_wind);
        EXPECT_EQ(restored.ComputeWeatherSubHash(), weather.ComputeWeatherSubHash());
        EXPECT_EQ(restored_wind.ComputeWindSubHash(), wind.ComputeWindSubHash());
        restored.RestoreAtTick(tick, anchor, restored_wind);
        EXPECT_EQ(restored.ComputeWeatherSubHash(), weather.ComputeWeatherSubHash());
        EXPECT_EQ(restored_wind.ComputeWindSubHash(), wind.ComputeWindSubHash());
        previous = tick;
    }
    // Reusing an explicit snapshot loader must also remove later strike history.
    restored.RestoreAtTick(360, anchor, restored_wind);
    WeatherSystem fresh(1337);
    WindFieldSystem fresh_wind(1337);
    fresh.RestoreAtTick(360, anchor, fresh_wind);
    EXPECT_EQ(restored.ComputeWeatherSubHash(), fresh.ComputeWeatherSubHash());
    EXPECT_EQ(restored_wind.ComputeWindSubHash(), fresh_wind.ComputeWindSubHash());
}
} // namespace
