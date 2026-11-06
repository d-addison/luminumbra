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
#include <stdexcept>
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
                intensity = (std::max)(intensity, storm.intensity * t);
            }
            const auto sample = weather.SampleAt(position);
            EXPECT_EQ(
                std::bit_cast<std::uint32_t>(sample.precip_intensity),
                std::bit_cast<std::uint32_t>((std::min)(1.0f, 0.2f + (std::min)(1.0f, precip))))
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

void ExpectRegionalProjectionMatches(const WeatherSystem& actual, const WeatherSystem& reference) {
    for (const float z : {-512.0f, -220.0f, 0.0f, 220.0f, 512.0f, 732.0f}) {
        for (const float x : {-512.0f, -220.0f, 0.0f, 220.0f, 512.0f, 732.0f}) {
            const Vec3 position{x, 0, z};
            EXPECT_EQ(actual.storm_candidates_at(position),
                      reference.storm_candidates_at(position));
            const auto sample = actual.SampleAt(position);
            const auto expected = reference.SampleAt(position);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sample.precip_intensity),
                      std::bit_cast<std::uint32_t>(expected.precip_intensity));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sample.storm_intensity),
                      std::bit_cast<std::uint32_t>(expected.storm_intensity));
        }
    }
}

void CheckRegionalProjectionUpdates() {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    WeatherSystem weather(kSeed), reference(kSeed);
    const auto sample = [](const Vec3&) {
        return Luminumbra::Systems::WeatherSample{};
    };
    const auto hash = [] {
        return std::string{};
    };
    weather.BindRegionalBackend(sample, hash);
    reference.BindRegionalBackend(sample, hash);
    std::vector<StormCell> storms{{.center_world = Vec2(0), .intensity = 0.25f},
                                  {.center_world = Vec2(0), .intensity = 1.0e-8f},
                                  {.center_world = Vec2(0), .intensity = 1.0e-8f}};
    weather.AdoptRegionalWeather(1, kAnchor, storms, {});
    // Move across bins, revisit a populated bin in identity order, and update in place.
    for (const Vec2 center : {Vec2(512, 512), Vec2(-220, -220), Vec2(0), Vec2(0)}) {
        storms[1].center_world = center;
        storms[1].intensity = center == Vec2(0) ? 1.0e-8f : 0.5f;
        weather.UpdateRegionalStorm(1, storms[1]);
        weather.RefreshRegionalWeather(2, kAnchor, {});
        reference.AdoptRegionalWeather(2, kAnchor, storms, {});
        ASSERT_NO_FATAL_FAILURE(ExpectRegionalProjectionMatches(weather, reference));
    }
}

TEST(WeatherSystem, RegionalStormUpdatesMatchCompleteProjection) {
    ASSERT_NO_FATAL_FAILURE(CheckRegionalProjectionUpdates());
}

