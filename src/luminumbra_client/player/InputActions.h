#pragma once

// Rebindable input action registry (task #11). Gameplay reads logical InputActions, not
// raw GLFW keys (docs/STANDARDS.md §6). Bindings resolve from SystemConfig user.controls.*
// (action name -> GLFW key code), so controls are rebindable + persisted with user settings.

#include <array>
#include <cstddef>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace Luminumbra::Client {

enum class InputAction : int {
    MoveForward = 0,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Crouch,
    Sprint,
    NoclipUp,
    NoclipDown,
    ToggleNoclip,
    Count
};

inline constexpr std::size_t kInputActionCount = static_cast<std::size_t>(InputAction::Count);

struct InputActionDef {
    InputAction action;
    const char* name;   // canonical key in user.controls.* (stable; do not rename casually)
    int default_key;    // GLFW key code used when the overlay does not bind this action
};

// The default bindings (today's hardcoded layout). The settings UI + SystemConfig overlay
// override these per-action by name.
inline constexpr std::array<InputActionDef, kInputActionCount> kInputActionDefs = {{
    {InputAction::MoveForward,  "MoveForward",  GLFW_KEY_W},
    {InputAction::MoveBack,     "MoveBack",     GLFW_KEY_S},
    {InputAction::MoveLeft,     "MoveLeft",     GLFW_KEY_A},
    {InputAction::MoveRight,    "MoveRight",    GLFW_KEY_D},
    {InputAction::Jump,         "Jump",         GLFW_KEY_SPACE},
    {InputAction::Crouch,       "Crouch",       GLFW_KEY_LEFT_CONTROL},
    {InputAction::Sprint,       "Sprint",       GLFW_KEY_LEFT_SHIFT},
    {InputAction::NoclipUp,     "NoclipUp",     GLFW_KEY_SPACE},
    {InputAction::NoclipDown,   "NoclipDown",   GLFW_KEY_LEFT_CONTROL},
    {InputAction::ToggleNoclip, "ToggleNoclip", GLFW_KEY_V},
}};

}  // namespace Luminumbra::Client
