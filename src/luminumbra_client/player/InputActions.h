#pragma once

// Rebindable input action registry. Gameplay reads logical InputActions, not
// raw GLFW keys. Bindings resolve from SystemConfig user.controls.*
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
    // photography photo-mode capture loop (feature). Read-only w.r.t. sim:
    // these drive a client-only PhotoModeState + capture/persist, never a tick.
    TogglePhotoMode,
    Shutter,
    LensApertureUp,    // stop DOWN (larger f-number, deeper DoF)
    LensApertureDown,  // open UP   (smaller f-number, shallower DoF / more bokeh)
    LensFocusUp,       // push focus distance farther
    LensFocusDown,     // pull focus distance nearer
    LensShutterUp,     // FASTER shutter (less light / higher EV) —  manual exposure
    LensShutterDown,   // SLOWER shutter (more light / lower EV)
    LensIsoUp,         // raise ISO (more sensitivity / lower required EV)
    LensIsoDown,       // lower ISO (less sensitivity / higher required EV)
    PhotoTodBack,      // photo mode: scrub time-of-day BACKWARD (toward dawn) —
    PhotoTodForward,   // photo mode: scrub time-of-day FORWARD (toward dusk/night)
    PhotoWeatherCycle, // photo mode: cycle weather/atmosphere preset (clear/fog/rain/snow/storm)
    ToggleCodex,       // open/close the creature codex browse screen (client-only)
    //   farming verbs: act on the plant nearest the player's aim. The sim verbs
    // are deterministic (FarmingSystem.h); only fire on a key press, so headless gates are
    // unaffected.
    FarmPlant,     // plant a seed at the aim point
    FarmWater,     // water the nearest plant (boost growth)
    FarmFertilize, // fertilize the nearest plant (stronger boost + stress recovery)
    FarmHarvest,   // harvest the nearest mature plant
    Count
};

inline constexpr std::size_t kInputActionCount = static_cast<std::size_t>(InputAction::Count);

struct InputActionDef {
    InputAction action;
    const char* name; // canonical key in user.controls.* (stable; do not rename casually)
    int default_key;  // GLFW key code used when the overlay does not bind this action
};

// The default bindings (today's hardcoded layout). The settings UI + SystemConfig overlay
// override these per-action by name.
inline constexpr std::array<InputActionDef, kInputActionCount> kInputActionDefs = {{
    {InputAction::MoveForward, "MoveForward", GLFW_KEY_W},
    {InputAction::MoveBack, "MoveBack", GLFW_KEY_S},
    {InputAction::MoveLeft, "MoveLeft", GLFW_KEY_A},
    {InputAction::MoveRight, "MoveRight", GLFW_KEY_D},
    {InputAction::Jump, "Jump", GLFW_KEY_SPACE},
    {InputAction::Crouch, "Crouch", GLFW_KEY_LEFT_CONTROL},
    {InputAction::Sprint, "Sprint", GLFW_KEY_LEFT_SHIFT},
    {InputAction::NoclipUp, "NoclipUp", GLFW_KEY_SPACE},
    {InputAction::NoclipDown, "NoclipDown", GLFW_KEY_LEFT_CONTROL},
    {InputAction::ToggleNoclip, "ToggleNoclip", GLFW_KEY_V},
    {InputAction::TogglePhotoMode, "TogglePhotoMode", GLFW_KEY_P},
    {InputAction::Shutter, "Shutter", GLFW_KEY_ENTER},
    {InputAction::LensApertureUp, "LensApertureUp", GLFW_KEY_RIGHT_BRACKET},
    {InputAction::LensApertureDown, "LensApertureDown", GLFW_KEY_LEFT_BRACKET},
    {InputAction::LensFocusUp, "LensFocusUp", GLFW_KEY_EQUAL},
    {InputAction::LensFocusDown, "LensFocusDown", GLFW_KEY_MINUS},
    {InputAction::LensShutterUp, "LensShutterUp", GLFW_KEY_PERIOD},
    {InputAction::LensShutterDown, "LensShutterDown", GLFW_KEY_COMMA},
    {InputAction::LensIsoUp, "LensIsoUp", GLFW_KEY_APOSTROPHE},
    {InputAction::LensIsoDown, "LensIsoDown", GLFW_KEY_SEMICOLON},
    {InputAction::PhotoTodBack, "PhotoTodBack", GLFW_KEY_K},
    {InputAction::PhotoTodForward, "PhotoTodForward", GLFW_KEY_L},
    {InputAction::PhotoWeatherCycle, "PhotoWeatherCycle", GLFW_KEY_T},
    {InputAction::ToggleCodex, "ToggleCodex", GLFW_KEY_C},
    {InputAction::FarmPlant, "FarmPlant", GLFW_KEY_F},
    {InputAction::FarmWater, "FarmWater", GLFW_KEY_G},
    {InputAction::FarmFertilize, "FarmFertilize", GLFW_KEY_H},
    {InputAction::FarmHarvest, "FarmHarvest", GLFW_KEY_J},
}};

} // namespace Luminumbra::Client
