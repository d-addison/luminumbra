#pragma once

#include "core/InspectorRegistry.h"

#include <cstddef>
#include <optional>
#include <string>

namespace Luminumbra::Client::Debug {
// Presentation samples only: frame interval is observed before the gameplay clamp.
struct PerfHudSample {
    double frame_seconds = 0.0;
    std::optional<double> ui_submit_ms;
    std::optional<std::size_t> terrain_draws;
    std::optional<std::uint64_t> observed_frame = {};
};
std::optional<luminumbra::core::InspectorPanelHandle>
RegisterPerfHudInspector(luminumbra::core::InspectorRegistry& registry);
bool PublishPerfHudInspector(luminumbra::core::InspectorRegistry& registry,
                             luminumbra::core::InspectorPanelHandle handle,
                             const PerfHudSample& sample);
void DrawPerfHudContents(const std::optional<luminumbra::core::InspectorPanelSnapshot>& snapshot);
void DrawTimeIndicatorContents(float scale, const std::string& reset_binding);
} // namespace Luminumbra::Client::Debug
