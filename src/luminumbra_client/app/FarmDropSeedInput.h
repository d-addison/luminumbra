#pragma once

#include "ui/KeyboardCapture.h"

namespace Luminumbra::Client::App {

// The complete input gate shared by the GLFW adapter and headless client tests.
// Runtime queries stay lazy so blocked HUD/overlay states never query UI capture.
template<typename App, typename Input>
bool FarmDropSeedInputAllowed(const App& app, const Input& input) {
    return input.WindowFocused() && !app.hud.paused && !app.hud.show_settings &&
           app.hud.rebindCaptureAction < 0 && !app.hud.codexOpen && !app.hud.photoModeUiShown &&
           !app.overlay.show_gpu_profiler && !app.overlay.show_shader_panel &&
           !app.overlay.show_worldgen_viewer && !input.PlayerPhotoModeActive() &&
           !input.ImGuiCapturesKeyboard() &&
           !FocusedElementCapturesKeyboard(input.GetFocusElement());
}

} // namespace Luminumbra::Client::App
