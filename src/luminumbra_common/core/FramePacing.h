#pragma once

namespace luminumbra::core {

// Persisted user.video.frame_cap: zero means unlimited.
inline constexpr int kFrameCaps[] = {0, 30, 60, 120, 144, 165, 240};

[[nodiscard]] constexpr bool IsValidFrameCap(int cap) {
    for (const int supported_cap : kFrameCaps) {
        if (cap == supported_cap)
            return true;
    }
    return false;
}

struct FramePacing {
    bool vsync;
    int frame_cap;
};

struct FramePacingOverrides {
    bool benchmark_active = false;
    bool scenario_active = false;
    bool profiling_active = false;
};

// Measurement overrides affect presentation only, never the saved player choices.
[[nodiscard]] constexpr FramePacing
ResolveFramePacing(bool vsync, int cap, FramePacingOverrides overrides = {}) {
    const bool force_uncapped =
        overrides.benchmark_active || overrides.scenario_active || overrides.profiling_active;
    return {.vsync = !force_uncapped && vsync,
            .frame_cap = !force_uncapped && IsValidFrameCap(cap) ? cap : 0};
}

} // namespace luminumbra::core
