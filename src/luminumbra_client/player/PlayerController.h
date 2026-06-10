#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include "imgui.h"

namespace Luminumbra::Rendering {
    class Camera;
}

namespace Luminumbra::Systems {
    class PhysicsSystem;
}

namespace Luminumbra::Client {

enum class MovementMode {
    Walking,
    Noclip
};

class PlayerController {
public:
    PlayerController(GLFWwindow* window, Rendering::Camera* camera, Systems::PhysicsSystem* physicsSystem);

    void Update(float deltaTime);
    void ProcessKeyInput(int key, int action);

    MovementMode GetMovementMode() const { return m_mode; }
    glm::vec3 GetPosition() const { return m_position; }
    void ProcessMouseScroll(double yoffset);
    void RenderDebugUI();

private:
    void UpdateWalking(float deltaTime, const glm::vec3& wishDir);
    void UpdateNoclip(float deltaTime, const glm::vec3& wishDir);

    GLFWwindow* m_window;
    Rendering::Camera* m_camera;
    Systems::PhysicsSystem* m_physicsSystem;

    MovementMode m_mode = MovementMode::Walking;
    glm::vec3 m_position{16.0f, 100.0f, 16.0f};
    glm::vec3 m_velocity{0.0f};

    // Player attributes
    float m_walkSpeed = 8.0f;
    float m_sprintSpeed = 15.0f;
    float m_noclipSpeed = 45.0f;
    float m_jumpForce = 9.0f;
    float m_crouchSpeed = 4.0f;
    float m_crouchHeight = 0.9f;
    float m_standingHeight = 1.8f;
    float m_noclipBaseSpeed = 5.0f;
    float m_noclipSpeedMultiplier = 3.0f;

    bool m_wantsToJump = false;
    bool m_isCrouching = false;
    bool m_wantsToCrouch = false;
    bool m_hasInitializedPhysicsPlayer = false;
};

} // namespace Luminumbra::Client
