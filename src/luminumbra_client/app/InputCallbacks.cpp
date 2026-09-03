#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/InputCallbacks.h"

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h" // GL debug counters + window-mode helpers
#include "luminumbra_common/core/SystemConfig.h"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "ui/Rml_UIManager.h"

#include <algorithm>
#include <atomic>
#include <imgui.h>

namespace Luminumbra::Client::App {

// GL debug counters + the window-mode helpers the callbacks use live in the
// scenario-harness namespace, exactly as main_client.cpp consumed them.
using namespace Luminumbra::Client::ScenarioHarness;

namespace {

// The bindings main_client.cpp installed on the window at setup.
InputCallbackBindings& CallbackBindings(GLFWwindow* window) {
    return *static_cast<InputCallbackBindings*>(glfwGetWindowUserPointer(window));
}

} // namespace

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    luminumbra::core::SystemConfig& systemConfig = *bindings.systemConfig;
    WindowState& windowState = *bindings.windowState;
    Luminumbra::Client::PlayerController* playerController = bindings.playerController->get();
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    // Rebind capture: while waiting for a key for some action, the next key press becomes
    // its binding (Escape cancels). Intercept first so any key — even F-keys — can be bound.
    if (app.hud.rebindCaptureAction >= 0 && action == GLFW_PRESS) {
        if (key != GLFW_KEY_ESCAPE &&
            app.hud.rebindCaptureAction < static_cast<int>(Luminumbra::Client::kInputActionCount)) {
            const char* name =
                Luminumbra::Client::kInputActionDefs[app.hud.rebindCaptureAction].name;
            systemConfig.user().keybinds[name] = key;
            if (playerController)
                playerController->ApplyKeyBindings(systemConfig);
        }
        app.hud.rebindCaptureAction = -1;
        return;
    }
    // Escape: toggle the in-game pause overlay (only in a world, and not while the  panel is up).
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && playerController &&
        !app.hud.show_settings) {
        SetGamePaused(window, !app.hud.paused);
        return;
    }
    // toggle the live per-pass GPU profiler overlay.
    if (key == GLFW_KEY_F3 && action == GLFW_PRESS) {
        app.overlay.show_gpu_profiler = !app.overlay.show_gpu_profiler;
        return;
    }
    // crawl — hot-reload every live shader from res/shaders/ next
    // frame (rollback-safe per shader; a broken edit keeps the prior program).
    if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
        app.overlay.request_shader_reload = true;
        return;
    }
    // walk — the dev shader panel (per-shader reload, auto-reload
    // watcher toggle, live uniform editing).
    if (key == GLFW_KEY_F10 && action == GLFW_PRESS) {
        app.overlay.show_shader_panel = !app.overlay.show_shader_panel;
        if (app.overlay.show_shader_panel) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (playerController) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
        return;
    }
    // cycle the render-only G-buffer debug view (off -> albedo -> normal -> depth ->
    // material -> position -> off). Diagnostic only; never affects sim/world_hash. The main
    // loop pushes app.overlay.debug_view_mode into the pipeline each frame.
    if (key == GLFW_KEY_F6 && action == GLFW_PRESS) {
        app.overlay.debug_view_mode = (app.overlay.debug_view_mode + 1) % 6; // 0..5
        return;
    }
    // toggle the settings menu and free/restore the cursor so the panel is usable.
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
        app.hud.show_settings = !app.hud.show_settings;
        app.hud.rebindCaptureAction = -1;
        if (app.hud.show_settings) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (playerController) { // in a world -> resume mouse-look
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            app.input.firstMouse = true; // avoid a camera jump when mouse-look resumes
        }
        return;
    }
    // Engine time scale (host_timescale-style): [ slower, ] faster, \ reset to 1x.
    if (action == GLFW_PRESS && (key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET ||
                                 key == GLFW_KEY_BACKSLASH)) {
        if (key == GLFW_KEY_LEFT_BRACKET)
            app.capture.timeScale = (app.capture.timeScale <= 0.125f)
                                        ? 0.0f
                                        : app.capture.timeScale * 0.5f; //...down to pause
        else if (key == GLFW_KEY_RIGHT_BRACKET)
            app.capture.timeScale = (app.capture.timeScale < 0.125f)
                                        ? 0.125f
                                        : std::min(app.capture.timeScale * 2.0f, 16.0f);
        else
            app.capture.timeScale = 1.0f; // reset
        LUMINUMBRA_CORE_INFO("Engine time scale: {:.3f}x", app.capture.timeScale);
        return;
    }
    if (key == GLFW_KEY_F7 && action == GLFW_PRESS) {
        app.overlay.show_worldgen_viewer = !app.overlay.show_worldgen_viewer;
        if (app.overlay.show_worldgen_viewer) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        return;
    }
    // Alt+Enter: runtime windowed <-> borderless toggle.
    if (key == GLFW_KEY_ENTER && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
        ToggleWindowedBorderless(window, windowState);
        return;
    }
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        ToggleFullscreen(window, windowState);
        return;
    }
    if (key == GLFW_KEY_F9 && action == GLFW_PRESS) {
        app.overlay.wireframe_mode = !app.overlay.wireframe_mode;
        return;
    }

    if (playerController) {
        playerController->ProcessKeyInput(key, action);
    }

    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard) {
            return;
        }
    }

    // Always give RmlUi a chance to process key events.
    if (uiManager) {
        uiManager->KeyCallback(window, key, scancode, action, mods);
        // If RmlUi has a focused input element, it should consume the event.
        if (uiManager->GetContext() && uiManager->GetContext()->GetFocusElement()) {
            return;
        }
    }
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    gameStateManager.SetState(newState);
    bool cursorDisabled = (newState == GameState::IN_GAME);

    if (cursorDisabled) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        // Set game-related callbacks
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        // Mouse buttons could be set here for game actions if needed
        glfwSetMouseButtonCallback(window, nullptr);
        app.input.firstMouse = true;
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (uiManager) {
            // Set UI-related callbacks
            glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
            glfwSetScrollCallback(window,
                                  menu_scroll_callback); // RmlUi + create-world preview zoom
            glfwSetMouseButtonCallback(window,
                                       Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
        } else {
            glfwSetCursorPosCallback(window, nullptr);
            glfwSetScrollCallback(window, nullptr);
            glfwSetMouseButtonCallback(window, nullptr);
        }
    }
}

