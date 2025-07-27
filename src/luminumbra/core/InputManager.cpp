// src/core/InputManager.cpp
#include "luminumbra/core/InputManager.h"
#include <iostream>

InputManager::InputManager(GLFWwindow* window) : m_Window(window) {
    loadKeybinds(); // Load default keybinds
}

void InputManager::loadKeybinds(const std::string& filePath) {
    // In a real-world scenario, you'd load this from a JSON or INI file.
    // For now, we'll hardcode the defaults.
    m_KeyMappings = {
        { GameAction::MoveForward,    GLFW_KEY_W },
        { GameAction::MoveBackward,   GLFW_KEY_S },
        { GameAction::MoveLeft,       GLFW_KEY_A },
        { GameAction::MoveRight,      GLFW_KEY_D },
        { GameAction::MoveUp,         GLFW_KEY_SPACE },
        { GameAction::MoveDown,       GLFW_KEY_LEFT_CONTROL },
        { GameAction::Jump,           GLFW_KEY_SPACE },
        { GameAction::Sprint,         GLFW_KEY_LEFT_SHIFT },
        { GameAction::Crouch,         GLFW_KEY_LEFT_CONTROL },
        { GameAction::ToggleGlide,    GLFW_KEY_G },
        { GameAction::Pause,          GLFW_KEY_ESCAPE },
        { GameAction::ToggleNoclip,   GLFW_KEY_V },
        { GameAction::ShowDebug,      GLFW_KEY_F3 },
        { GameAction::ToggleWireframe,GLFW_KEY_F4 },
        { GameAction::ToggleWeather,  GLFW_KEY_F6 },
        { GameAction::StartFire,      GLFW_KEY_F7 }
    };
    std::cout << "InputManager: Loaded default keybinds." << std::endl;
}

void InputManager::update() {
    m_PreviousKeyStates = m_CurrentKeyStates;
    m_CurrentKeyStates.clear();

    for (const auto& pair : m_KeyMappings) {
        int key = pair.second;
        if (glfwGetKey(m_Window, key) == GLFW_PRESS) {
            m_CurrentKeyStates[key] = true;
        }
    }
}

bool InputManager::isActionPressed(GameAction action) const {
    auto it = m_KeyMappings.find(action);
    if (it == m_KeyMappings.end()) return false;
    
    int key = it->second;
    bool isCurrentlyPressed = m_CurrentKeyStates.count(key) > 0;
    bool wasPreviouslyPressed = m_PreviousKeyStates.count(key) > 0;

    return isCurrentlyPressed && !wasPreviouslyPressed;
}

bool InputManager::isActionReleased(GameAction action) const {
    auto it = m_KeyMappings.find(action);
    if (it == m_KeyMappings.end()) return false;

    int key = it->second;
    bool isCurrentlyPressed = m_CurrentKeyStates.count(key) > 0;
    bool wasPreviouslyPressed = m_PreviousKeyStates.count(key) > 0;

    return !isCurrentlyPressed && wasPreviouslyPressed;
}

bool InputManager::isActionHeld(GameAction action) const {
    auto it = m_KeyMappings.find(action);
    if (it == m_KeyMappings.end()) return false;

    int key = it->second;
    return m_CurrentKeyStates.count(key) > 0;
}