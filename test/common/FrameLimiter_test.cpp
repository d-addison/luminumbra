#include "luminumbra_client/core/FrameLimiter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Models policy deterministically; native SteadyFrameTiming timer and sleep
// behavior remains unqualified until measured on Windows.
struct VirtualTiming {
    Clock::time_point time{};
    Clock::time_point sleep_deadline{};
    Clock::duration oversleep{};
    Clock::duration margin = 200us;
    Clock::duration sleep_quantum{};
    Clock::duration spin_step = 10us;
    int sleeps = 0;
    int spins = 0;
    bool fail_next_sleep = false;
    int failed_sleeps = 0;

    [[nodiscard]] Clock::time_point now() const {
        return time;
    }

    [[nodiscard]] Clock::duration sleep_margin() const {
        return margin;
    }

    [[nodiscard]] Clock::duration coarse_sleep_quantum() const {
        return fail_next_sleep ? Clock::duration::zero() : sleep_quantum;
    }

    void sleep_until(Clock::time_point deadline) {
        if (fail_next_sleep) {
            fail_next_sleep = false;
            ++failed_sleeps;
            margin = Luminumbra::Client::CoarseFrameSleepMargin(
                std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_quantum));
            return;
        }
        ++sleeps;
        sleep_deadline = deadline;
        auto wait = deadline - time;
        if (sleep_quantum > Clock::duration::zero()) {
            // Round the requested relative sleep up to whole timer ticks,
            // matching a millisecond sleep API on a coarse-resolution host.
            const auto ticks = wait / sleep_quantum + (wait % sleep_quantum != 0ns ? 1 : 0);
            wait = ticks * sleep_quantum;
        }
        time += wait + oversleep;
    }

    void spin() {
        ++spins;
        time += spin_step;
    }
};

TEST(FrameLimiter, SupportedCapsSleepThenSpinToTheDeadline) {
    for (const int cap : {30, 60, 120, 144, 165, 240}) {
        SCOPED_TRACE(cap);
        VirtualTiming timing;
        Luminumbra::Client::FrameLimiter limiter;
        limiter.pace(cap, timing);
        EXPECT_EQ(timing.sleeps, 0);
        EXPECT_EQ(timing.spins, 0);
        const auto period = std::chrono::ceil<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(cap)));
        for (int frame = 0; frame < 5; ++frame) {
            const auto start = timing.time;
            timing.time += 1ms; // Work is included in the period, not added to it.
            limiter.pace(cap, timing);
            EXPECT_EQ(timing.sleep_deadline, start + period - 200us);
            EXPECT_GE(timing.time - start, period);
            EXPECT_LT(timing.time - start, period + 10us);
        }
        EXPECT_EQ(timing.sleeps, 5);
        EXPECT_GT(timing.spins, 0);
    }
}

TEST(FrameLimiter, QuantizedSleepsMaintainTheSelectedRate) {
    for (const auto quantum : {1000us, 2000us, 15625us}) {
        for (const int cap : {240, 165, 144, 120, 60, 30}) {
            SCOPED_TRACE(quantum.count());
            SCOPED_TRACE(cap);
            VirtualTiming timing;
            timing.sleep_quantum = quantum;
            timing.margin = Luminumbra::Client::CoarseFrameSleepMargin(quantum);
            timing.spin_step = 1us;
            Luminumbra::Client::FrameLimiter limiter;
            limiter.pace(cap, timing);
            constexpr int frames = 120;
            const auto start = timing.time;
            for (int frame = 0; frame < frames; ++frame) {
                // Vary the phase relative to timer ticks; fixed work can hide
                // repeated upward rounding at a particular cap.
                timing.time += 100us * (1 + frame % 7);
                limiter.pace(cap, timing);
            }
            const double elapsed = std::chrono::duration<double>(timing.time - start).count();
            const double achieved = static_cast<double>(frames) / elapsed;
            EXPECT_LE(achieved, static_cast<double>(cap));
            EXPECT_NEAR(achieved, static_cast<double>(cap), static_cast<double>(cap) * 0.0005);
            EXPECT_GT(timing.spins, 0);
            if (quantum <= 2ms || cap <= 60)
                EXPECT_EQ(timing.sleeps, frames);
            else
                EXPECT_EQ(timing.sleeps, 0);
        }
    }
}

