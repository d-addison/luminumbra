#include <gtest/gtest.h>

#include "../support/CalendarMetadataCases.h"

#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/core/SimulationClock.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/WorldClock.h"

#include <array>
#include <bit>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>

namespace {
using Luminumbra::world::WorldCalendar;
using Luminumbra::world::WorldClock;
using nlohmann::json;

TEST(WorldClockTest, MidnightMidwinterPhasesAndIndicesWrapExactly) {
    const WorldClock clock;
    EXPECT_EQ(clock.calendar(), WorldCalendar{});
    constexpr std::uint64_t day = WorldCalendar{}.dayLengthTicks;
    constexpr std::uint64_t year = day * WorldCalendar{}.daysPerYear;
    EXPECT_EQ(day, 162000u);
    EXPECT_EQ(clock.lunar_period_ticks(), 1296000u);
    EXPECT_EQ(clock.calendar().daylightNumerator, 2u);
    EXPECT_EQ(clock.calendar().daylightDenominator, 3u);
    EXPECT_EQ(WorldCalendar{}.daysPerYear, 8u);
    EXPECT_EQ(clock.year_length_ticks(), year);
    EXPECT_EQ(clock.day_phase(0), 0.0);
    EXPECT_EQ(clock.day_phase(day / 4), 0.25);
    EXPECT_EQ(clock.day_phase(day / 2), 0.5);
    EXPECT_EQ(clock.day_phase(day * 3 / 4), 0.75);
    EXPECT_EQ(clock.day_phase(day), 0.0);
    EXPECT_EQ(clock.year_phase(year / 4), 0.25);
    EXPECT_EQ(clock.year_phase(year / 2), 0.5);
    EXPECT_EQ(clock.year_phase(year * 3 / 4), 0.75);
    EXPECT_EQ(clock.year_phase(year), 0.0);
    EXPECT_EQ(clock.day_index(day - 1), 0u);
    EXPECT_EQ(clock.day_index(day), 1u);
    EXPECT_EQ(clock.day_index(year), 8u);
    EXPECT_EQ(clock.year_index(year - 1), 0u);
    EXPECT_EQ(clock.year_index(year), 1u);
    const auto last = WorldClock::kTickLimit - 1;
    EXPECT_EQ(clock.day_phase(last), static_cast<double>(last % day) / day);
    EXPECT_EQ(clock.year_index(last), last / year);
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
        std::string("world_clock:v2:") +
        std::string("\x08\x07\x06\x05\x04\x03\x02\x01\xa0\x8c\x00\x00\x08\x00\x00\x00"
                    "\x02\x00\x00\x00\x03\x00\x00\x00\x08\x00\x00\x00",
                    28);
    EXPECT_EQ(clock.canonical_bytes(), expected);
    const std::string default_expected =
        std::string("world_clock:v2:") +
        std::string("\x08\x07\x06\x05\x04\x03\x02\x01\xd0\x78\x02\x00\x08\x00\x00\x00"
                    "\x02\x00\x00\x00\x03\x00\x00\x00\x08\x00\x00\x00",
                    28);
    EXPECT_EQ(WorldClock(clock.tick()).canonical_bytes(), default_expected);
    json metadata;
    clock.write_metadata(metadata);
    WorldClock restored;
    std::string error;
    ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
    EXPECT_EQ(restored.canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick() + 1, clock.calendar()).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36001, 8}).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36000, 9}).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36000, 8, 1, 3, 8}).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36000, 8, 2, 5, 8}).canonical_bytes(), expected);
    EXPECT_NE(WorldClock(clock.tick(), {36000, 8, 2, 3, 7}).canonical_bytes(), expected);
}

TEST(WorldClockTest, LegacyCanonicalBytesMatchV1Exactly) {
    const WorldClock clock(12345, WorldCalendar::Legacy());
    // V1 stores tick (u64), day length (u32), and year length in days (u32), all LE.
    const std::string expected =
        std::string("world_clock:v1:") +
        std::string("\x39\x30\x00\x00\x00\x00\x00\x00\xc0\x4b\x03\x00\x08\x00\x00\x00", 16);
    EXPECT_EQ(clock.canonical_bytes(), expected);
}

