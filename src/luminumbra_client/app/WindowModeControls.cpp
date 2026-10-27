#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/WindowModeControls.h"

#include "core/Log.h"

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

// Write a downscaled 24-bit uncompressed TGA thumbnail of a captured frame. `rgb` is the
// glReadPixels buffer (bottom-up, RGB); TGA with descriptor=0 is bottom-up origin too, so the
// rows map directly. RmlUi's GL3 backend only decodes TGA, so the gallery thumbs are TGA. The
// downscale is nearest-neighbour (thumbnails don't need filtering) and keeps a small on-disk size.
bool WriteCaptureThumbnailTga(const std::filesystem::path& path,
                              int srcW,
                              int srcH,
                              const std::vector<unsigned char>& rgb,
                              int maxDim) {
    if (srcW <= 0 || srcH <= 0 || rgb.size() < static_cast<std::size_t>(srcW) * srcH * 3u)
        return false;
    const float scale =
        std::min(1.0f, static_cast<float>(maxDim) / static_cast<float>(std::max(srcW, srcH)));
    const int dstW = std::max(1, static_cast<int>(static_cast<float>(srcW) * scale));
    const int dstH = std::max(1, static_cast<int>(static_cast<float>(srcH) * scale));
    std::vector<unsigned char> bgr(static_cast<std::size_t>(dstW) * dstH * 3u);
    for (int y = 0; y < dstH; ++y) {
        const int sy =
            std::min(srcH - 1, static_cast<int>((static_cast<float>(y) + 0.5f) / dstH * srcH));
        for (int x = 0; x < dstW; ++x) {
            const int sx =
                std::min(srcW - 1, static_cast<int>((static_cast<float>(x) + 0.5f) / dstW * srcW));
            const unsigned char* s = &rgb[(static_cast<std::size_t>(sy) * srcW + sx) * 3u];
            unsigned char* d = &bgr[(static_cast<std::size_t>(y) * dstW + x) * 3u];
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0]; // RGB -> BGR
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    unsigned char hdr[18] = {0};
    hdr[2] = 2; // uncompressed true-color
    hdr[12] = static_cast<unsigned char>(dstW & 0xFF);
    hdr[13] = static_cast<unsigned char>((dstW >> 8) & 0xFF);
    hdr[14] = static_cast<unsigned char>(dstH & 0xFF);
    hdr[15] = static_cast<unsigned char>((dstH >> 8) & 0xFF);
    hdr[16] = 24; // bits per pixel
    hdr[17] = 0;  // descriptor: bottom-up origin (matches the GL buffer)
    out.write(reinterpret_cast<const char*>(hdr), 18);
    out.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    return static_cast<bool>(out);
}

} // namespace Luminumbra::Client::App