void SetGamePaused(GLFWwindow* window, bool paused) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    app.hud.paused = paused;
    if (paused) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (uiManager) {
            glfwSetCursorPosCallback(window, Luminumbra::Client::Rml_UIManager::CursorPosCallback);
            glfwSetMouseButtonCallback(window,
                                       Luminumbra::Client::Rml_UIManager::MouseButtonCallback);
            glfwSetScrollCallback(window, Luminumbra::Client::Rml_UIManager::ScrollCallback);
            uiManager->RequestLoadDocument("pause.rml");
        }
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, nullptr);
        app.input.firstMouse = true;
        app.hud.photoModeUiShown = false; // re-sync the in-game overlay next frame
        if (uiManager)
            uiManager->RequestLoadDocument("hud.rml");
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    Luminumbra::Rendering::Camera* camera = bindings.camera->get();
    (void)window;
    if (app.hud.show_settings || app.hud.paused)
        return; // settings/pause open (cursor freed) -> don't swing the camera
    if (app.input.firstMouse) {
        app.input.lastX = (float)xpos;
        app.input.lastY = (float)ypos;
        app.input.firstMouse = false;
    }
    float xoffset = (float)xpos - app.input.lastX;
    float yoffset = app.input.lastY - (float)ypos;
    app.input.lastX = (float)xpos;
    app.input.lastY = (float)ypos;
    if (camera)
        camera->ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    Luminumbra::Rendering::Camera* camera = bindings.camera->get();
    Luminumbra::Client::PlayerController* playerController = bindings.playerController->get();
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    (void)window;
    (void)xoffset;
    const bool imgui_wants_mouse =
        app.overlay.imgui_enabled && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    if (imgui_wants_mouse || (uiManager && uiManager->GetContext()->GetHoverElement() != nullptr)) {
        return;
    }
    if (camera)
        camera->ProcessMouseScroll((float)yoffset);
    if (playerController)
        playerController->ProcessMouseScroll(yoffset);
}

void menu_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    // Keep RmlUi's scroll behaviour for menu lists/galleries...
    Luminumbra::Client::Rml_UIManager::ScrollCallback(window, xoffset, yoffset);
    //...and accrue the vertical wheel delta for the create-world preview zoom.
    // The preview block consumes + resets this each frame (only when over the pane).
    app.menu.menu_scroll_accum += yoffset;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    WindowState& windowState = *bindings.windowState;
    (void)window;
    if (width <= 0 || height <= 0)
        return; // minimized window: ignore
    // Capture-pinned (scenario) runs must never resize their targets; the gate
    // depends on a fixed 1280x720 framebuffer.
    if (windowState.capture_pinned)
        return;
    // Debounce: record the pending size and let the main loop coalesce a burst
    // of drag events into one RenderPipeline::on_resize after the size settles.
    windowState.pending_width = width;
    windowState.pending_height = height;
    windowState.pending_since_seconds = glfwGetTime();
    windowState.resize_pending = true;
}

void GLFWErrorCallback(int error, const char* description) {
    LUMINUMBRA_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
}

void GLAPIENTRY GLDebugMessageCallback(GLenum source,
                                       GLenum type,
                                       GLuint id,
                                       GLenum severity,
                                       GLsizei length,
                                       const GLchar* message,
                                       const void* userParam) {
    (void)source;
    (void)length;
    (void)userParam;
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204)
        return;
    g_gl_debug_message_count.fetch_add(1, std::memory_order_relaxed);
    const bool is_error = type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH;
    if (is_error) {
        g_gl_debug_error_count.fetch_add(1, std::memory_order_relaxed);
        LUMINUMBRA_CORE_ERROR("OpenGL: {0}", message);
        return;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            g_gl_debug_notification_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_TRACE("OpenGL: {0}", message);
            break;
        default:
            g_gl_debug_warning_count.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_WARN("OpenGL: {0}", message);
            break;
    }
}

} // namespace Luminumbra::Client::App