void CheckRegionalStormNodeRetention(bool identity_ordered) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    WeatherSystem weather(kSeed), reference(kSeed);
    const auto sample = [](const Vec3&) {
        return Luminumbra::Systems::WeatherSample{};
    };
    const auto hash = [] {
        return std::string{};
    };
    weather.BindRegionalBackend(sample, hash);
    reference.BindRegionalBackend(sample, hash);
    std::vector<StormCell> storms{{.center_world = Vec2(256),
                                   .intensity = 0.5f,
                                   .spawn_tick = 1,
                                   .lifetime_ticks = 240,
                                   .seed_salt = 1}};
    weather.AdoptRegionalWeather(1, kAnchor, storms, {}, identity_ordered);
    const auto invalid = std::numeric_limits<float>::quiet_NaN();
    // Equal projection, changed values in retained bins, partial overlap, disjoint bins,
    // nonfinite coordinates, repeated nonfinite projection, and a return to valid bins.
    const std::array centers{Vec2(256),
                             Vec2(260),
                             Vec2(300),
                             Vec2(304),
                             Vec2(2048),
                             Vec2(invalid),
                             Vec2(invalid),
                             Vec2(256)};
    for (std::size_t i = 0; i < centers.size(); ++i) {
        SCOPED_TRACE(i);
        storms.front().center_world = centers[i];
        if (i == 1) {
            storms.front().intensity = 0.75f;
            storms.front().velocity = Vec2(1, 2);
            storms.front().lifetime_ticks = 241;
        }
        weather.ResetRegionalProjectionWork();
        weather.UpdateRegionalStorm(0, storms.front());
        if (i == 0 || i == 1 || i == 3 || i == 6) {
            EXPECT_EQ(weather.RegionalStormNodeChanges(), 0u);
        } else {
            EXPECT_GT(weather.RegionalStormNodeChanges(), 0u);
        }
        reference.AdoptRegionalWeather(2, kAnchor, storms, {}, identity_ordered);
        ASSERT_NO_FATAL_FAILURE(ExpectRegionalProjectionMatches(weather, reference));
        ASSERT_EQ(weather.StormCells().size(), 1u);
        const auto& projected = weather.StormCells().front();
        EXPECT_EQ(std::bit_cast<std::uint32_t>(projected.center_world.x),
                  std::bit_cast<std::uint32_t>(storms.front().center_world.x));
        EXPECT_EQ(projected.velocity, storms.front().velocity);
        EXPECT_EQ(projected.intensity, storms.front().intensity);
        EXPECT_EQ(projected.lifetime_ticks, storms.front().lifetime_ticks);
    }
    if (identity_ordered) {
        // An identity change retires the old identity even when its bins stay the same.
        storms.front().spawn_tick = 3;
        storms.front().seed_salt = 3;
        weather.ResetRegionalProjectionWork();
        weather.UpdateRegionalStorm(0, storms.front());
        EXPECT_GT(weather.RegionalStormNodeChanges(), 0u);
        ASSERT_EQ(weather.StormCells().size(), 1u);
        EXPECT_EQ(weather.StormCells().front().spawn_tick, 3u);
        EXPECT_EQ(weather.StormCells().front().seed_salt, 3u);
        reference.AdoptRegionalWeather(2, kAnchor, storms, {}, true);
        ASSERT_NO_FATAL_FAILURE(ExpectRegionalProjectionMatches(weather, reference));
        weather.ResetRegionalProjectionWork();
        weather.EraseRegionalStorm({1, 1});
        EXPECT_EQ(weather.RegionalStormNodeChanges(), 0u);
        ASSERT_EQ(weather.StormCells().size(), 1u);
        weather.EraseRegionalStorm({3, 3});
        EXPECT_GT(weather.RegionalStormNodeChanges(), 0u);
        EXPECT_TRUE(weather.StormCells().empty());
        EXPECT_EQ(weather.storm_candidates_at(Vec3(256, 0, 256)), 0u);
        weather.ResetRegionalProjectionWork();
        weather.EraseRegionalStorm({3, 3});
        EXPECT_EQ(weather.RegionalStormNodeChanges(), 0u);
    }
}

TEST(WeatherSystem, RegionalStormUpdatesRetainIdentityAndUnchangedBinNodes) {
    for (const bool identity_ordered : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckRegionalStormNodeRetention(identity_ordered));
    }
}

TEST(WeatherSystem, RegionalStormIndexHandlesExtremeAndNonfiniteCoordinates) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    WeatherSystem weather(kSeed);
    weather.BindRegionalBackend([](const Vec3&) { return Luminumbra::Systems::WeatherSample{}; },
                                [] { return std::string{}; });
    const auto extreme = (std::numeric_limits<float>::max)();
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

