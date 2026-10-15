#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/InputCallbacks.h"

#include "core/Log.h"
#include "core/RuntimeScenarioConfig.h" // GL debug counters + window-mode helpers
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

void RegionPanelMouseButton(GLFWwindow* window, int button, int action, int) {
    const auto& app = *CallbackBindings(window).app;
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext() && button >= 0 &&
        button < ImGuiMouseButton_COUNT)
        ImGui::GetIO().AddMouseButtonEvent(button, action == GLFW_PRESS);
}

ImGuiKey RegionPanelKey(int key) {
    switch (key) {
        case GLFW_KEY_TAB:
            return ImGuiKey_Tab;
        case GLFW_KEY_LEFT:
            return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT:
            return ImGuiKey_RightArrow;
        case GLFW_KEY_UP:
            return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN:
            return ImGuiKey_DownArrow;
        case GLFW_KEY_HOME:
            return ImGuiKey_Home;
        case GLFW_KEY_END:
            return ImGuiKey_End;
        case GLFW_KEY_PAGE_UP:
            return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN:
            return ImGuiKey_PageDown;
        case GLFW_KEY_SPACE:
            return ImGuiKey_Space;
        case GLFW_KEY_ENTER:
            return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE:
            return ImGuiKey_Escape;
        default:
            return ImGuiKey_None;
    }
}

} // namespace

void UpdateGameplayPlayer(const RegionPanelState& panel,
                          PlayerController& player,
                          float delta_time) {
    if (panel.open) {
        player.DiscardPendingMovementInput();
        player.ApplyReplayInput(delta_time, PlayerReplayInputFrame{});
    } else {
        player.Update(delta_time);
    }
}

PlayerVerbPresses PollPlayerVerbPresses(ClientAppContext& app,
                                        GLFWwindow* window,
                                        const PlayerController& player,
                                        int (*read_key)(GLFWwindow*, int)) {
    const auto edge = [&](int key_code, bool& was_down) {
        const bool down =
            (read_key ? read_key(window, key_code) : glfwGetKey(window, key_code)) == GLFW_PRESS;
        bool suppressed = app.overlay.region_panel.open;
        if (key_code >= 0 && key_code <= GLFW_KEY_LAST) {
            auto& captured =
                app.overlay.region_panel_captured_keys[static_cast<std::size_t>(key_code)];
            if (!down)
                captured = false;
            else if (app.overlay.region_panel.open)
                captured = true;
            suppressed = suppressed || captured;
        }
        const bool fired = down && !was_down && !suppressed;
        was_down = down;
        return fired;
    };
    auto& previous = app.interactions;
    PlayerVerbPresses presses;
    presses.farmPlant = edge(player.key(InputAction::FarmPlant), previous.farmPlant);
    presses.farmWater = edge(player.key(InputAction::FarmWater), previous.farmWater);
    presses.farmFertilize = edge(player.key(InputAction::FarmFertilize), previous.farmFertilize);
    presses.farmHarvest = edge(player.key(InputAction::FarmHarvest), previous.farmHarvest);
    presses.farmCycleSpecies = edge(GLFW_KEY_V, previous.farmCycleSpecies);
    presses.terraformDig = edge(GLFW_KEY_R, previous.terraformDig);
    presses.terraformFill = edge(GLFW_KEY_T, previous.terraformFill);
    return presses;
}

void CloseRegionPanelInput(GLFWwindow* window) {
    auto& app = *CallbackBindings(window).app;
    app.overlay.region_panel.open = false;
    if (!app.overlay.region_panel_input_attached)
        return;
    if (ImGui::GetCurrentContext()) {
        auto& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
    }
    glfwSetMouseButtonCallback(window, app.overlay.region_panel_previous_mouse_button);
    glfwSetCursorPosCallback(window, app.overlay.region_panel_previous_cursor);
    glfwSetScrollCallback(window, app.overlay.region_panel_previous_scroll);
    app.overlay.region_panel_previous_mouse_button = nullptr;
    app.overlay.region_panel_previous_cursor = nullptr;
    app.overlay.region_panel_previous_scroll = nullptr;
    app.overlay.region_panel_input_attached = false;
    const bool showCursor = app.hud.show_settings || app.hud.paused ||
                            app.overlay.show_shader_panel || app.overlay.show_worldgen_viewer;
    const int cursorMode =
        showCursor ? GLFW_CURSOR_NORMAL : app.overlay.region_panel_previous_cursor_mode;
    glfwSetInputMode(window, GLFW_CURSOR, cursorMode);
    if (cursorMode == GLFW_CURSOR_DISABLED)
        app.input.firstMouse = true;
}

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
    // Read-only region diagnostics take ownership of interactive input.
    if (key == GLFW_KEY_F4 && action == GLFW_PRESS) {
        if (!app.overlay.region_panel.open &&
            (!app.overlay.region_panel_available || !app.overlay.imgui_enabled ||
             !ImGui::GetCurrentContext()))
            return;
        if (app.overlay.region_panel.open) {
            CloseRegionPanelInput(window);
            return;
        }
        app.overlay.region_panel.open = true;
        // A gameplay press earlier in this event batch may already be queued.
        if (playerController)
            playerController->DiscardPendingMovementInput();
        app.overlay.region_panel_previous_cursor_mode = glfwGetInputMode(window, GLFW_CURSOR);
        app.overlay.region_panel_previous_mouse_button =
            glfwSetMouseButtonCallback(window, RegionPanelMouseButton);
        app.overlay.region_panel_previous_cursor = glfwSetCursorPosCallback(window, mouse_callback);
        app.overlay.region_panel_previous_scroll = glfwSetScrollCallback(window, scroll_callback);
        app.overlay.region_panel_input_attached = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        return;
    }
    if (key >= 0 && key <= GLFW_KEY_LAST) {
        auto& captured = app.overlay.region_panel_captured_keys[static_cast<std::size_t>(key)];
        const bool was_captured = captured;
        if (action == GLFW_RELEASE)
            captured = false;
        else if (app.overlay.region_panel.open)
            captured = true;
        if (!app.overlay.region_panel.open && was_captured)
            return;
    }
    if (app.overlay.region_panel.open) {
        if (app.overlay.imgui_enabled && ImGui::GetCurrentContext()) {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Shift, (mods & GLFW_MOD_SHIFT) != 0);
            if (const auto panel_key = RegionPanelKey(key);
                panel_key != ImGuiKey_None && (action == GLFW_PRESS || action == GLFW_RELEASE))
                io.AddKeyEvent(panel_key, action == GLFW_PRESS);
        }
        return;
    }
    // Escape: toggle the in-game pause overlay (only in a world, and not while the panel is up).
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
    CloseRegionPanelInput(window);
    gameStateManager.SetState(newState);
    app.overlay.region_panel_available = newState == GameState::IN_GAME;
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
    if (app.overlay.region_panel.open) {
        if (app.overlay.imgui_enabled && ImGui::GetCurrentContext())
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(xpos), static_cast<float>(ypos));
        return;
    }
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
    if (app.overlay.region_panel.open) {
        if (app.overlay.imgui_enabled && ImGui::GetCurrentContext())
            ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(xoffset),
                                              static_cast<float>(yoffset));
        return;
    }
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
