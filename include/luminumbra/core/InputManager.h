// src/core/InputManager.h
#pragma once

#include "InputActions.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <string>
#include <unordered_map>

class InputManager {
public:
    InputManager(GLFWwindow* window);

    void update(); // Updates key states each frame

    // Load keybinds from a file (or set defaults)
    void loadKeybinds(const std::string& filePath = "");

    // Check action states
    bool isActionPressed(GameAction action) const;  // True for one frame on press
    bool isActionReleased(GameAction action) const; // True for one frame on release
    bool isActionHeld(GameAction action) const;     // True while the key is down

private:
    GLFWwindow* m_Window;
    std::unordered_map<GameAction, int> m_KeyMappings;
    std::unordered_map<int, bool> m_CurrentKeyStates;
    std::unordered_map<int, bool> m_PreviousKeyStates;
};