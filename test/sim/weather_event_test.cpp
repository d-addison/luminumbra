// sim.weather_events: the deterministic world-level WEATHER-EVENT scheduler
// that sits ON TOP of WeatherSystem (no edit to it). These tests pin the contract:
// the schedule is a pure function of (tick, seed); events tile fixed-length windows
// and turn over (not stuck on one); intensity stays in [0, 1]; transitions are
// BOUNDED (the forbidden Drought<->Snow back-to-back jumps never occur); run==replay
// over many ticks; and different seeds diverge. No entt, no rng-from-clock.
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "systems/WeatherEventSystem.h"

namespace {

using luminumbra::sim::WeatherEvent;
using luminumbra::sim::WeatherEventState;
using luminumbra::sim::WeatherEventAt;
using luminumbra::sim::WeatherEventDriver;
using luminumbra::sim::WeatherEventName;
using luminumbra::sim::kWindowTicks;
using luminumbra::sim::kWeatherEventCount;

// ---- determinism: same (tick, seed) -> same state ----

TEST(WeatherEvents, SameTickSeedSameState) {
    for (std::uint64_t seed : {0ull, 1ull, 7ull, 123456ull}) {
        for (std::uint64_t tick : {0ull, 1ull, 901ull, 5000ull, 1000000ull}) {
            const WeatherEventState a = WeatherEventAt(tick, seed);
            const WeatherEventState b = WeatherEventAt(tick, seed);
            EXPECT_EQ(a.event, b.event);
            EXPECT_FLOAT_EQ(a.intensity, b.intensity);
            EXPECT_EQ(a.window_index, b.window_index);
        }
    }
}

// ---- window tiling: window_index = tick / kWindowTicks; constant within a window ----

TEST(WeatherEvents, WindowIndexTilesTicks) {
    EXPECT_EQ(WeatherEventAt(0ull).window_index, 0ull);
    EXPECT_EQ(WeatherEventAt(kWindowTicks - 1ull).window_index, 0ull);
    EXPECT_EQ(WeatherEventAt(kWindowTicks).window_index, 1ull);
    EXPECT_EQ(WeatherEventAt(2ull * kWindowTicks + 5ull).window_index, 2ull);

    // The event + intensity are window-keyed: every tick inside a window matches.
    const std::uint64_t seed = 42ull;
    const WeatherEventState first = WeatherEventAt(3ull * kWindowTicks, seed);
    for (std::uint64_t off = 0; off < kWindowTicks; off += 137ull) {
        const WeatherEventState s = WeatherEventAt(3ull * kWindowTicks + off, seed);
        EXPECT_EQ(s.event, first.event);
        EXPECT_FLOAT_EQ(s.intensity, first.intensity);
        EXPECT_EQ(s.window_index, 3ull);
    }
}

// ---- window 0 is always Clear (calm premise) ----

TEST(WeatherEvents, FirstWindowIsClear) {
    for (std::uint64_t seed : {0ull, 1ull, 99ull, 1000ull}) {
        EXPECT_EQ(WeatherEventAt(0ull, seed).event, WeatherEvent::Clear);
        EXPECT_EQ(WeatherEventAt(kWindowTicks - 1ull, seed).event, WeatherEvent::Clear);
    }
}

// ---- events change over windows (not stuck on one) ----

TEST(WeatherEvents, EventsChangeOverWindows) {
    const std::uint64_t seed = 12345ull;
    std::set<WeatherEvent> seen;
    for (std::uint64_t w = 0; w < 400ull; ++w) {
        seen.insert(WeatherEventAt(w * kWindowTicks, seed).event);
    }
    // Over hundreds of windows the chain must visit more than a single event.
    EXPECT_GT(seen.size(), static_cast<std::size_t>(1));
}

// ---- intensity always in [0, 1] ----

TEST(WeatherEvents, IntensityInUnitRange) {
    for (std::uint64_t seed : {0ull, 3ull, 77ull, 250000ull}) {
        for (std::uint64_t w = 0; w < 1000ull; ++w) {
            const WeatherEventState s = WeatherEventAt(w * kWindowTicks, seed);
            EXPECT_GE(s.intensity, 0.0f);
            EXPECT_LE(s.intensity, 1.0f);
            // Event enum stays in range.
            EXPECT_GE(static_cast<int>(s.event), 0);
            EXPECT_LT(static_cast<int>(s.event), kWeatherEventCount);
        }
    }
}

// ---- transitions are BOUNDED: Drought<->Snow never occur back-to-back ----

TEST(WeatherEvents, ForbiddenTransitionsNeverOccur) {
    for (std::uint64_t seed : {0ull, 5ull, 88ull, 9001ull, 777777ull}) {
        WeatherEvent prev = WeatherEventAt(0ull, seed).event;
        for (std::uint64_t w = 1; w < 2000ull; ++w) {
            const WeatherEvent cur = WeatherEventAt(w * kWindowTicks, seed).event;
            const bool droughtToSnow = (prev == WeatherEvent::Drought && cur == WeatherEvent::Snow);
            const bool snowToDrought = (prev == WeatherEvent::Snow && cur == WeatherEvent::Drought);
            EXPECT_FALSE(droughtToSnow) << "Drought->Snow at window " << w << " seed " << seed;
            EXPECT_FALSE(snowToDrought) << "Snow->Drought at window " << w << " seed " << seed;
            prev = cur;
        }
    }
}

// ---- run==replay over many ticks (full state stream reproduces) ----

TEST(WeatherEvents, RunEqualsReplayOverManyTicks) {
    const std::uint64_t seed = 555ull;
    std::vector<WeatherEventState> run1;
    std::vector<WeatherEventState> run2;
    run1.reserve(6000);
    run2.reserve(6000);
    for (std::uint64_t t = 0; t < 6000ull; ++t) run1.push_back(WeatherEventAt(t, seed));
    for (std::uint64_t t = 0; t < 6000ull; ++t) run2.push_back(WeatherEventAt(t, seed));
    ASSERT_EQ(run1.size(), run2.size());
    for (std::size_t i = 0; i < run1.size(); ++i) {
        EXPECT_EQ(run1[i].event, run2[i].event) << "tick " << i;
        EXPECT_FLOAT_EQ(run1[i].intensity, run2[i].intensity) << "tick " << i;
        EXPECT_EQ(run1[i].window_index, run2[i].window_index) << "tick " << i;
    }
}

// ---- different seeds diverge ----

TEST(WeatherEvents, DifferentSeedsDiverge) {
    // Compare the event stream over many windows for two seeds: they must differ
    // somewhere (a single shared chain would be a determinism/seed bug).
    bool diverged = false;
    for (std::uint64_t w = 0; w < 500ull && !diverged; ++w) {
        const WeatherEvent a = WeatherEventAt(w * kWindowTicks, 1ull).event;
        const WeatherEvent b = WeatherEventAt(w * kWindowTicks, 2ull).event;
        if (a != b) diverged = true;
    }
    EXPECT_TRUE(diverged);
}

// ---- the optional driver matches the pure function exactly (no-op-vs-pure parity) ----

TEST(WeatherEvents, DriverMatchesPureFunction) {
    const std::uint64_t seed = 31337ull;
    WeatherEventDriver driver(seed);
    for (std::uint64_t t = 0; t < 4000ull; ++t) {
        driver.AdvanceTo(t);
        const WeatherEventState pure = WeatherEventAt(t, seed);
        const WeatherEventState& cur = driver.current();
        EXPECT_EQ(cur.event, pure.event) << "tick " << t;
        EXPECT_FLOAT_EQ(cur.intensity, pure.intensity) << "tick " << t;
        EXPECT_EQ(cur.window_index, pure.window_index) << "tick " << t;
    }
}

// Step (one tick at a time) reproduces the same stream as the pure function.
TEST(WeatherEvents, DriverStepReproducesStream) {
    const std::uint64_t seed = 246ull;
    WeatherEventDriver driver(seed);
    for (std::uint64_t t = 0; t < 3000ull; ++t) {
        const WeatherEventState pure = WeatherEventAt(t, seed);
        EXPECT_EQ(driver.current().event, pure.event) << "tick " << t;
        EXPECT_FLOAT_EQ(driver.current().intensity, pure.intensity) << "tick " << t;
        driver.Step();
    }
}

// ---- name mapping is total + sane (diagnostics surface) ----

TEST(WeatherEvents, EventNamesAreStable) {
    EXPECT_STREQ(WeatherEventName(WeatherEvent::Clear), "clear");
    EXPECT_STREQ(WeatherEventName(WeatherEvent::Storm), "storm");
    EXPECT_STREQ(WeatherEventName(WeatherEvent::Drought), "drought");
    EXPECT_STREQ(WeatherEventName(WeatherEvent::Snow), "snow");
}

} // namespace
