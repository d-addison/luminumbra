#pragma once

// Application-owned GLFW callbacks feed UI backends once, then route through
// explicit ownership. The bindings hold addresses of current-service owners so
// callbacks remain valid across world transitions.

#include "app/ClientAppContext.h"
#include "app/UiInputRouter.h"
#include "app/WindowModeControls.h"
#include "core/GameState.h"
#include "debug/DeveloperConsole.h"

#include <memory>

struct GLFWwindow;

namespace Luminumbra::Client {
class PlayerController;
class Rml_UIManager;
} // namespace Luminumbra::Client
namespace Luminumbra::Rendering {
class Camera;
}
namespace luminumbra::core {
class SystemConfig;
}

namespace Luminumbra::Client::App {

// Everything the GLFW callbacks touch. The unique_ptr owners are addressed
// indirectly because main_client.cpp re-seats them across world transitions
// (menu backdrop -> real world); the callbacks always see the live object.
struct InputCallbackBindings {
    ClientAppContext* app = nullptr;
    UiInputRouter* router = nullptr;
    Debug::DeveloperConsole* developerConsole = nullptr;
    bool in_world = false;
    bool window_focused = true;
    luminumbra::core::SystemConfig* systemConfig = nullptr;
    WindowState* windowState = nullptr;
    std::unique_ptr<Luminumbra::Rendering::Camera>* camera = nullptr;
    std::unique_ptr<Luminumbra::Client::PlayerController>* playerController = nullptr;
    std::unique_ptr<Luminumbra::Client::Rml_UIManager>* uiManager = nullptr;
};

void InstallInputCallbacks(GLFWwindow* window);
void RefreshInputOwnership(GLFWwindow* window);
void char_callback(GLFWwindow* window, unsigned int codepoint);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
void window_focus_callback(GLFWwindow* window, int focused);
void cursor_enter_callback(GLFWwindow* window, int entered);
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
// Menu-state scroll callback — forwards to RmlUi (so menu lists still scroll)
// AND accrues the wheel delta so the create-world preview block can zoom the
// diorama when the cursor is over the pane.
void menu_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void GLFWErrorCallback(int error, const char* description);
void SetGameState(GLFWwindow* window, GameStateManager& gameStateManager, GameState newState);
void SetGamePaused(GLFWwindow* window, bool paused);

} // namespace Luminumbra::Client::App