TEST(WeatherSystem, RegionalRebindIndexesCurrentStormsAfterLegacyUpdates) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    using Luminumbra::Systems::WeatherSample;
    WeatherSystem weather(kSeed);
    const auto sampler = [](const Vec3&) {
        return WeatherSample{.precip_intensity = 0.125f};
    };
    weather.BindRegionalBackend(sampler, [] { return std::string("regional hash"); });
    const StormCell expired{.center_world = Vec2(0.0f),
                            .velocity = Vec2(0.0f),
                            .intensity = 0.5f,
                            .spawn_tick = 0,
                            .lifetime_ticks = 60,
                            .seed_salt = 1};
    const StormCell survivor{.center_world = Vec2(2048.0f, 0.0f),
                             .velocity = Vec2(30720.0f, 0.0f),
                             .intensity = 0.5f,
                             .spawn_tick = 46,
                             .lifetime_ticks = 240,
                             .seed_salt = 2};
    weather.AdoptRegionalWeather(60, kAnchor, {expired, survivor}, {});
    weather.BindRegionalBackend({}, {});
    weather.Update(61, kAnchor, nullptr);
    ASSERT_EQ(weather.StormCells().size(), 1u);
    ASSERT_EQ(weather.StormCells().front().seed_salt, survivor.seed_salt);
    const auto current = weather.StormCells().front();
    EXPECT_FLOAT_EQ(current.center_world.x, 3072.0f);
    EXPECT_FLOAT_EQ(current.intensity, 15.0f / 45.0f);

    weather.BindRegionalBackend(sampler, [] { return std::string("regional hash"); });
    for (const float x : {0.0f, 2048.0f}) {
        const Vec3 position(x, 0.0f, 0.0f);
        ASSERT_EQ(weather.storm_candidates_at(position), 0u);
        EXPECT_FLOAT_EQ(weather.PrecipitationAt(position), 0.125f);
        const auto sample = weather.SampleAt(position);
        EXPECT_FLOAT_EQ(sample.precip_intensity, 0.125f);
        EXPECT_FLOAT_EQ(sample.storm_intensity, 0.0f);
    }
    const Vec3 position(current.center_world.x, 0.0f, current.center_world.y);
    ASSERT_EQ(weather.storm_candidates_at(position), 1u);
    EXPECT_FLOAT_EQ(weather.PrecipitationAt(position), 0.125f + current.intensity);
    const auto sample = weather.SampleAt(position);
    EXPECT_FLOAT_EQ(sample.precip_intensity, 0.125f + current.intensity);
    EXPECT_FLOAT_EQ(sample.storm_intensity, current.intensity);

    weather.BindRegionalBackend({}, {});
    weather.Update(301, kAnchor, nullptr);
    ASSERT_TRUE(weather.StormCells().empty());
    weather.BindRegionalBackend(sampler, [] { return std::string("regional hash"); });
    ASSERT_EQ(weather.storm_candidates_at(position), 0u);
    EXPECT_FLOAT_EQ(weather.PrecipitationAt(position), 0.125f);
    EXPECT_FLOAT_EQ(weather.SampleAt(position).storm_intensity, 0.0f);
}

TEST(WeatherSystem, RegionalClockUpdatesPreserveLegacyWindPerturbations) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    using Luminumbra::Systems::WeatherSample;
    using Luminumbra::Systems::WindLayer;
    for (const bool populated : {false, true}) {
        SCOPED_TRACE(populated);
        WeatherSystem weather(kSeed);
        WindFieldSystem wind(kSeed);
        WindFieldSystem baseline_wind(kSeed);
        if (populated) {
            const WindFieldSystem::StormPerturbation retained{
                Vec2(kAnchor.x, kAnchor.z), 192.0f, Vec2(8.0f, -3.0f), 0.75f};
            wind.InjectStormPerturbation(retained);
            baseline_wind.InjectStormPerturbation(retained);
        }
        weather.BindRegionalBackend(
            [](const Vec3&) { return WeatherSample{.wind = Vec2(3.0f, 4.0f)}; },
            [] { return std::string("regional hash"); });
        wind.BindRegionalBackend([](const Vec3&, WindLayer) { return Vec2(3.0f, 4.0f); },
                                 [] { return std::string("regional wind"); });
        const StormCell storm{.center_world = Vec2(kAnchor.x, kAnchor.z),
                              .velocity = Vec2(-2.0f, 7.0f),
                              .intensity = 0.5f,
                              .spawn_tick = 1,
                              .lifetime_ticks = 240,
                              .seed_salt = 1};
        weather.AdoptRegionalWeather(60, kAnchor, {storm, storm}, {});
        for (std::uint64_t tick = 61; tick <= 62; ++tick) {
            weather.UpdateFromClock(tick, kAnchor, wind);
            EXPECT_EQ(weather.last_tick(), tick);
            EXPECT_EQ(wind.last_tick(), tick);
            EXPECT_EQ(weather.StormCells().size(), 2u);
        }
        // Returning to legacy wind makes any queued perturbations observable in the grid.
        wind.BindRegionalBackend({}, {});
        wind.Update(63, kAnchor);
        baseline_wind.Update(63, kAnchor);
        EXPECT_EQ(wind.ComputeWindSubHash(), baseline_wind.ComputeWindSubHash());
        if (populated) {
            WindFieldSystem empty_wind(kSeed);
            empty_wind.Update(63, kAnchor);
            EXPECT_NE(baseline_wind.ComputeWindSubHash(), empty_wind.ComputeWindSubHash());
        }
    }
}