TEST(FrameLimiter, CoarseFallbackAtSixtyFpsBoundsSpinShare) {
    for (const bool timer_fails : {false, true}) {
        SCOPED_TRACE(timer_fails);
        VirtualTiming timing;
        timing.sleep_quantum = 15625us;
        timing.margin = timer_fails ? 200us : Luminumbra::Client::CoarseFrameSleepMargin(15625us);
        timing.spin_step = 1us;
        timing.fail_next_sleep = timer_fails;
        Luminumbra::Client::FrameLimiter limiter;
        limiter.pace(60, timing);
        constexpr int frames = 120;
        const auto start = timing.time;
        const auto period = std::chrono::ceil<Clock::duration>(1s / 60.0);
        for (int frame = 0; frame < frames; ++frame) {
            const auto frame_start = timing.time;
            const int spins_before = timing.spins;
            timing.time += 100us * (1 + frame % 7);
            limiter.pace(60, timing);
            const auto spin_time = (timing.spins - spins_before) * timing.spin_step;
            // One 15.625 ms sleep leaves at most 1.042 ms of a 60 FPS frame.
            EXPECT_LT(spin_time, period / 10);
            EXPECT_GE(timing.time - frame_start, period);
            EXPECT_LT(timing.time - frame_start, period + timing.spin_step);
        }
        EXPECT_EQ(timing.failed_sleeps, timer_fails ? 1 : 0);
        EXPECT_EQ(timing.sleeps, frames);
        EXPECT_GT(timing.spins, 0);
        EXPECT_LT(timing.spins * timing.spin_step, (timing.time - start) / 10);
    }
}

TEST(FrameLimiter, CoarseSleepMarginCoversAnEntireTimerTick) {
    using Luminumbra::Client::CoarseFrameSleepMargin;
    EXPECT_GE(CoarseFrameSleepMargin(1ms), 2ms);
    EXPECT_GE(CoarseFrameSleepMargin(2ms), 3ms);
    EXPECT_GE(CoarseFrameSleepMargin(15625us), 16625us);
}

TEST(FrameLimiter, ShortRemainingWaitSpinsWithoutSleeping) {
    VirtualTiming timing;
    Luminumbra::Client::FrameLimiter limiter;
    limiter.pace(240, timing);
    timing.time += 4100us;
    limiter.pace(240, timing);
    EXPECT_EQ(timing.sleeps, 0);
    EXPECT_GT(timing.spins, 0);
    EXPECT_GE(timing.time.time_since_epoch(), 4166667ns);
    EXPECT_LT(timing.time.time_since_epoch(), 4176667ns);
}

TEST(FrameLimiter, SlowFramesAndOversleepsRebaseWithoutCatchUp) {
    VirtualTiming timing;
    Luminumbra::Client::FrameLimiter limiter;
    limiter.pace(120, timing);
    timing.time += 1s;
    limiter.pace(120, timing);
    EXPECT_EQ(timing.sleeps, 0);
    EXPECT_EQ(timing.spins, 0);
    timing.oversleep = 5ms;
    limiter.pace(120, timing);
    EXPECT_EQ(timing.sleeps, 1);
    EXPECT_EQ(timing.spins, 0);
    const auto late_start = timing.time;
    timing.oversleep = Clock::duration::zero();
    limiter.pace(120, timing);
    EXPECT_EQ(timing.sleeps, 2);
    EXPECT_GE(timing.time - late_start, 8333334ns);
    EXPECT_LT(timing.time - late_start, 8343334ns);
}

TEST(FrameLimiter, CapChangesUnlimitedAndInvalidValuesResetPacing) {
    VirtualTiming timing;
    Luminumbra::Client::FrameLimiter limiter;
    for (const int cap : {60, 144, 0, 144, -1, 144, 61, 240}) {
        limiter.pace(cap, timing);
        EXPECT_EQ(timing.sleeps, 0);
        EXPECT_EQ(timing.spins, 0);
    }
    limiter.pace(240, timing);
    EXPECT_EQ(timing.sleeps, 1);
    EXPECT_GT(timing.spins, 0);
}

