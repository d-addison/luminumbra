#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "app/DeveloperKeyDispatch.h"
#include "app/GameplayBindingCapture.h"
#include "app/InputCallbacks.h"

#include "core/Log.h"
#include "core/RuntimeScenarioConfig.h" // GL debug counters + window-mode helpers
#include "luminumbra_common/core/SystemConfig.h"
#include "player/PlayerController.h"
#include "rendering/Camera.h"
#include "ui/Rml_UIManager.h"

#include <algorithm>
#include <atomic>
#include <backends/imgui_impl_glfw.h>
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
                                        int (*read_key)(GLFWwindow*, int),
                                        const UiInputRouter* router) {
    const auto edge = [&](int key_code, bool& was_down) {
        const bool down =
            (read_key ? read_key(window, key_code) : glfwGetKey(window, key_code)) == GLFW_PRESS;
        bool suppressed =
            app.overlay.region_panel.open || (router && !router->GameplayKeyAllowed(key_code));
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
    RefreshInputOwnership(window);
    if (!CallbackBindings(window).router) {
        glfwSetInputMode(window, GLFW_CURSOR, app.overlay.region_panel_previous_cursor_mode);
        if (app.overlay.region_panel_previous_cursor_mode == GLFW_CURSOR_DISABLED)
            app.input.firstMouse = true;
    }
}

void InstallInputCallbacks(GLFWwindow* window) {
    glfwSetKeyCallback(window, key_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetWindowFocusCallback(window, window_focus_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);
}

void RefreshInputOwnership(GLFWwindow* window) {
    auto& b = CallbackBindings(window);
    auto& app = *b.app;
    if (!b.router)
        return;
    const bool ui_enabled = DeveloperUiAllowed(app);
    const bool imgui = ui_enabled && ImGui::GetCurrentContext();
    const auto* ui = b.uiManager->get();
    UiInputOwnership ownership;
    ownership.in_world = b.in_world;
    ownership.window_focused = b.window_focused;
    ownership.binding_capture =
        app.hud.rebindCaptureAction >= 0 ||
        (ui_enabled && b.developerConsole && b.developerConsole->IsCapturing());
    ownership.console_open = ui_enabled && b.developerConsole && b.developerConsole->IsOpen();
    ownership.interactive_panel =
        app.overlay.region_panel.open ||
        (ui_enabled && b.developerConsole &&
         b.developerConsole->Widgets().HasInteractivePanel("ui.debugger"));
    ownership.rml_debugger = ui_enabled && ui && ui->DeveloperDebuggerVisible();
    ownership.rml_interaction = ownership.rml_debugger || app.hud.paused || app.hud.codexOpen ||
                                (ui && ui->HasInputInteraction());
    ownership.rml_text_input = ui && ui->HasTextInputFocus();
    ownership.imgui_text_input = imgui && ImGui::GetIO().WantTextInput;
    ownership.imgui_keyboard = imgui && ImGui::GetIO().WantCaptureKeyboard;
    ownership.imgui_mouse = imgui && ImGui::GetIO().WantCaptureMouse;
    if (b.router->SetOwnership(ownership)) {
        app.interactions = {};
        if (auto* player = b.playerController->get())
            player->ClearQueuedLiveInput();
    }
    const int cursor = b.router->CursorCaptured() ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
    if (glfwGetInputMode(window, GLFW_CURSOR) != cursor) {
        glfwSetInputMode(window, GLFW_CURSOR, cursor);
        app.input.firstMouse = true;
    }
}

void char_callback(GLFWwindow* window, unsigned int codepoint) {
    auto& b = CallbackBindings(window);
    RefreshInputOwnership(window);
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return;
    const auto owner = b.router ? b.router->Character(codepoint) : InputOwner::Developer;
    if (owner == InputOwner::Unfocused)
        return;
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_CharCallback(window, codepoint);
    if (owner == InputOwner::RmlUi)
        Luminumbra::Client::Rml_UIManager::CharCallback(window, codepoint);
}
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    RefreshInputOwnership(window);
    if (b.router && (b.router->Owner() == InputOwner::RmlUi || action == GLFW_RELEASE))
        Luminumbra::Client::Rml_UIManager::MouseButtonCallback(window, button, action, mods);
    RefreshInputOwnership(window);
}
void window_focus_callback(GLFWwindow* window, int focused) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_WindowFocusCallback(window, focused);
    b.window_focused = focused != 0;
    RefreshInputOwnership(window);
}
void cursor_enter_callback(GLFWwindow* window, int entered) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    luminumbra::core::SystemConfig& systemConfig = *bindings.systemConfig;
    WindowState& windowState = *bindings.windowState;
    Luminumbra::Client::PlayerController* playerController = bindings.playerController->get();
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    RefreshInputOwnership(window);
    auto* console = DeveloperUiAllowed(app) ? bindings.developerConsole : nullptr;
    const bool photo = playerController && playerController->photo_mode_active();
    const bool worldShortcuts =
        bindings.router ? bindings.router->WorldShortcutsAllowed() : bindings.in_world;
    const unsigned scope =
        worldShortcuts ? (photo ? Debug::Photo : Debug::Gameplay) : Debug::Global;
    // Panel input still updates the router's held/release state, without firing shortcuts.
    const auto route =
        app.overlay.region_panel.open
            ? (bindings.router ? bindings.router->Key(key, action, false, false) : KeyRoute::Ignore)
            : DispatchDeveloperKey(console, bindings.router, key, action, mods, scope);
    // Read-only region diagnostics take ownership of interactive input.
    if (key == GLFW_KEY_F4 && action == GLFW_PRESS &&
        (app.overlay.region_panel.open ||
         (route == KeyRoute::Gameplay && DeveloperUiAllowed(app) &&
          app.hud.rebindCaptureAction < 0 &&
          (!bindings.router || bindings.router->GameplayAllowed())))) {
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
            playerController->ClearQueuedLiveInput();
        app.overlay.region_panel_previous_cursor_mode = glfwGetInputMode(window, GLFW_CURSOR);
        app.overlay.region_panel_previous_mouse_button =
            glfwSetMouseButtonCallback(window, RegionPanelMouseButton);
        app.overlay.region_panel_previous_cursor = glfwSetCursorPosCallback(window, mouse_callback);
        app.overlay.region_panel_previous_scroll = glfwSetScrollCallback(window, scroll_callback);
        app.overlay.region_panel_input_attached = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        RefreshInputOwnership(window);
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
    if (action == GLFW_RELEASE) {
        if (uiManager)
            uiManager->KeyCallback(window, key, scancode, action, mods);
        return;
    }
    if (route == KeyRoute::Capture && console && console->IsCapturing()) {
        if (action == GLFW_PRESS)
            console->CaptureKey(key, mods);
        RefreshInputOwnership(window);
        return;
    }
    if (route == KeyRoute::ConsoleToggle) {
        if (console)
            console->InvokeAction("console.toggle");
        RefreshInputOwnership(window);
        return;
    }
    if (route == KeyRoute::DeveloperEscape) {
        if (console && console->CloseTopInteraction()) {
            RefreshInputOwnership(window);
            return;
        }
        RefreshInputOwnership(window);
        return;
    }
    if (route == KeyRoute::DeveloperShortcut) {
        RefreshInputOwnership(window);
        return;
    }
    if (route != KeyRoute::Capture &&
        (!bindings.router || bindings.router->Owner() != InputOwner::Unfocused)) {
        // Alt+Enter: runtime windowed <-> borderless toggle.
        if (key == GLFW_KEY_ENTER && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
            ToggleWindowedBorderless(window, windowState);
            RefreshInputOwnership(window);
            return;
        }
        if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
            ToggleFullscreen(window, windowState);
            RefreshInputOwnership(window);
            return;
        }
    }
    if (route == KeyRoute::RmlUi) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && app.hud.paused)
            SetGamePaused(window, false);
        else if (app.hud.codexOpen && playerController && action == GLFW_PRESS &&
                 (key == playerController->key(Luminumbra::Client::InputAction::ToggleCodex) ||
                  key == GLFW_KEY_ESCAPE)) {
            // Closing a gameplay screen belongs to that UI, without releasing gameplay actions.
            app.hud.codexOpen = false;
            app.hud.photoModeUiShown = false;
            if (uiManager)
                uiManager->RequestLoadDocument("hud.rml");
        } else if (uiManager)
            uiManager->KeyCallback(window, key, scancode, action, mods);
        RefreshInputOwnership(window);
        return;
    }
    if (route == KeyRoute::Ignore)
        return;
    // Rebind capture: while waiting for a key for some action, the next key press becomes
    // its binding (Escape cancels). Intercept first so any key — even F-keys — can be bound.
    if (app.hud.rebindCaptureAction >= 0) {
        const auto captured = CaptureGameplayBinding(app.hud.rebindCaptureAction, key, action);
        if (captured &&
            captured->actionIndex < static_cast<int>(Luminumbra::Client::kInputActionCount)) {
            const char* name = Luminumbra::Client::kInputActionDefs[captured->actionIndex].name;
            bool conflict = false;
            if (console) {
                const auto actionDef =
                    Luminumbra::Client::kInputActionDefs[captured->actionIndex].action;
                const bool photoOnly =
                    actionDef >= Luminumbra::Client::InputAction::Shutter &&
                    actionDef <= Luminumbra::Client::InputAction::PhotoWeatherCycle;
                const unsigned scopes = photoOnly ? Debug::Photo : Debug::Gameplay | Debug::Photo;
                for (const auto& def : console->Bindings().List()) {
                    const auto chord = console->Bindings().Get(def.id);
                    if (chord && chord->key == captured->key && (def.scopes & scopes)) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (conflict)
                LUMINUMBRA_CORE_WARN("Gameplay binding conflicts with a developer action");
            else {
                systemConfig.user().keybinds[name] = captured->key;
                if (playerController)
                    playerController->ApplyKeyBindings(systemConfig);
                if (console)
                    console->RefreshAvailability();
                if (uiManager)
                    uiManager->RefreshDeveloperSettings();
            }
        }
        RefreshInputOwnership(window);
        return;
    }
    // Developer Escape was routed above; gameplay Escape toggles the pause overlay.
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && playerController) {
        SetGamePaused(window, !app.hud.paused);
        return;
    }
    if (playerController)
        playerController->ProcessKeyInput(key, action);
}

