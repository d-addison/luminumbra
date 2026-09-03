#pragma once

// Window-mode plumbing for the client app, extracted verbatim from
// main_client.cpp: the runtime window-arrangement state, the mode
// apply/toggle helpers, and the capture-thumbnail TGA writer.

#include "core/RuntimeScenarioConfig.h"

#include <filesystem>
#include <vector>

struct GLFWwindow;

namespace Luminumbra::Client::App {

// Tracks the active window arrangement plus the saved windowed geometry so the
// Alt+Enter windowed<->borderless toggle (and F11 exclusive-fullscreen toggle)
// can restore it. The framebuffer-size callback writes pending sizes here; the
// main loop debounces them to a single RenderPipeline::on_resize per settle
// window so dragging the window edge does not reallocate targets every event.
struct WindowState {
    Luminumbra::Client::ScenarioHarness::WindowMode mode =
        Luminumbra::Client::ScenarioHarness::WindowMode::Borderless;
    // Geometry to restore when leaving borderless/fullscreen back to windowed.
    int windowedX = 100, windowedY = 100;
    int windowedWidth = 1280, windowedHeight = 720;
    // Capture-pin lock: when true the window is held at the pinned capture size
    // and the runtime mode toggles are suppressed (scenario/capture runs).
    bool capture_pinned = false;

    // Debounced framebuffer resize (driven by the GLFW framebuffer-size cb).
    bool resize_pending = false;
    int pending_width = 0;
    int pending_height = 0;
    double pending_since_seconds = 0.0;
};

// Applies a window mode to an existing window (no-op for capture-pinned runs).
void ApplyWindowMode(GLFWwindow* window,
                     WindowState& state,
                     Luminumbra::Client::ScenarioHarness::WindowMode mode);

// Alt+Enter runtime toggle: windowed <-> borderless. Suppressed on
// capture-pinned (scenario) runs.
void ToggleWindowedBorderless(GLFWwindow* window, WindowState& state);

// F11 toggle: exclusive fullscreen <-> windowed (kept for back-compat with the
// previous F11 binding).
void ToggleFullscreen(GLFWwindow* window, WindowState& state);

// Write a downscaled 24-bit uncompressed TGA thumbnail of a captured frame.
bool WriteCaptureThumbnailTga(const std::filesystem::path& path,
                              int srcW,
                              int srcH,
                              const std::vector<unsigned char>& rgb,
                              int maxDim);

} // namespace Luminumbra::Client::App
