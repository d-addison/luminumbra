#include "SimulationClock.h"

#include <algorithm>
#include <cmath>

namespace luminumbra::core {

SimulationClock::SimulationClock(double tick_rate_hz, std::uint32_t max_catch_up_ticks)
    : tick_rate_hz_(tick_rate_hz > 0.0 ? tick_rate_hz : kCanonicalTickRateHz)
    , fixed_dt_(1.0 / (tick_rate_hz > 0.0 ? tick_rate_hz : kCanonicalTickRateHz))
    , max_catch_up_ticks_(max_catch_up_ticks > 0 ? max_catch_up_ticks : 1) {}

std::uint32_t SimulationClock::advance(double frame_dt) {
    if (frame_dt > 0.0 && std::isfinite(frame_dt)) {
        accumulator_ += frame_dt;
    }

    const auto ticks_possible = static_cast<std::uint64_t>(std::floor(accumulator_ / fixed_dt_));
    const auto ticks_executed =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(ticks_possible, max_catch_up_ticks_));

    accumulator_ -= static_cast<double>(ticks_executed) * fixed_dt_;
    if (ticks_possible > ticks_executed) {
        // Clamp hit: drop the whole-tick excess (never replayed) but keep the
        // sub-tick fractional remainder so steady-state cadence stays exact.
        const auto dropped_ticks = ticks_possible - ticks_executed;
        const double dropped_seconds = static_cast<double>(dropped_ticks) * fixed_dt_;
        accumulator_ = std::max(0.0, accumulator_ - dropped_seconds);
        dropped_time_seconds_ += dropped_seconds;
        ++dropped_frame_count_;
    }

    tick_count_ += ticks_executed;
    return ticks_executed;
}

void SimulationClock::reset() {
    accumulator_ = 0.0;
    tick_count_ = 0;
    dropped_time_seconds_ = 0.0;
    dropped_frame_count_ = 0;
}

} // namespace luminumbra::core
