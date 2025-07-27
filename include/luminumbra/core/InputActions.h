// src/core/InputActions.h
#pragma once

enum class GameAction {
    // Movement
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,

    // Actions
    Jump,
    Sprint,
    Crouch,
    ToggleGlide,
    
    // UI / System
    Pause,
    
    // Debug
    ToggleNoclip,
    ShowDebug,
    ToggleWireframe
};