TEST(WorldClockTest, LegacyMetadataRoundTripsAsExactTwoFieldV1Record) {
    for (const auto calendar : {WorldCalendar::Legacy(), WorldCalendar{36000, 3, 1, 2, 8}}) {
        SCOPED_TRACE(calendar.dayLengthTicks);
        const WorldClock clock(12345, calendar);
        const json expected = {
            {"simulationTick", 12345u},
            {"calendar",
             {{"dayLengthTicks", calendar.dayLengthTicks}, {"daysPerYear", calendar.daysPerYear}}}};
        json metadata;
        clock.write_metadata(metadata);
        EXPECT_EQ(metadata, expected);
        WorldClock restored;
        std::string error;
        ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(restored.tick(), 12345u);
        EXPECT_EQ(restored.calendar(), calendar);
        EXPECT_EQ(restored.canonical_bytes(), clock.canonical_bytes());
        EXPECT_EQ(restored.canonical_bytes().substr(0, 15), "world_clock:v1:");
        json rewritten;
        restored.write_metadata(rewritten);
        EXPECT_EQ(rewritten, expected);

        // Existing v2 saves with symmetric parameters still load without loss.
        metadata["calendar"] = clock.calendar_metadata();
        ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
        EXPECT_EQ(restored.calendar(), calendar);
        EXPECT_EQ(restored.canonical_bytes(), clock.canonical_bytes());
    }
}

TEST(WorldClockTest, ExtendedSolarOrLunarParametersRetainV2Encoding) {
    for (const auto calendar : {WorldCalendar{}, WorldCalendar{216000, 8, 1, 2, 7}}) {
        const WorldClock clock(12345, calendar);
        EXPECT_EQ(clock.canonical_bytes().substr(0, 15), "world_clock:v2:");
        EXPECT_EQ(clock.canonical_bytes().size(), 43u);
        json metadata;
        clock.write_metadata(metadata);
        EXPECT_EQ(metadata.at("calendar"), clock.calendar_metadata());
        EXPECT_EQ(metadata.at("calendar").size(), 6u);
        EXPECT_EQ(metadata.at("calendar").at("version"), 2);
        WorldClock restored;
        std::string error;
        ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
        EXPECT_EQ(restored.tick(), clock.tick());
        EXPECT_EQ(restored.calendar(), calendar);
        EXPECT_EQ(restored.canonical_bytes(), clock.canonical_bytes());
    }
}

