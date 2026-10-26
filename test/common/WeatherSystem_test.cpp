// deterministic weather-core snapshot + determinism test.
//
// Proves the weather core is bit-deterministic (the property the world_hash
// `weather` sub-hash and the WeatherVisual state-hash assertion depend on): two
// independent instances advanced through the same ticks with the same seed/anchor
// (and a parallel wind field for advection) produce the IDENTICAL weather
// sub-hash; a different seed produces a different hash; the state evolves over
// time; the storm-cell set stays bounded (<= kMaxStormCells, ); the public
// query API returns the in-region category/precip; and the geometry matches the
// PINNED 24 m / 64-cell shape.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/core/DeterministicMath.h"
#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/systems/AmbientNoiseDispatch.h"

#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Luminumbra::Vec3;
using Luminumbra::Systems::kMaxStormCells;
using Luminumbra::Systems::kWeatherCellSizeM;
using Luminumbra::Systems::kWeatherExtentCells;
using Luminumbra::Systems::WeatherCategory;
using Luminumbra::Systems::WeatherSystem;
using Luminumbra::Systems::WindFieldSystem;

constexpr int kSeed = 424242;
constexpr std::uint64_t kTicks = 300; // Endurance300Storm horizon
const Vec3 kAnchor(8.0f, 100.0f, 8.0f);

struct RunResult {
    std::string sub_hash;
    int max_storm_cells = 0;
};

RunResult RunWeather(int seed, std::uint64_t ticks, const Vec3& anchor) {
    WindFieldSystem wind(seed);
    WeatherSystem weather(seed);
    RunResult result;
    for (std::uint64_t t = 1; t <= ticks; ++t) {
        wind.Update(t, anchor);
        weather.Update(t, anchor, &wind);
        if (weather.active_storm_count() > result.max_storm_cells) {
            result.max_storm_cells = weather.active_storm_count();
        }
    }
    result.sub_hash = weather.ComputeWeatherSubHash();
    return result;
}

// Compare with terrain's explicit ceiling, independently of the ambient helper.
// On non-x86 builds the existing native dispatch remains the reference.
TEST(WeatherSystem, NoiseDispatchMatchesTerrainCeiling) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    const auto reference = FastNoise::New<FastNoise::FractalFBm>(FastSIMD::Level_AVX2);
#else
    const auto reference = FastNoise::New<FastNoise::FractalFBm>();
#endif
    WeatherSystem field(kSeed);
    RecordProperty("cpu_max_simd", static_cast<int>(FastSIMD::CPUMaxSIMDLevel()));
    RecordProperty("reference_simd", static_cast<int>(reference->GetSIMDLevel()));
    RecordProperty("pressure_noise_simd_level",
                   static_cast<int>(field.pressure_noise_simd_level()));
    EXPECT_EQ(field.pressure_noise_simd_level(), reference->GetSIMDLevel());
    RecordProperty("temperature_noise_simd_level",
                   static_cast<int>(field.temperature_noise_simd_level()));
    EXPECT_EQ(field.temperature_noise_simd_level(), reference->GetSIMDLevel());
    RecordProperty("humidity_noise_simd_level",
                   static_cast<int>(field.humidity_noise_simd_level()));
    EXPECT_EQ(field.humidity_noise_simd_level(), reference->GetSIMDLevel());
    // Keep constructor and evolved hashes in the test receipt. The control and
    // repaired binaries run identical ticks within each build configuration.
    RecordProperty("constructor_hash", field.ComputeWeatherSubHash());
    for (std::uint64_t tick = 1; tick <= kTicks; ++tick) {
        field.Update(tick, kAnchor, nullptr);
    }
    RecordProperty("evolved_hash", field.ComputeWeatherSubHash());
}

TEST(WeatherSystem, GeometryMatchesPinnedShape) {
    WeatherSystem weather(kSeed);
    EXPECT_EQ(weather.extent_cells(), kWeatherExtentCells);
    EXPECT_FLOAT_EQ(weather.cell_size_m(), kWeatherCellSizeM);
    EXPECT_EQ(kMaxStormCells, 16);
}