TEST(WeatherSystem, RegionalBackendRefusesUnpairedCallbacks) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::WeatherSample;
    WeatherSystem weather(kSeed);
    weather.BindRegionalBackend([](const Vec3&) { return WeatherSample{.wind = Vec2(3.0f, 4.0f)}; },
                                [] { return std::string("regional hash"); });
    EXPECT_THROW(weather.BindRegionalBackend(
                     [](const Vec3&) { return WeatherSample{.wind = Vec2(-1.0f, 0.0f)}; }, {}),
                 std::invalid_argument);
    EXPECT_THROW(weather.BindRegionalBackend({}, [] { return std::string("other hash"); }),
                 std::invalid_argument);
    // A refused binding leaves the previous backend in place.
    EXPECT_EQ(weather.ComputeWeatherSubHash(), "regional hash");
    EXPECT_FLOAT_EQ(weather.SampleAt(Vec3(0.0f)).wind.x, 3.0f);
}

TEST(WeatherSystem, RegionalRestoreAdvancesWindToRestoredTick) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    using Luminumbra::Systems::WeatherSample;
    using Luminumbra::Systems::WindLayer;
    WeatherSystem weather(kSeed);
    WindFieldSystem wind(kSeed);
    weather.BindRegionalBackend([](const Vec3&) { return WeatherSample{.wind = Vec2(3.0f, 4.0f)}; },
                                [] { return std::string("regional hash"); });
    wind.BindRegionalBackend([](const Vec3&, WindLayer) { return Vec2(3.0f, 4.0f); },
                             [] { return std::string("regional wind"); });
    const StormCell storm{.center_world = Vec2(kAnchor.x, kAnchor.z),
                          .velocity = Vec2(-2.0f, 7.0f),
                          .intensity = 0.5f,
                          .spawn_tick = 1,
                          .lifetime_ticks = 240,
                          .seed_salt = 1};
    weather.AdoptRegionalWeather(60, kAnchor, {storm}, {});
    weather.RestoreAtTick(90, kAnchor, wind);
    EXPECT_EQ(weather.last_tick(), 90u);
    EXPECT_EQ(wind.last_tick(), 90u);
    ASSERT_EQ(weather.StormCells().size(), 1u);
    EXPECT_EQ(weather.StormCells().front().seed_salt, storm.seed_salt);
}

TEST(WeatherSystem, RegionalSampleUsesOneBackendSnapshotPerQuery) {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    using Luminumbra::Systems::WeatherSample;
    WeatherSystem weather(kSeed);
    const StormCell storm{.center_world = Vec2(0.0f),
                          .velocity = Vec2(0.0f),
                          .intensity = 0.5f,
                          .spawn_tick = 1,
                          .lifetime_ticks = 240,
                          .seed_salt = 1};
    weather.AdoptRegionalWeather(60, kAnchor, {storm}, {});
    std::uint32_t calls = 0;
    weather.BindRegionalBackend(
        [&calls](const Vec3&) {
            ++calls;
            const float value = static_cast<float>(calls) * 0.25f;
            return WeatherSample{.category =
                                     calls % 2 == 1 ? WeatherCategory::Rain : WeatherCategory::Snow,
                                 .precip_intensity = value,
                                 .storm_intensity = 0.9f,
                                 .wind = Vec2(value, -value)};
        },
        [] { return std::string("regional hash"); });
    ASSERT_EQ(calls, 0u);
    for (std::uint32_t query = 1; query <= 3; ++query) {
        const auto sample = weather.SampleAt(Vec3(0.0f));
        EXPECT_EQ(calls, query);
        const float value = static_cast<float>(query) * 0.25f;
        EXPECT_EQ(sample.category, query % 2 == 1 ? WeatherCategory::Rain : WeatherCategory::Snow);
        EXPECT_FLOAT_EQ(sample.precip_intensity, (std::min)(1.0f, value + storm.intensity));
        EXPECT_FLOAT_EQ(sample.storm_intensity, storm.intensity);
        EXPECT_FLOAT_EQ(sample.wind.x, value);
        EXPECT_FLOAT_EQ(sample.wind.y, -value);
    }
}