TEST(WorldClockTest, MissingMetadataUsesPinnedDefaults) {
    WorldClock clock(42, {100, 3});
    std::string error;
    ASSERT_TRUE(WorldClock::from_metadata(json::object(), clock, error));
    EXPECT_EQ(clock.tick(), 0u);
    EXPECT_EQ(clock.calendar(), WorldCalendar::Legacy());
    ASSERT_TRUE(WorldClock::from_metadata(json::parse(R"({"simulationTick":13})"), clock, error));
    EXPECT_EQ(clock.tick(), 13u);
    EXPECT_EQ(clock.calendar(), WorldCalendar::Legacy());
    ASSERT_TRUE(WorldClock::from_metadata(
        json::parse(R"({"simulationTick":13,"calendar":{"dayLengthTicks":36000,"daysPerYear":8}})"),
        clock,
        error));
    EXPECT_EQ(clock.tick(), 13u);
    EXPECT_EQ(clock.calendar(), (WorldCalendar{36000, 8, 1, 2, 8}));
    EXPECT_EQ(clock.year_length_ticks(), 288000u);
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

TEST(WorldClockTest, SolarHorizonsUseIntegerBoundariesAtEverySeason) {
    const WorldClock clock;
    for (const auto base : {0ull, 162000ull, 324000ull, 648000ull, 972000ull}) {
        EXPECT_EQ(clock.solar_phase(base), 0.0);
        EXPECT_EQ(clock.solar_phase(base + 27000), 0.25);
        EXPECT_EQ(clock.solar_phase(base + 81000), 0.5);
        EXPECT_EQ(clock.solar_phase(base + 135000), 0.75);
        for (const auto horizon : {27000ull, 135000ull}) {
            const double anchor = horizon == 27000 ? 0.25 : 0.75;
            EXPECT_LT(clock.solar_phase(base + horizon - 1), anchor);
            EXPECT_GT(clock.solar_phase(base + horizon + 1), anchor);
            const WorldClock at(base + horizon);
            const auto season = Luminumbra::Rendering::ComputeSeason(at);
            const auto sun = Luminumbra::Rendering::ComputeSunGeometry(
                Luminumbra::Rendering::TimeOfDayFromWorldClock(at), season.sunDeclination);
            EXPECT_NEAR(sun.upFactor, 0.0f, 2e-7f);
        }
    }
    EXPECT_EQ(clock.day_phase(27000), 1.0 / 6.0);
    EXPECT_EQ(clock.day_phase(135000), 5.0 / 6.0);
}

TEST(WorldClockTest, SymmetricIdentityOddDaysAndLargeIntegerProducts) {
    for (const auto day : {1u, 101u, 36000u, 216000u, 2147483647u}) {
        const WorldClock symmetric(0, {day, 366, 1, 2, 366});
        const WorldClock warped(0, {day, 3, 32768, 65535, 366});
        EXPECT_EQ(warped.lunar_period_ticks(), std::uint64_t{day} * 366);
        for (const auto tick :
             std::array<std::uint64_t, 4>{0, 1, 100, WorldClock::kTickLimit - 1}) {
            EXPECT_EQ(symmetric.solar_phase(tick), symmetric.day_phase(tick));
            EXPECT_EQ(warped.solar_phase(tick), warped.solar_phase(tick % day));
            EXPECT_GE(warped.solar_phase(tick), 0.0);
            EXPECT_LT(warped.solar_phase(tick), 1.0);
        }
    }
    const WorldClock odd(0, {101, 3, 2, 3, 8});
    EXPECT_LT(odd.solar_phase(16), .25);
    EXPECT_GT(odd.solar_phase(17), .25);
    EXPECT_LT(odd.solar_phase(84), .75);
    EXPECT_GT(odd.solar_phase(85), .75);
    EXPECT_EQ(odd.solar_phase(101), 0.0);
}

TEST(WorldClockTest, SmoothSolarPhaseIsStrictlyMonotonicAcrossDenseTickSweeps) {
    for (const auto calendar : {WorldCalendar{},
                                WorldCalendar::Legacy(),
                                WorldCalendar{120000, 8, 1, 3, 8},
                                WorldCalendar{120000, 8, 4, 5, 8},
                                WorldCalendar{131070, 8, 1, 65535, 8},
                                WorldCalendar{131070, 8, 65534, 65535, 8},
                                WorldCalendar{101, 8, 2, 3, 8},
                                WorldCalendar{2147483647u, 8, 32768, 65535, 8}}) {
        SCOPED_TRACE(calendar.dayLengthTicks);
        SCOPED_TRACE(calendar.daylightNumerator);
        const WorldClock clock(0, calendar);
        const std::uint64_t day = calendar.dayLengthTicks;
        const std::uint64_t samples = day < 262144 ? day : 262144;
        double previous = -1.0;
        for (std::uint64_t index = 0; index < samples; ++index) {
            const auto tick = day * index / samples;
            const double phase = clock.solar_phase(tick);
            ASSERT_GT(phase, previous) << tick;
            ASSERT_LT(phase, 1.0) << tick;
            EXPECT_NEAR(phase, clock.solar_phase_from_day_phase(clock.day_phase(tick)), 5e-15);
            previous = phase;
        }
        EXPECT_EQ(clock.solar_phase(day), 0.0);
    }
}

void CheckSolarKnotsAndRates(std::uint32_t numerator, std::uint32_t denominator) {
    // Even the shortest quarter has 8192 ticks, including extreme valid shares.
    const std::uint64_t day = std::uint64_t{2} * denominator * 8192;
    const WorldClock clock(0, {static_cast<std::uint32_t>(day), 8, numerator, denominator, 8});
    const std::uint64_t dawn = 8192ull * (denominator - numerator);
    const std::uint64_t dusk = 8192ull * (denominator + numerator);
    const std::array<std::uint64_t, 5> knots{0, dawn, day / 2, dusk, day};
    for (std::size_t index = 0; index < knots.size(); ++index) {
        const auto knot = knots[index];
        const double anchor = static_cast<double>(index) * .25;
        EXPECT_EQ(clock.solar_phase(knot), index == 4 ? 0.0 : anchor);
        // Unwrap around the second day so both sides of midnight are ordinary samples.
        const auto unwrapped = [&clock, day](std::uint64_t tick) {
            return clock.solar_phase(tick) + static_cast<double>(tick / day);
        };
        const auto boundary = day + knot;
        const double center = unwrapped(boundary);
        EXPECT_LT(unwrapped(boundary - 1), center);
        EXPECT_GT(unwrapped(boundary + 1), center);
        const double shortest =
            8192.0 * (numerator < denominator - numerator ? numerator : denominator - numerator);
        const double continuityTolerance =
            1.0 / static_cast<double>(day) + .75 / (shortest * shortest) + 2e-15;
        EXPECT_NEAR(unwrapped(boundary - 1), center, continuityTolerance);
        EXPECT_NEAR(unwrapped(boundary + 1), center, continuityTolerance);
        // One-sided cubic finite differences cancel curvature exactly. This also
        // resolves the common endpoint rate for very short daylight/night spans.
        const double left = (11.0 * center - 18.0 * unwrapped(boundary - 1) +
                             9.0 * unwrapped(boundary - 2) - 2.0 * unwrapped(boundary - 3)) /
                            6.0;
        const double right = (-11.0 * center + 18.0 * unwrapped(boundary + 1) -
                              9.0 * unwrapped(boundary + 2) + 2.0 * unwrapped(boundary + 3)) /
                             6.0;
        EXPECT_NEAR(left * static_cast<double>(day), 1.0, 2e-6);
        EXPECT_NEAR(right * static_cast<double>(day), 1.0, 2e-6);
        EXPECT_NEAR(left * static_cast<double>(day), right * static_cast<double>(day), 2e-6);
    }
}

TEST(WorldClockTest, SmoothSolarKnotsHaveExactPhasesAndContinuousRates) {
    for (std::uint32_t denominator = 2; denominator <= 32; ++denominator) {
        for (std::uint32_t numerator = 1; numerator < denominator; ++numerator) {
            if (std::gcd(numerator, denominator) == 1) {
                SCOPED_TRACE(numerator);
                SCOPED_TRACE(denominator);
                CheckSolarKnotsAndRates(numerator, denominator);
            }
        }
    }
    for (const auto numerator : {1u, 32768u, 65534u}) {
        SCOPED_TRACE(numerator);
        CheckSolarKnotsAndRates(numerator, 65535);
    }
    const WorldClock clock;
    constexpr std::uint64_t day = WorldCalendar{}.dayLengthTicks;
    for (const auto boundary : {day, day + 27000, day + 81000, day + 135000}) {
        const auto unwrapped = [&clock](std::uint64_t tick) {
            return clock.solar_phase(tick) + static_cast<double>(tick / day);
        };
        const double left =
            (unwrapped(boundary) - unwrapped(boundary - 1)) * static_cast<double>(day);
        const double right =
            (unwrapped(boundary + 1) - unwrapped(boundary)) * static_cast<double>(day);
        EXPECT_NEAR(left, right, 2e-4);
        EXPECT_NEAR(left, 1.0, 2e-4);
        EXPECT_NEAR(right, 1.0, 2e-4);
    }
}

TEST(WorldClockTest, SmoothSolarDaylightShareIsExactToOneTickInEverySeason) {
    for (const auto calendar : {WorldCalendar{},
                                WorldCalendar{132000, 8, 3, 11, 8},
                                WorldCalendar{132000, 8, 10, 11, 8},
                                WorldCalendar{101, 8, 2, 3, 8}}) {
        const WorldClock clock(0, calendar);
        const std::uint64_t day = calendar.dayLengthTicks;
        for (std::uint64_t season = 0; season < calendar.daysPerYear; ++season) {
            SCOPED_TRACE(season);
            std::uint64_t daylight = 0;
            for (std::uint64_t tick = season * day; tick < (season + 1) * day; ++tick) {
                const double phase = clock.solar_phase(tick);
                if (phase >= .25 && phase < .75)
                    ++daylight;
            }
            EXPECT_NEAR(static_cast<double>(daylight) / static_cast<double>(day),
                        static_cast<double>(calendar.daylightNumerator) /
                            calendar.daylightDenominator,
                        1.0 / static_cast<double>(day));
        }
    }
}

TEST(WorldClockTest, SmoothSolarPresentationInverseAndMetadataRoundTripShareTheTickCurve) {
    namespace sky = Luminumbra::Rendering;
    for (const auto calendar : {WorldCalendar{},
                                WorldCalendar::Legacy(),
                                WorldCalendar{131070, 8, 1, 65535, 8},
                                WorldCalendar{131070, 8, 65534, 65535, 8}}) {
        const WorldClock clock(12345, calendar);
        json metadata;
        clock.write_metadata(metadata);
        WorldClock restored;
        std::string error;
        ASSERT_TRUE(WorldClock::from_metadata(json::parse(metadata.dump()), restored, error));
        EXPECT_EQ(restored.canonical_bytes(), clock.canonical_bytes());
        for (const auto tick :
             std::array<std::uint64_t, 5>{0, 1, 12345, 81001, WorldClock::kTickLimit - 1}) {
            const double solar = clock.solar_phase(tick);
            EXPECT_EQ(std::bit_cast<std::uint64_t>(restored.solar_phase(tick)),
                      std::bit_cast<std::uint64_t>(solar));
            EXPECT_NEAR(clock.day_phase_from_solar_phase(solar), clock.day_phase(tick), 5e-12);
            const double noonElapsed = sky::WrapDayPhase(clock.day_phase(tick) + .5);
            const float noonSolar = static_cast<float>(sky::WrapDayPhase(solar + .5));
            EXPECT_NEAR(sky::SolarTimeFromNoonElapsed(noonElapsed, calendar), noonSolar, 1e-7f);
        }
        for (const auto solar : {0.0, .1, .25, .3, .5, .75, .9, 1.0}) {
            const double elapsed = sky::ElapsedPhaseFromSolar(solar, calendar);
            EXPECT_NEAR(sky::SolarPhaseFromElapsed(elapsed, calendar), solar, 5e-12);
        }
    }
    const WorldClock symmetric(0, WorldCalendar::Legacy());
    for (std::uint64_t tick = 0; tick < symmetric.calendar().dayLengthTicks; ++tick) {
        const double legacy = static_cast<double>(tick) / symmetric.calendar().dayLengthTicks;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(symmetric.solar_phase(tick)),
                  std::bit_cast<std::uint64_t>(legacy));
        EXPECT_EQ(symmetric.solar_phase_from_day_phase(legacy), legacy);
        EXPECT_EQ(symmetric.day_phase_from_solar_phase(legacy), legacy);
    }
}

