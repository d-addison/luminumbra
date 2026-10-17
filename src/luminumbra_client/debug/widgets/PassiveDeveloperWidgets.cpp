#include "debug/widgets/PassiveDeveloperWidgets.h"

#include <cmath>
#include <imgui.h>

namespace Luminumbra::Client::Debug {
void DrawPerfHudContents(const PerfHudSample& sample) {
    if (std::isfinite(sample.frame_seconds) && sample.frame_seconds > 0.0)
        ImGui::Text(
            "%.1f FPS  /  %.2f ms", 1.0 / sample.frame_seconds, sample.frame_seconds * 1000.0);
    else
        ImGui::TextUnformatted("Frame interval unavailable");
    if (sample.ui_submit_ms)
        ImGui::Text("UI submit (CPU): %.3f ms", *sample.ui_submit_ms);
    if (sample.terrain_draws)
        ImGui::Text("Terrain draws: %zu", *sample.terrain_draws);
}
void DrawTimeIndicatorContents(float scale, const std::string& reset_binding) {
    if (scale == 0.0f)
        ImGui::Text("PAUSED  /  time.reset: %s", reset_binding.c_str());
    else
        ImGui::Text("TIME  x%.2f", scale);
}
} // namespace Luminumbra::Client::Debug
