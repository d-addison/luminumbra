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

} // namespace

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
    ownership.interactive_panel = ui_enabled && b.developerConsole &&
                                  b.developerConsole->Widgets().HasInteractivePanel("ui.debugger");
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
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext())
        ImGui_ImplGlfw_CharCallback(window, codepoint);
    if (owner == InputOwner::RmlUi)
        Luminumbra::Client::Rml_UIManager::CharCallback(window, codepoint);
}
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext())
        ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    RefreshInputOwnership(window);
    if (b.router && (b.router->Owner() == InputOwner::RmlUi || action == GLFW_RELEASE))
        Luminumbra::Client::Rml_UIManager::MouseButtonCallback(window, button, action, mods);
    RefreshInputOwnership(window);
}
void window_focus_callback(GLFWwindow* window, int focused) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext())
        ImGui_ImplGlfw_WindowFocusCallback(window, focused);
    b.window_focused = focused != 0;
    RefreshInputOwnership(window);
}
void cursor_enter_callback(GLFWwindow* window, int entered) {
    auto& b = CallbackBindings(window);
    if (b.app->overlay.imgui_enabled && ImGui::GetCurrentContext())
        ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    InputCallbackBindings& bindings = CallbackBindings(window);
    ClientAppContext& app = *bindings.app;
    luminumbra::core::SystemConfig& systemConfig = *bindings.systemConfig;
    WindowState& windowState = *bindings.windowState;
    Luminumbra::Client::PlayerController* playerController = bindings.playerController->get();
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext())
        ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    RefreshInputOwnership(window);
    auto* console = DeveloperUiAllowed(app) ? bindings.developerConsole : nullptr;
    const bool photo = playerController && playerController->photo_mode_active();
    const unsigned scope = photo ? Debug::Photo : Debug::Gameplay;
    const auto shortcut =
        console ? console->Bindings().Match({key, mods & 15}, action, scope) : std::nullopt;
    const bool consoleToggle =
        console && console->Bindings().Get("console.toggle") == Debug::KeyChord{key, mods & 15};
    const auto route = bindings.router
                           ? bindings.router->Key(key, action, consoleToggle, shortcut.has_value())
                           : KeyRoute::Gameplay;
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
    if (route == KeyRoute::DeveloperShortcut && shortcut && console) {
        if (UiInputRouter::TimeShortcutAllowed(photo, key))
            console->InvokeAction(*shortcut);
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
        if (action != GLFW_PRESS || Debug::IsModifierKey(key))
            return;
        if (key != GLFW_KEY_ESCAPE &&
            app.hud.rebindCaptureAction < static_cast<int>(Luminumbra::Client::kInputActionCount)) {
            const char* name =
                Luminumbra::Client::kInputActionDefs[app.hud.rebindCaptureAction].name;
            bool conflict = false;
            if (console) {
                const auto actionDef =
                    Luminumbra::Client::kInputActionDefs[app.hud.rebindCaptureAction].action;
                const bool photoOnly =
                    actionDef >= Luminumbra::Client::InputAction::Shutter &&
                    actionDef <= Luminumbra::Client::InputAction::PhotoWeatherCycle;
                const unsigned scopes = photoOnly ? Debug::Photo : Debug::Gameplay | Debug::Photo;
                for (const auto& def : console->Bindings().List()) {
                    const auto chord = console->Bindings().Get(def.id);
                    if (chord && chord->key == key && (def.scopes & scopes)) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (conflict)
                LUMINUMBRA_CORE_WARN("Gameplay binding conflicts with a developer action");
            else {
                systemConfig.user().keybinds[name] = key;
                if (playerController)
                    playerController->ApplyKeyBindings(systemConfig);
                if (console)
                    console->RefreshAvailability();
                if (uiManager)
                    uiManager->RefreshDeveloperSettings();
            }
        }
        app.hud.rebindCaptureAction = -1;
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
    gameStateManager.SetState(newState);
    bindings.in_world = newState == GameState::IN_GAME;
    if (bindings.developerConsole)
        bindings.developerConsole->RefreshAvailability();
    RefreshInputOwnership(window);
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
    Luminumbra::Rendering::Camera* camera = bindings.camera->get();
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext())
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
    Luminumbra::Rendering::Camera* camera = bindings.camera->get();
    Luminumbra::Client::PlayerController* playerController = bindings.playerController->get();
    Luminumbra::Client::Rml_UIManager* uiManager = bindings.uiManager->get();
    if (app.overlay.imgui_enabled && ImGui::GetCurrentContext())
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