TEST(WorldClockTest, LunarMonthIsIndependentOfYearAndImportsRecordedDayLength) {
    const WorldClock clock(0, {162000, 3, 2, 3, 8});
    EXPECT_EQ(clock.lunar_phase(0), 0.0);
    EXPECT_EQ(clock.lunar_phase(4 * 162000), 0.5);
    EXPECT_EQ(clock.lunar_phase(8 * 162000), 0.0);
    EXPECT_EQ(clock.lunar_phase(clock.year_length_ticks()), 3.0 / 8.0);
    const auto last = WorldClock::kTickLimit - 1;
    EXPECT_EQ(clock.lunar_phase(last), static_cast<double>(last % 1296000) / 1296000.0);
    for (const auto day : {36000u, 216000u}) {
        WorldClock imported;
        std::string error;
        ASSERT_TRUE(WorldClock::from_metadata(
            {{"calendar", {{"dayLengthTicks", day}, {"daysPerYear", 3}}}}, imported, error));
        EXPECT_EQ(imported.lunar_period_ticks(), std::uint64_t{day} * 8);
        EXPECT_EQ(imported.solar_phase(day / 4), .25);
    }
}

TEST(WorldClockTest, StrictV2AndMarkerRefusalMatrixLeavesOutputUnchanged) {
    for (const auto& metadata : Luminumbra::TestSupport::InvalidCalendarMetadata()) {
        SCOPED_TRACE(metadata.dump());
        WorldClock clock(123);
        const auto before = clock.canonical_bytes();
        std::string error;
        EXPECT_FALSE(WorldClock::from_metadata(metadata, clock, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(clock.canonical_bytes(), before);
    }
    const json metadata = {{"renderCalendar", Luminumbra::TestSupport::DefaultCalendarMetadata()}};
    WorldClock clock;
    std::string error;
    ASSERT_TRUE(WorldClock::from_metadata(metadata, clock, error));
    EXPECT_EQ(clock.calendar(), WorldCalendar::Legacy());
    std::optional<WorldCalendar> marker;
    ASSERT_TRUE(WorldClock::read_render_calendar(metadata, marker, error));
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker, std::optional<WorldCalendar>{WorldCalendar{}});
}

TEST(WorldClockTest, ConstructorChecksRationalAndLunarBounds) {
    for (const auto calendar : {WorldCalendar{162000, 8, 0, 3, 8},
                                WorldCalendar{162000, 8, 3, 3, 8},
                                WorldCalendar{162000, 8, 2, 0, 8},
                                WorldCalendar{162000, 8, 4, 6, 8},
                                WorldCalendar{162000, 8, 1, 65536, 8},
                                WorldCalendar{162000, 8, 2, 3, 0},
                                WorldCalendar{162000, 8, 2, 3, 367}})
        EXPECT_THROW(WorldClock(0, calendar), std::invalid_argument);
    EXPECT_NO_THROW(WorldClock(0, (WorldCalendar{1, 1, 65534, 65535, 1})));
}

TEST(WorldClockTest, StimulusSolarLightTemperatureAndOverridesPreserveFallbacks) {
    namespace ai = luminumbra::ai;
    namespace dm = Luminumbra::DeterministicMath;
    for (const auto tick :
         {26999ull, 27000ull, 27001ull, 81000ull, 134999ull, 135000ull, 135001ull, 162000ull}) {
        const WorldClock clock(tick);
        ai::StimulusContext context;
        context.tick = tick;
        context.world_clock = &clock;
        context.precip_override = .4f;
        const ai::StimulusChannelRegistry sample(context);
        const auto activity =
            ai::CircadianActivity(static_cast<float>(clock.solar_phase(tick)), false);
        EXPECT_FLOAT_EQ(sample.Sample(ai::StimulusChannel::TimeOfDay), activity);
        EXPECT_FLOAT_EQ(sample.Sample(ai::StimulusChannel::LightLevel),
                        activity * (1.0f - .4f * .4f));
        const float season = dm::Sin(static_cast<float>(clock.spring_phase(tick)) * dm::kTwoPi);
        const float temperature = .5f + .25f * season + .12f * (activity - .5f) - .15f * .4f;
        EXPECT_FLOAT_EQ(sample.Sample(ai::StimulusChannel::Temperature), temperature);
        context.ambient_light = .8f;
        EXPECT_FLOAT_EQ(
            ai::StimulusChannelRegistry(context).Sample(ai::StimulusChannel::LightLevel),
            .8f * (1.0f - .4f * .4f));
        context.world_clock = nullptr;
        const float legacy = static_cast<float>(static_cast<double>(tick % ai::kTicksPerDayCycle) /
                                                ai::kTicksPerDayCycle);
        EXPECT_FLOAT_EQ(ai::StimulusChannelRegistry(context).Sample(ai::StimulusChannel::TimeOfDay),
                        ai::CircadianActivity(legacy, false));
    }
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
    EXPECT_EQ(simulation.advance(simulation.fixed_dt()), 1u);
    EXPECT_EQ(simulation.tick_count(), 0u); // legacy unsigned wrap, guarded by enabled WorldClock
}

TEST(WorldClockTest, ActiveRegionsIsGeneratedHashedAndDefaultOn) {
    using namespace luminumbra::core;
    const SystemConfig defaults;
    EXPECT_TRUE(defaults.enabled(SysKey::SimActiveRegions));
    EXPECT_FALSE(defaults.ComputeConfigSubHash().empty());
    const auto enabled =
        SystemConfig::FromJsonString(R"({"sim":{"active_regions":{"enabled":true}}})");
    EXPECT_TRUE(enabled.enabled(SysKey::SimActiveRegions));
    EXPECT_FALSE(enabled.ComputeConfigSubHash().empty());
}

TEST(WorldClockTest, ActiveRegionsExplicitConfigurationIsHashed) {
    using namespace luminumbra::core;
    const auto defaults =
        SystemConfig::FromJsonString(R"({"sim":{"active_regions":{"enabled":false}}})");
    EXPECT_FALSE(defaults.enabled(SysKey::SimActiveRegions));

    const auto enabled =
        SystemConfig::FromJsonString(R"({"sim":{"active_regions":{"enabled":true}}})");
    EXPECT_TRUE(enabled.enabled(SysKey::SimActiveRegions));
    EXPECT_NE(enabled.ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
}

TEST(WorldClockTest, SkyCircadianAndStimulusSharePinnedCalendarPhases) {
    namespace ai = luminumbra::ai;
    namespace sky = Luminumbra::Rendering;
    constexpr std::uint64_t day = WorldCalendar{}.dayLengthTicks;
    constexpr std::uint64_t year = day * WorldCalendar{}.daysPerYear;
    for (const auto tick : {std::uint64_t{0},
                            day / 4,
                            day / 2,
                            day * 3 / 4,
                            day,
                            year / 4,
                            year / 2,
                            year * 3 / 4,
                            year}) {
        SCOPED_TRACE(tick);
        const WorldClock clock(tick);
        const float day_phase = static_cast<float>(clock.solar_phase(tick));
        ai::StimulusContext context;
        context.tick = tick;
        context.world_clock = &clock;
        const ai::StimulusChannelRegistry stimulus(context);
        EXPECT_FLOAT_EQ(stimulus.Sample(ai::StimulusChannel::TimeOfDay),
                        ai::CircadianActivity(day_phase, false));
        const auto season = sky::ComputeSeason(clock);
        EXPECT_FLOAT_EQ(season.phase, static_cast<float>(clock.spring_phase(tick)));
        EXPECT_NEAR(
            stimulus.Sample(ai::StimulusChannel::Season), (season.wave + 1.0f) * 0.5f, 1e-6f);
        const float sky_day = sky::TimeOfDayFromWorldClock(clock);
        EXPECT_FLOAT_EQ(sky_day, day_phase >= 0.5f ? day_phase - 0.5f : day_phase + 0.5f);
        const auto sun = sky::ComputeSunGeometry(sky_day, season.sunDeclination);
        if (day_phase == 0.0f) {
            EXPECT_LT(sun.upFactor, 0.0f);
        }
        if (day_phase == 0.5f) {
            EXPECT_GT(sun.upFactor, 0.0f);
        }
    }
    EXPECT_NEAR(sky::ComputeSeason(WorldClock(0)).wave, -1.0f, 1e-6f);
    EXPECT_NEAR(sky::ComputeSeason(WorldClock(year / 2)).wave, 1.0f, 1e-6f);
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