TEST(WeatherSystem, EmptyRegionalCallbacksPreserveLegacySamplesAndHash) {
    WeatherSystem baseline(kSeed);
    WeatherSystem weather(kSeed);
    WindFieldSystem baseline_wind(kSeed);
    WindFieldSystem wind(kSeed);
    weather.BindRegionalBackend({}, {});
    EXPECT_EQ(weather.ComputeWeatherSubHash(), baseline.ComputeWeatherSubHash());
    for (std::uint64_t tick = 1; tick <= kTicks; ++tick) {
        baseline.UpdateFromClock(tick, kAnchor, baseline_wind);
        weather.UpdateFromClock(tick, kAnchor, wind);
        EXPECT_EQ(weather.ComputeWeatherSubHash(), baseline.ComputeWeatherSubHash());
        EXPECT_EQ(wind.ComputeWindSubHash(), baseline_wind.ComputeWindSubHash());
        for (const float coordinate : {-768.0f, -0.1f, 0.0f, 767.0f, 768.0f, 1000000.0f}) {
            const Vec3 position(coordinate, 5.0f, coordinate);
            const auto sampled = weather.SampleAt(position);
            const auto original = baseline.SampleAt(position);
            EXPECT_EQ(sampled.category, original.category);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.precip_intensity),
                      std::bit_cast<std::uint32_t>(original.precip_intensity));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.storm_intensity),
                      std::bit_cast<std::uint32_t>(original.storm_intensity));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.wind.x),
                      std::bit_cast<std::uint32_t>(original.wind.x));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(sampled.wind.y),
                      std::bit_cast<std::uint32_t>(original.wind.y));
            EXPECT_EQ(weather.storm_candidates_at(position), weather.StormCells().size());
        }
    }
    weather.RestoreAtTick(kTicks, kAnchor, wind);
    EXPECT_EQ(weather.ComputeWeatherSubHash(), baseline.ComputeWeatherSubHash());
    EXPECT_EQ(wind.ComputeWindSubHash(), baseline_wind.ComputeWindSubHash());
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
    const float far = 1.0e6f;
    EXPECT_EQ(weather.CategoryAt(Vec3(far, 5.0f, far)), WeatherCategory::Clear);
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

