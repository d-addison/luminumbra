#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace Luminumbra::Client {

struct FramePresentation {
    std::optional<std::uint64_t> tick;
    std::optional<float> imgui_delta;
};

// Counts describe completed frames. Startup holds zero; warmup and measurement
// select the upcoming frame. Report, screenshot and AOV frames hold the final
// tick, including when the report latch increments measured_frames past its limit.
// Both UI clocks use this selection, independent of startup/reload wall time.
inline FramePresentation select_frame_presentation(bool capture_active,
                                                   bool world_ready,
                                                   std::uint32_t warmup_frames,
                                                   std::uint32_t measured_frames,
                                                   std::uint32_t warmup_limit,
                                                   std::uint32_t expected_ticks,
                                                   std::uint32_t tick_rate) {
    if (!capture_active)
        return {std::nullopt, std::nullopt};
    if (tick_rate == 0)
        throw std::invalid_argument("presentation tick rate must be positive");
    // Widen before adding so even the largest input counts cannot overflow.
    const auto warmup = static_cast<std::uint64_t>(std::min(warmup_frames, warmup_limit));
    const auto measured = static_cast<std::uint64_t>(std::min(measured_frames, expected_ticks));
    const std::uint64_t tick = !world_ready                ? 0
                               : warmup < warmup_limit     ? warmup + 1
                               : measured < expected_ticks ? warmup + measured + 1
                                                           : warmup + measured;
    return {tick, 1.0f / static_cast<float>(tick_rate)};
}

// Main-thread presentation authority. Captures start before UI initialization,
// hold tick zero through startup, and advance only with the workload frame/tick.
// Reloads and repeated renders of a tick never advance this clock.
class PresentationClock {
public:
    void set_capture_tick(std::uint64_t tick, std::uint32_t tick_rate) {
        if (tick_rate == 0)
            throw std::invalid_argument("presentation tick rate must be positive");
        capture_seconds_ = static_cast<double>(tick) / static_cast<double>(tick_rate);
    }
    void use_wall_clock() {
        capture_seconds_.reset();
    }
    const std::optional<double>& capture_seconds() const {
        return capture_seconds_;
    }
    template<class WallClock>
    double now(WallClock wall_clock) const {
        return capture_seconds_ ? *capture_seconds_ : wall_clock();
    }

private:
    std::optional<double> capture_seconds_;
};

inline PresentationClock presentation_clock;

} // namespace Luminumbra::Client
