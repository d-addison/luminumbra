#pragma once

#include "ui/KeyboardCapture.h"

namespace Luminumbra::Client::App {

// The complete input gate shared by the GLFW adapter and headless client tests.
// Runtime queries stay lazy so blocked HUD/overlay states never query UI capture.
template<typename App, typename Input>
bool FarmDropSeedInputAllowed(const App& app, const Input& input) {
    return input.WindowFocused() && input.GameplayKeyAllowed() && !app.hud.paused &&
           !input.DeveloperPanelVisible("client.settings") && app.hud.rebindCaptureAction < 0 &&
           !app.hud.codexOpen && !app.hud.photoModeUiShown &&
           !input.DeveloperPanelVisible("perf.gpu") &&
           !input.DeveloperPanelVisible("render.shaders") &&
           !input.DeveloperPanelVisible("world.generation") && !input.PlayerPhotoModeActive() &&
           !input.ImGuiCapturesKeyboard() &&
           !FocusedElementCapturesKeyboard(input.GetFocusElement());
}

} // namespace Luminumbra::Client::App