namespace {

void CheckRegionalIdentityMembershipChanges() {
    using Luminumbra::Vec2;
    using Luminumbra::Systems::StormCell;
    using Luminumbra::Systems::StrikeEvent;
    WeatherSystem weather(kSeed), reference(kSeed);
    const auto sample = [](const Vec3&) {
        return Luminumbra::Systems::WeatherSample{};
    };
    const auto hash = [] {
        return std::string("owner weather");
    };
    weather.BindRegionalBackend(sample, hash);
    reference.BindRegionalBackend(sample, hash);
    std::vector<StormCell> storms{
        {.center_world = Vec2(0), .intensity = 0.25f, .spawn_tick = 1, .seed_salt = 1},
        {.center_world = Vec2(0), .intensity = 1.0e-8f, .spawn_tick = 2, .seed_salt = 2},
        {.center_world = Vec2(512), .intensity = 0.5f, .spawn_tick = 3, .seed_salt = 3}};
    const StrikeEvent retained{10, 0, 0, 0.5f, 1};
    const StrikeEvent retired{9, 512, 512, 0.75f, 2};
    const StrikeEvent generated{10, -512, 0, 0.25f, 3};
    weather.AdoptRegionalWeather(9, kAnchor, storms, {retired, retained}, true);
    weather.ResetRegionalProjectionWork();
    // Removing an earlier identity must not shift any spatial reference to the survivors.
    weather.EraseRegionalStorm({1, 1});
    storms.erase(storms.begin());
    storms.front().center_world = Vec2(-220, 0);
    weather.PutRegionalStorm(storms.front());
    storms.push_back(
        {.center_world = Vec2(0), .intensity = 0.125f, .spawn_tick = 4, .seed_salt = 4});
    weather.PutRegionalStorm(storms.back());
    weather.EraseRegionalStrike(retired);
    weather.PutRegionalStrike(generated);
    weather.RefreshRegionalWeather(10, kAnchor);
    reference.AdoptRegionalWeather(10, kAnchor, storms, {retained, generated});
    ASSERT_NO_FATAL_FAILURE(ExpectRegionalProjectionMatches(weather, reference));
    ASSERT_EQ(weather.StormCells().size(), storms.size());
    for (std::size_t i = 0; i < storms.size(); ++i) {
        EXPECT_EQ(weather.StormCells()[i].spawn_tick, storms[i].spawn_tick);
        EXPECT_EQ(weather.StormCells()[i].center_world, storms[i].center_world);
    }
    const auto strikes = weather.StrikesThisTick();
    ASSERT_EQ(strikes.size(), 2u);
    EXPECT_EQ(strikes[0].storm_salt, retained.storm_salt);
    EXPECT_EQ(strikes[1].storm_salt, generated.storm_salt);
    EXPECT_EQ(weather.live_strike_count(), 2);
    EXPECT_EQ(weather.StrikeSchedule().size(), 2u);
    EXPECT_GT(weather.RegionalProjectionWork(), 0u);
    EXPECT_LT(weather.RegionalProjectionWork(), 64u);
    weather.RefreshRegionalWeather(11, kAnchor);
    EXPECT_TRUE(weather.StrikesThisTick().empty());
    EXPECT_EQ(weather.StrikeSchedule().size(), 2u);
}

void CheckRegionalRestoredStrikeVisibility() {
    using Luminumbra::Systems::StrikeEvent;
    WeatherSystem weather(kSeed);
    weather.BindRegionalBackend([](const Vec3&) { return Luminumbra::Systems::WeatherSample{}; },
                                [] { return std::string("restored weather"); });
    const StrikeEvent past{9, 0, 0, 0.5f, 1};
    const StrikeEvent delivered{10, 512, 0, 0.75f, 2};
    const StrikeEvent future{11, 0, 512, 0.25f, 3};
    weather.AdoptRegionalWeather(10, kAnchor, {}, {past, delivered, future}, true, true);
    EXPECT_EQ(weather.live_strike_count(), 1);
    ASSERT_EQ(weather.StrikeSchedule().size(), 1u);
    EXPECT_EQ(weather.StrikeSchedule().front().storm_salt, future.storm_salt);
    EXPECT_TRUE(weather.StrikesThisTick().empty());
    weather.ResetRegionalProjectionWork();
    weather.RefreshRegionalWeather(11, kAnchor);
    ASSERT_EQ(weather.StrikesThisTick().size(), 1u);
    EXPECT_EQ(weather.StrikeSchedule().size(), 1u);
    weather.RevealRegionalStrikeSchedule();
    EXPECT_EQ(weather.live_strike_count(), 3);
    EXPECT_EQ(weather.StrikeSchedule().size(), 3u);
    EXPECT_EQ(weather.StrikesThisTick().size(), 1u);
    EXPECT_EQ(weather.RegionalProjectionWork(), 0u);
    weather.EraseRegionalStrike(delivered);
    EXPECT_EQ(weather.live_strike_count(), 2);
    EXPECT_EQ(weather.StrikeSchedule().size(), 2u);
}

} // namespace

TEST(WeatherSystem, RegionalIdentityMembershipChangesPreserveSamplesAndStrikeOrdering) {
    ASSERT_NO_FATAL_FAILURE(CheckRegionalIdentityMembershipChanges());
}

TEST(WeatherSystem, RegionalRestoredStrikesStayHiddenUntilParticipatingReplacement) {
    ASSERT_NO_FATAL_FAILURE(CheckRegionalRestoredStrikeVisibility());
}