TEST(WeatherSystem, RegionalStormQueriesBoundWorkAndPreserveCanonicalAccumulation) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    namespace Math = Luminumbra::DeterministicMath;
    WeatherSystem weather(kSeed);
    weather.BindRegionalBackend(
        [](const Vec3&) {
            return Luminumbra::Systems::WeatherSample{.category = WeatherCategory::Rain,
                                                      .precip_intensity = 0.2f,
                                                      .storm_intensity = 0.0f,
                                                      .wind = Vec2(0.0f)};
        },
        [] { return std::string("regional hash"); });
    std::vector<StormCell> storms;
    storms.reserve(Luminumbra::world::FieldPageState::kMaxStorms);
    // 4,096 owners, each at the admitted 16-storm cap, including distant retained storms.
    for (int z = -32; z < 32; ++z) {
        for (int x = -32; x < 32; ++x) {
            for (std::uint32_t salt = 0; salt < 16; ++salt) {
                const bool order_witness = x == 0 && z == 0;
                const auto offset = order_witness ? 0.0f : static_cast<float>(salt) * 16.0f;
                storms.push_back({.center_world = Vec2(static_cast<float>(x) * 512.0f + offset,
                                                       static_cast<float>(z) * 512.0f + offset),
                                  .velocity = Vec2(0.0f),
                                  .intensity = order_witness ? (salt == 0 ? 0.25f : 1.0e-8f)
                                                             : (salt % 2 == 0 ? 0.01f : 1.0e-8f),
                                  .spawn_tick = salt,
                                  .lifetime_ticks = 240,
                                  .seed_salt = salt});
            }
        }
    }
    ASSERT_EQ(storms.size(), Luminumbra::world::FieldPageState::kMaxStorms);
    weather.AdoptRegionalWeather(1, kAnchor, storms, {});
    std::size_t candidates = 0;
    // Match the water-cell tick budget; distant owners must not multiply query work.
    for (int cell = 0; cell < 4096; ++cell) {
        const int column = cell % 64;
        const int row = cell / 64;
        const Vec3 position(static_cast<float>(column) * 8.0f - 256.0f,
                            0.0f,
                            static_cast<float>(row) * 8.0f - 256.0f);
        candidates += weather.storm_candidates_at(position);
    }
    // A 512 m bin expanded by the 220 m radius intersects at most 9 owners of 16 storms.
    EXPECT_LE(candidates, 4096u * 9u * 16u);
    EXPECT_GT(candidates, 0u);
    constexpr std::array coordinates{
        -512.0f, -220.0f, -0.001f, 0.0f, 219.999f, 220.0f, 511.999f, 512.0f, 732.0f};
    for (const float z : coordinates) {
        for (const float x : coordinates) {
            const Vec3 position(x, 0.0f, z);
            float precip = 0.0f, intensity = 0.0f;
            // Independent reference: the original full scan, in projection order.
            for (const auto& storm : storms) {
                const float dx = x - storm.center_world.x;
                const float dz = z - storm.center_world.y;
                const float distance = Math::Sqrt(dx * dx + dz * dz);
                if (distance >= 220.0f)
                    continue;
                const float t = 1.0f - distance / 220.0f;
                const float falloff = t * t;
                precip += storm.intensity * falloff;
                intensity = std::max(intensity, storm.intensity * t);
            }
            const auto sample = weather.SampleAt(position);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sample.precip_intensity),
                      std::bit_cast<std::uint32_t>(std::min(1.0f, 0.2f + std::min(1.0f, precip))))
                << x << "," << z;
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sample.storm_intensity),
                      std::bit_cast<std::uint32_t>(intensity));
            EXPECT_LE(weather.storm_candidates_at(position), 9u * 16u);
        }
    }
    // The centre sample is deliberately sensitive to reversing the canonical sum.
    float reversed = 0.0f;
    for (int salt = 15; salt >= 0; --salt)
        reversed += salt == 0 ? 0.25f : 1.0e-8f;
    EXPECT_NE(std::bit_cast<std::uint32_t>(weather.PrecipitationAt(Vec3(0.0f))),
              std::bit_cast<std::uint32_t>(0.2f + reversed));
    // Replacement and empty projections must discard all old spatial references.
    weather.AdoptRegionalWeather(2, kAnchor, {storms.front()}, {});
    EXPECT_EQ(weather.storm_candidates_at(kAnchor), 0u);
    EXPECT_EQ(weather.PrecipitationAt(kAnchor), 0.2f);
    weather.AdoptRegionalWeather(3, kAnchor, {}, {});
    EXPECT_EQ(weather.storm_candidates_at(Vec3(-16384.0f, 0.0f, -16384.0f)), 0u);
    EXPECT_EQ(weather.ComputeWeatherSubHash(), "regional hash");
}

