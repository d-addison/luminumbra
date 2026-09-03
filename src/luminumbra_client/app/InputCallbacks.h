#pragma once

// The GLFW input callbacks and game-state/cursor transitions extracted
// verbatim from main_client.cpp: key/mouse/scroll/menu-scroll and
// framebuffer-size callbacks, SetGameState/SetGamePaused, plus the GLFW error
// and GL debug-message callbacks. The callbacks reach their state through the
// window user pointer, which main() points at an InputCallbackBindings once
// at window setup: the app context plus the core singletons that stayed
// file-scope globals in main_client.cpp (held as pointers here, not externs).

#include "app/ClientAppContext.h"
#include "app/WindowModeControls.h"
#include "core/GameState.h"

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
    luminumbra::core::SystemConfig* systemConfig = nullptr;
    WindowState* windowState = nullptr;
    std::unique_ptr<Luminumbra::Rendering::Camera>* camera = nullptr;
    std::unique_ptr<Luminumbra::Client::PlayerController>* playerController = nullptr;
    std::unique_ptr<Luminumbra::Client::Rml_UIManager>* uiManager = nullptr;
};

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
