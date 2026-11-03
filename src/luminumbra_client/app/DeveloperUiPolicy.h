#pragma once

namespace Luminumbra::Client::App {
// Render-only overrides; never mutate saved developer preferences.
struct DeveloperUiPolicy {
    bool imgui_enabled = true;
    bool scenario_active = false;
    bool timelapse = false;
    bool scene = false;
    bool frame_scan = false;
    bool render_parity = false;
    bool render_benchmark = false;
    bool survey = false;
    bool bake_impostor = false;
    bool ui_screenshot = false;
    bool ui_thumbnails = false;
};
inline bool DeveloperUiAllowed(const DeveloperUiPolicy& policy) {
    return policy.imgui_enabled && !policy.scenario_active && !policy.timelapse && !policy.scene &&
           !policy.frame_scan && !policy.render_parity && !policy.render_benchmark &&
           !policy.survey && !policy.bake_impostor && !policy.ui_screenshot &&
           !policy.ui_thumbnails;
}
} // namespace Luminumbra::Client::App