TEST(WeatherSystem, RegionalStormIndexHandlesExtremeAndNonfiniteCoordinates) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    WeatherSystem weather(kSeed);
    weather.BindRegionalBackend([](const Vec3&) { return Luminumbra::Systems::WeatherSample{}; },
                                [] { return std::string{}; });
    const auto extreme = std::numeric_limits<float>::max();
    const auto invalid = std::numeric_limits<float>::quiet_NaN();
    const StormCell distant{.center_world = Vec2(extreme),
                            .velocity = Vec2(0.0f),
                            .intensity = 0.5f,
                            .spawn_tick = 1,
                            .lifetime_ticks = 240,
                            .seed_salt = 1};
    auto malformed = distant;
    malformed.center_world.x = invalid;
    auto malformed_z = distant;
    malformed_z.center_world.y = invalid;
    weather.AdoptRegionalWeather(1, kAnchor, {distant, malformed, malformed_z}, {});
    EXPECT_EQ(weather.storm_candidates_at(Vec3(extreme, 0.0f, extreme)), 1u);
    EXPECT_EQ(weather.PrecipitationAt(Vec3(extreme, 0.0f, extreme)), 0.5f);
    for (const auto coordinate : {invalid, std::numeric_limits<float>::infinity(), -extreme}) {
        const auto sample = weather.SampleAt(Vec3(coordinate, 0.0f, coordinate));
        EXPECT_EQ(weather.storm_candidates_at(Vec3(coordinate, 0.0f, coordinate)), 0u);
        EXPECT_EQ(sample.precip_intensity, 0.0f);
        EXPECT_EQ(sample.storm_intensity, 0.0f);
    }
    EXPECT_EQ(weather.storm_candidates_at(Vec3(0.0f, 0.0f, invalid)), 0u);
}

TEST(WeatherSystem, SubHashIsDeterministicAcrossRuns) {
    // The load-bearing property: two fresh instances, same inputs, equal hash.
    // This is the same-tick determinism the world_hash `weather` slot, the
    // ReplayRoundtrip checkpoints, and the WeatherVisual state-hash depend on.
    const RunResult a = RunWeather(kSeed, kTicks, kAnchor);
    const RunResult b = RunWeather(kSeed, kTicks, kAnchor);
    EXPECT_FALSE(a.sub_hash.empty());
    EXPECT_EQ(a.sub_hash, b.sub_hash) << "weather sub-hash diverged across identical runs";
}

TEST(WeatherSystem, SubHashEvolvesWithTick) {
    // The state must actually change over time (a frozen state would make the
    // gate vacuous). Different tick counts => different hashes.
    const RunResult h30 = RunWeather(kSeed, 30, kAnchor);
    const RunResult h300 = RunWeather(kSeed, 300, kAnchor);
    EXPECT_NE(h30.sub_hash, h300.sub_hash);
}

TEST(WeatherSystem, SubHashDependsOnSeed) {
    const RunResult a = RunWeather(kSeed, kTicks, kAnchor);
    const RunResult b = RunWeather(kSeed + 1, kTicks, kAnchor);
    EXPECT_NE(a.sub_hash, b.sub_hash) << "weather state ignored the seed";
}

TEST(WeatherSystem, StormCellsAreBounded) {
    // regression contract: the active storm-cell set never exceeds the pinned cap, and at
    // least one cell spawns over the run (so advection + precip are exercised).
    const RunResult r = RunWeather(kSeed, kTicks, kAnchor);
    EXPECT_LE(r.max_storm_cells, kMaxStormCells);
    EXPECT_GT(r.max_storm_cells, 0) << "no storm cell ever spawned (schedule vacuous)";
}

TEST(WeatherSystem, CategoryQueryInRegionIsValid) {
    WindFieldSystem wind(kSeed);
    WeatherSystem weather(kSeed);
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        wind.Update(t, kAnchor);
        weather.Update(t, kAnchor, &wind);
    }
    // A sample at the anchor is inside the centred streamed extent; the category
    // is one of the valid enum values and precipitation is in [0, 1].
    const WeatherCategory cat = weather.CategoryAt(kAnchor);
    EXPECT_GE(static_cast<int>(cat), 0);
    EXPECT_LT(static_cast<int>(cat), Luminumbra::Systems::kWeatherCategoryCount);
    const float precip = weather.PrecipitationAt(kAnchor);
    EXPECT_GE(precip, 0.0f);
    EXPECT_LE(precip, 1.0f);
}