TEST(FramePacing, PlayerSettingsAndInvalidCapFallback) {
    using luminumbra::core::ResolveFramePacing;
    for (const int cap : luminumbra::core::kFrameCaps) {
        for (const bool vsync : {false, true}) {
            const auto pacing = ResolveFramePacing(vsync, cap);
            EXPECT_EQ(pacing.vsync, vsync);
            EXPECT_EQ(pacing.frame_cap, cap);
        }
    }
    const auto pacing = ResolveFramePacing(true, 61);
    EXPECT_TRUE(pacing.vsync);
    EXPECT_EQ(pacing.frame_cap, 0);
}

TEST(FramePacing, SupportedCapsMatchTheContractAndRejectNeighboringValues) {
    constexpr int expected[] = {0, 30, 60, 120, 144, 165, 240};
    ASSERT_EQ(std::size(luminumbra::core::kFrameCaps), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        EXPECT_EQ(luminumbra::core::kFrameCaps[i], expected[i]);
        EXPECT_TRUE(luminumbra::core::IsValidFrameCap(expected[i]));
    }
    for (const int cap : {std::numeric_limits<int>::min(),
                          -1,
                          1,
                          29,
                          31,
                          59,
                          61,
                          119,
                          121,
                          143,
                          145,
                          164,
                          166,
                          239,
                          241,
                          std::numeric_limits<int>::max()}) {
        SCOPED_TRACE(cap);
        EXPECT_FALSE(luminumbra::core::IsValidFrameCap(cap));
        EXPECT_EQ(luminumbra::core::ResolveFramePacing(true, cap).frame_cap, 0);
    }
}

TEST(FramePacing, BenchmarkAndPerfCaptureOverridesAlwaysDisableBothLimits) {
    using luminumbra::core::FramePacingOverrides;
    using luminumbra::core::ResolveFramePacing;
    for (const auto overrides :
         {FramePacingOverrides{
              .benchmark_active = true, .scenario_active = false, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = true, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = false, .profiling_active = true}}) {
        for (const int cap : luminumbra::core::kFrameCaps) {
            for (const bool vsync : {false, true}) {
                const auto pacing = ResolveFramePacing(vsync, cap, overrides);
                EXPECT_FALSE(pacing.vsync);
                EXPECT_EQ(pacing.frame_cap, 0);
                VirtualTiming timing;
                Luminumbra::Client::FrameLimiter limiter;
                limiter.pace(cap, timing);
                limiter.pace(pacing.frame_cap, timing);
                limiter.pace(pacing.frame_cap, timing);
                EXPECT_EQ(timing.sleeps, 0);
                EXPECT_EQ(timing.spins, 0);
            }
        }
    }
}

TEST(FramePacing, LeavingMeasurementRestoresThePlayerChoices) {
    const auto overridden = luminumbra::core::ResolveFramePacing(
        true, 144, {.benchmark_active = true, .scenario_active = false, .profiling_active = false});
    EXPECT_FALSE(overridden.vsync);
    EXPECT_EQ(overridden.frame_cap, 0);
    const auto restored = luminumbra::core::ResolveFramePacing(true, 144);
    EXPECT_TRUE(restored.vsync);
    EXPECT_EQ(restored.frame_cap, 144);
}

TEST(FramePacing, LiveSettingsEditsStayOverriddenUntilMeasurementEnds) {
    using luminumbra::core::FramePacingOverrides;
    using luminumbra::core::ResolveFramePacing;
    for (const auto overrides :
         {FramePacingOverrides{
              .benchmark_active = true, .scenario_active = false, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = true, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = false, .profiling_active = true}}) {
        VirtualTiming timing;
        Luminumbra::Client::FrameLimiter limiter;
        for (const int cap : {240, 30, 60, 165, 144, 120, 0}) {
            for (const bool vsync : {true, false}) {
                const auto pacing = ResolveFramePacing(vsync, cap, overrides);
                EXPECT_FALSE(pacing.vsync);
                EXPECT_EQ(pacing.frame_cap, 0);
                limiter.pace(pacing.frame_cap, timing);
                limiter.pace(pacing.frame_cap, timing);
                EXPECT_EQ(timing.sleeps, 0);
                EXPECT_EQ(timing.spins, 0);
                const auto restored = ResolveFramePacing(vsync, cap);
                EXPECT_EQ(restored.vsync, vsync);
                EXPECT_EQ(restored.frame_cap, cap);
            }
        }
    }
}
} // namespace