void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    CloseRegionPanelInput(window);
    gameStateManager.SetState(newState);
    bindings.app->overlay.region_panel_available = newState == GameState::IN_GAME;
    bindings.in_world = newState == GameState::IN_GAME;
    if (bindings.developerConsole)
        bindings.developerConsole->RefreshAvailability();
    RefreshInputOwnership(window);
    if (!bindings.router) {
        const bool in_world = bindings.in_world;
        glfwSetInputMode(window, GLFW_CURSOR, in_world ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        const auto* ui = bindings.uiManager->get();
        glfwSetCursorPosCallback(window,
                                 in_world ? mouse_callback
                                 : ui     ? Rml_UIManager::CursorPosCallback
                                          : nullptr);
        glfwSetScrollCallback(window,
                              in_world ? scroll_callback
                              : ui     ? menu_scroll_callback
                                       : nullptr);
        glfwSetMouseButtonCallback(window,
                                   !in_world && ui ? Rml_UIManager::MouseButtonCallback : nullptr);
        bindings.app->input.firstMouse = true;
    }
}

void SetGamePaused(GLFWwindow* window, bool paused) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    app.hud.paused = paused;
    app.hud.photoModeUiShown = false;
    if (uiManager)
        uiManager->RequestLoadDocument(paused ? "pause.rml" : "hud.rml");
    RefreshInputOwnership(window);
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
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);
    RefreshInputOwnership(window);
    if (bindings.router && bindings.router->Owner() == InputOwner::RmlUi) {
        Luminumbra::Client::Rml_UIManager::CursorPosCallback(window, xpos, ypos);
        return;
    }
    if (bindings.router && !bindings.router->GameplayMouseAllowed())
        return;
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
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendPlatformUserData)
        ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);
    RefreshInputOwnership(window);
    if (bindings.router && bindings.router->Owner() == InputOwner::RmlUi) {
        if (bindings.in_world)
            Luminumbra::Client::Rml_UIManager::ScrollCallback(window, xoffset, yoffset);
        else
            menu_scroll_callback(window, xoffset, yoffset);
        return;
    }
    if (bindings.router && !bindings.router->GameplayMouseAllowed())
        return;
    if (uiManager && uiManager->GetContext() && uiManager->GetContext()->GetHoverElement())
        return;
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