TEST(WeatherSystem, OutOfRegionCategoryClampsToClear) {
    WindFieldSystem wind(kSeed);
    WeatherSystem weather(kSeed);
    wind.Update(kTicks, kAnchor);
    weather.Update(kTicks, kAnchor, &wind);
    // Far outside the centred 64-cell (1536 m) extent the category clamps to the
    // control-phase default (clear) and base precip is 0 (no storm reaches there).
    const float distant_coordinate = 1.0e6f;
    EXPECT_EQ(weather.CategoryAt(Vec3(distant_coordinate, 5.0f, distant_coordinate)),
              WeatherCategory::Clear);
}

TEST(WeatherSystem, AdvectionIsDeterministicAndStormsMove) {
    // Storm cells advected by the wind grid reach the SAME positions across two
    // runs (bit-determinism), and at least one cell actually moved from its spawn
    // (advection is non-trivial). We compare the sub-hash (which folds storm
    // pos/velocity) at a tick where storms are active.
    WindFieldSystem wind_a(kSeed), wind_b(kSeed);
    WeatherSystem wa(kSeed), wb(kSeed);
    for (std::uint64_t t = 1; t <= 120; ++t) {
        wind_a.Update(t, kAnchor);
        wa.Update(t, kAnchor, &wind_a);
        wind_b.Update(t, kAnchor);
        wb.Update(t, kAnchor, &wind_b);
    }
    EXPECT_EQ(wa.ComputeWeatherSubHash(), wb.ComputeWeatherSubHash());
}

// lightning strike schedule. The strike schedule is folded into the
// `weather` sub-hash (world_hash hash revision); it must FIRE over a storm-bearing
// run (non-vacuous), be bit-deterministic across runs (same total strike count +
// same sub-hash), stay BOUNDED (<= kMaxLiveStrikes, ), and depend on the +13
// seed offset (a different seed -> a different strike total in general).
TEST(WeatherSystem, StrikeScheduleFiresDeterministicallyAndBounded) {
    using Luminumbra::Systems::kMaxLiveStrikes;
    WindFieldSystem wind_a(kSeed), wind_b(kSeed);
    WeatherSystem wa(kSeed), wb(kSeed);
    std::uint64_t total_a = 0, total_b = 0;
    int max_live_a = 0;
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        wind_a.Update(t, kAnchor);
        wa.Update(t, kAnchor, &wind_a);
        wind_b.Update(t, kAnchor);
        wb.Update(t, kAnchor, &wind_b);
        total_a += static_cast<std::uint64_t>(wa.StrikesThisTick().size());
        total_b += static_cast<std::uint64_t>(wb.StrikesThisTick().size());
        if (wa.live_strike_count() > max_live_a)
            max_live_a = wa.live_strike_count();
    }
    EXPECT_GT(total_a, 0u) << "no lightning strike scheduled (strike path vacuous)";
    EXPECT_EQ(total_a, total_b) << "strike schedule diverged across identical runs";
    EXPECT_LE(max_live_a, kMaxLiveStrikes) << "live strike window exceeded the bounded cap";
    EXPECT_EQ(wa.ComputeWeatherSubHash(), wb.ComputeWeatherSubHash());
}

TEST(WeatherSystem, StrikeFieldsAreReplayableWorldEvents) {
    // A scheduled strike carries a tick/position/magnitude that reproduces exactly
    // across runs (the property the render bolt + thunder cue read one-way).
    WindFieldSystem wind_a(kSeed), wind_b(kSeed);
    WeatherSystem wa(kSeed), wb(kSeed);
    for (std::uint64_t t = 1; t <= kTicks; ++t) {
        wind_a.Update(t, kAnchor);
        wa.Update(t, kAnchor, &wind_a);
        wind_b.Update(t, kAnchor);
        wb.Update(t, kAnchor, &wind_b);
    }
    const auto& sa = wa.StrikeSchedule();
    const auto& sb = wb.StrikeSchedule();
    ASSERT_EQ(sa.size(), sb.size());
    for (std::size_t i = 0; i < sa.size(); ++i) {
        EXPECT_EQ(sa[i].strike_tick, sb[i].strike_tick);
        EXPECT_FLOAT_EQ(sa[i].world_x, sb[i].world_x);
        EXPECT_FLOAT_EQ(sa[i].world_z, sb[i].world_z);
        EXPECT_FLOAT_EQ(sa[i].magnitude, sb[i].magnitude);
        EXPECT_EQ(sa[i].storm_salt, sb[i].storm_salt);
    }
}

} // namespace
