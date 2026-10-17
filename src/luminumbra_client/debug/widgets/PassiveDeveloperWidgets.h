#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace Luminumbra::Client::Debug {
// Presentation samples only: frame interval is observed before the gameplay clamp.
struct PerfHudSample {
    double frame_seconds = 0.0;
    std::optional<double> ui_submit_ms;
    std::optional<std::size_t> terrain_draws;
};
void DrawPerfHudContents(const PerfHudSample& sample);
void DrawTimeIndicatorContents(float scale, const std::string& reset_binding);
} // namespace Luminumbra::Client::Debug
