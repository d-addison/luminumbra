#pragma once

#include <cstdint>

namespace luminumbra::core {

// Fixed-rate simulation clock. Accumulates variable frame dt and
// converts it into fixed simulation ticks. The canonical shipped rate is
// 30 Hz (the deterministic runtime contract ); the rate parameter exists for tests only.
//
// Catch-up policy: at most max_catch_up_ticks ticks are produced per
// advance call. When a frame carries more accumulated time than the clamp
// allows, the excess whole-tick time is dropped (never replayed) and recorded
// in the dropped-time telemetry counters; the sub-tick fractional remainder
// is always preserved so steady-state cadence stays exact.
class SimulationClock {
public:
    static constexpr double kCanonicalTickRateHz = 30.0;
    static constexpr std::uint32_t kMaxCatchUpTicksPerFrame = 4;

    explicit SimulationClock(double tick_rate_hz = kCanonicalTickRateHz,
                             std::uint32_t max_catch_up_ticks = kMaxCatchUpTicksPerFrame);

    // Advances the accumulator by frame_dt seconds and returns how many fixed
    // ticks the caller must execute this frame (0..max_catch_up_ticks).
    // tick_count grows by the returned value before this call returns, so
    // the ids of the returned ticks are
    // (tick_count - returned + 1).. tick_count.
    std::uint32_t advance(double frame_dt);

    void reset();

    [[nodiscard]] double fixed_dt() const noexcept {
        return fixed_dt_;
    }
    [[nodiscard]] double tick_rate_hz() const noexcept {
        return tick_rate_hz_;
    }
    [[nodiscard]] std::uint32_t max_catch_up_ticks() const noexcept {
        return max_catch_up_ticks_;
    }

    // Total fixed ticks produced since construction/reset. Tick ids are
    // 1-based: the first executed tick is tick 1.
    [[nodiscard]] std::uint64_t tick_count() const noexcept {
        return tick_count_;
    }

    // Sub-tick time currently carried toward the next tick (always < fixed_dt).
    [[nodiscard]] double accumulator() const noexcept {
        return accumulator_;
    }

    // Telemetry: total simulated time discarded by the catch-up clamp and how
    // many advance calls hit the clamp.
    [[nodiscard]] double dropped_time_seconds() const noexcept {
        return dropped_time_seconds_;
    }
    [[nodiscard]] std::uint64_t dropped_frame_count() const noexcept {
        return dropped_frame_count_;
    }

private:
    double tick_rate_hz_;
    double fixed_dt_;
    std::uint32_t max_catch_up_ticks_;
    double accumulator_ = 0.0;
    std::uint64_t tick_count_ = 0;
    double dropped_time_seconds_ = 0.0;
    std::uint64_t dropped_frame_count_ = 0;
};

} // namespace luminumbra::core
