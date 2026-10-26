#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/WindowModeControls.h"

#include "core/Log.h"
#include "rendering/PixelIo.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace Luminumbra::Client::App {

using Luminumbra::Client::ScenarioHarness::WindowMode;

namespace {

// Monitor under the window's center (falls back to the primary monitor). Used
// so borderless/fullscreen target the display the window currently lives on.
GLFWmonitor* MonitorForWindow(GLFWwindow* window) {
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const int cx = wx + ww / 2;
    const int cy = wy + wh / 2;

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            return monitors[i];
        }
    }
    return glfwGetPrimaryMonitor();
}

// Saves the current windowed geometry so a later return to windowed restores it.
void SaveWindowedGeometry(GLFWwindow* window, WindowState& state) {
    glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
    glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
}

} // namespace

// Applies a window mode to an existing window. capture_pinned runs (scenario
// captures) are never reconfigured: they stay at the pinned size in a hidden /
// stable window so every pixel-ROI gate sees exactly 1280x720.
void ApplyWindowMode(GLFWwindow* window, WindowState& state, WindowMode mode) {
    if (state.capture_pinned) {
        state.mode = mode; // record intent, but do not touch the pinned window
        return;
    }
    if (mode == state.mode)
        return;

    // Leaving windowed: remember where it was so we can come back to it.
    if (state.mode == WindowMode::Windowed) {
        SaveWindowedGeometry(window, state);
    }

    GLFWmonitor* monitor = MonitorForWindow(window);
    switch (mode) {
        case WindowMode::Windowed: {
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_TRUE);
            glfwSetWindowMonitor(window,
                                 nullptr,
                                 state.windowedX,
                                 state.windowedY,
                                 state.windowedWidth,
                                 state.windowedHeight,
                                 0);
            break;
        }
        case WindowMode::Borderless: {
            // Borderless window covering the WHOLE monitor (owner default 2026-06-16:
            // make full use of the ultrawide display for reviews/critiques). Unlike
            // the work-area variant, this spans the full native resolution including
            // under the taskbar (a borderless fullscreen), without an exclusive
            // video-mode switch so alt-tab stays instant. Position = monitor origin,
            // size = native video mode.
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitor, &mx, &my);
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            const int mw = vmode ? vmode->width : 1920;
            const int mh = vmode ? vmode->height : 1080;
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowAttrib(window, GLFW_RESIZABLE, GLFW_FALSE);
            glfwSetWindowMonitor(window, nullptr, mx, my, mw, mh, 0);
            break;
        }
        case WindowMode::Fullscreen: {
            // Exclusive fullscreen at the monitor's native video mode.
            const GLFWvidmode* vmode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(
                window, monitor, 0, 0, vmode->width, vmode->height, vmode->refreshRate);
            break;
        }
        case WindowMode::Headless:
            glfwHideWindow(window);
            break;
    }
    state.mode = mode;
    LUMINUMBRA_CORE_INFO("Window mode -> {}",
                         Luminumbra::Client::ScenarioHarness::WindowModeName(mode));
}

// Alt+Enter runtime toggle: windowed <-> borderless. Suppressed on
// capture-pinned (scenario) runs.
void ToggleWindowedBorderless(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned)
        return;
    const WindowMode next =
        (state.mode == WindowMode::Windowed) ? WindowMode::Borderless : WindowMode::Windowed;
    ApplyWindowMode(window, state, next);
}

// F11 toggle: exclusive fullscreen <-> windowed (kept for back-compat with the
// previous F11 binding).
void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.capture_pinned)
        return;
    const WindowMode next =
        (state.mode == WindowMode::Fullscreen) ? WindowMode::Windowed : WindowMode::Fullscreen;
    ApplyWindowMode(window, state, next);
}

bool WriteCaptureThumbnailTga(const std::filesystem::path& path,
                              int srcW,
                              int srcH,
                              const std::vector<unsigned char>& rgb,
                              int maxDim) {
    return Luminumbra::Rendering::WritePixelBufferThumbnailTga(path, srcW, srcH, rgb, maxDim);
}

} // namespace Luminumbra::Client::App
