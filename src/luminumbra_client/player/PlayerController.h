#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include "imgui.h"
#include "InputActions.h"

namespace Luminumbra::Rendering {
    class Camera;
}

namespace Luminumbra::Systems {
    class PhysicsSystem;
}

namespace luminumbra::core {
    class SystemConfig;
}

namespace Luminumbra::Client {

enum class MovementMode {
    Walking,
    Noclip
};

struct PlayerReplayInputFrame {
    glm::vec3 wishDirection{0.0f};
    bool jumpPressed = false;
    bool crouchPressed = false;
    bool sprintHeld = false;
};

struct PlayerReplaySnapshot {
    std::uint64_t frame = 0;
    MovementMode mode = MovementMode::Walking;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool isCrouching = false;
    bool wantsToJump = false;
    bool wantsToCrouch = false;
    bool hasInitializedPhysicsPlayer = false;
    float noclipSpeedMultiplier = 1.0f;
};

class PlayerController {
public:
    PlayerController(GLFWwindow* window, Rendering::Camera* camera, Systems::PhysicsSystem* physicsSystem);

    void Update(float deltaTime);
    void ApplyReplayInput(float deltaTime, const PlayerReplayInputFrame& inputFrame);
    PlayerReplaySnapshot CaptureReplaySnapshot() const;
    void ResetReplayFrameCounter(std::uint64_t frame = 0);
    void ProcessKeyInput(int key, int action);

    MovementMode GetMovementMode() const { return m_mode; }
    glm::vec3 GetPosition() const { return m_position; }
    void ProcessMouseScroll(double yoffset);
    void RenderDebugUI();

    // Resolve all key bindings from SystemConfig user.controls.* (action name -> key),
    // falling back to the compiled defaults (kInputActionDefs). Call after construction
    // and whenever bindings change.
    void ApplyKeyBindings(const luminumbra::core::SystemConfig& cfg);
    [[nodiscard]] int key(InputAction action) const { return m_keys[static_cast<std::size_t>(action)]; }

private:
    PlayerReplayInputFrame ReadLiveInputFrame() const;
    void UpdateWalking(float deltaTime, const glm::vec3& wishDir, bool jumpPressed, bool crouchPressed, bool sprintHeld);
    void UpdateNoclip(float deltaTime, const glm::vec3& wishDir, bool sprintHeld);
    void UpdateCameraFromControllerPosition();

    GLFWwindow* m_window;
    Rendering::Camera* m_camera;
    Systems::PhysicsSystem* m_physicsSystem;

    // Resolved key code per InputAction (defaults from kInputActionDefs; overridden by config).
    std::array<int, kInputActionCount> m_keys{};

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
    std::uint64_t m_replayFrameCounter = 0;
};

} // namespace Luminumbra::Client
