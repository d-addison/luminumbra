#include "debug/widgets/PassiveDeveloperWidgets.h"

#include <cmath>
#include <imgui.h>

namespace Luminumbra::Client::Debug {
std::optional<luminumbra::core::InspectorPanelHandle>
RegisterPerfHudInspector(luminumbra::core::InspectorRegistry& registry) {
    using luminumbra::core::InspectorMetricDefinition;
    constexpr std::array definitions{
        InspectorMetricDefinition{
            "frame", "Frame interval", "CPU observed frame interval", "ms", 0},
        InspectorMetricDefinition{"ui", "UI submit (CPU)", "CPU last completed UI submit", "ms", 0},
        InspectorMetricDefinition{
            "terrain", "Terrain draws", "CPU last render counters", "draws", 0}};
    return registry.Register("perf.hud", "Frame performance", definitions);
}

bool PublishPerfHudInspector(luminumbra::core::InspectorRegistry& registry,
                             luminumbra::core::InspectorPanelHandle handle,
                             const PerfHudSample& sample) {
    using luminumbra::core::InspectorMetricSample;
    using luminumbra::core::MetricAvailability;
    std::array<InspectorMetricSample, 3> metrics{};
    const double frame_ms = sample.frame_seconds * 1000.0;
    if (std::isfinite(frame_ms) && frame_ms > 0.0) {
        metrics[0] = {MetricAvailability::Available, frame_ms, sample.observed_frame};
    } else if (sample.frame_seconds != 0.0) {
        metrics[0].availability = MetricAvailability::Unavailable;
    }
    // Legacy UI/render counters expose no producer frame. Preserve unknown age
    // rather than stamping the query frame onto an older measurement.
    if (sample.ui_submit_ms && std::isfinite(*sample.ui_submit_ms) && *sample.ui_submit_ms > 0.0) {
        metrics[1] = {MetricAvailability::Available, *sample.ui_submit_ms, {}};
    } else {
        metrics[1].availability = MetricAvailability::Unavailable;
    }
    // A double represents all integer counts through 2^53 exactly.
    constexpr std::uint64_t exact_count_limit = std::uint64_t{1} << 53;
    if (sample.terrain_draws && *sample.terrain_draws <= exact_count_limit) {
        metrics[2] = {
            MetricAvailability::Available, static_cast<double>(*sample.terrain_draws), {}};
    } else {
        metrics[2].availability = MetricAvailability::Unavailable;
    }
    return registry.Publish(handle, metrics);
}

void DrawPerfHudContents(const std::optional<luminumbra::core::InspectorPanelSnapshot>& snapshot) {
    if (!snapshot) {
        ImGui::TextUnformatted("Frame performance unavailable");
        return;
    }
    const auto& frame = snapshot->metrics[0];
    if (snapshot->metric_count > 0 && std::string_view(frame.id.data()) == "frame" && frame.value &&
        *frame.value > 0.0 && std::isfinite(1000.0 / *frame.value)) {
        ImGui::Text("%.1f FPS", 1000.0 / *frame.value);
    }
    // Snapshot counts are bounded by the engine registry's fixed metric limit.
    for (std::size_t index = 0; index < std::min(snapshot->metric_count, snapshot->metrics.size());
         ++index) {
        const auto& metric = snapshot->metrics[index];
        if (metric.value) {
            ImGui::Text(
                "%s: %.3f %s (available)", metric.label.data(), *metric.value, metric.unit.data());
        } else {
            ImGui::Text("%s: unavailable (%s)",
                        metric.label.data(),
                        luminumbra::core::MetricAvailabilityName(metric.availability));
        }
        if (metric.age_frames) {
            ImGui::Text("  source: %s; age: %llu frames",
                        metric.source.data(),
                        static_cast<unsigned long long>(*metric.age_frames));
        } else {
            ImGui::Text("  source: %s; age: unknown", metric.source.data());
        }
    }
}
void DrawTimeIndicatorContents(float scale, const std::string& reset_binding) {
    if (scale == 0.0f)
        ImGui::Text("PAUSED  /  time.reset: %s", reset_binding.c_str());
    else
        ImGui::Text("TIME  x%.2f", scale);
}
} // namespace Luminumbra::Client::Debug
