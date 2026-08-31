#include "PlayerController.h"
#include "core/Log.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "rendering/Camera.h"

namespace Luminumbra::Client {

PlayerController::PlayerController(GLFWwindow* window,
                                   Rendering::Camera* camera,
                                   Systems::PhysicsSystem* physicsSystem)
    : m_window(window)
    , m_camera(camera)
    , m_physicsSystem(physicsSystem) {
    // Seed key bindings from the compiled defaults; ApplyKeyBindings later overlays config.
    for (const auto& def : kInputActionDefs) {
        m_keys[static_cast<std::size_t>(def.action)] = def.default_key;
    }
    if (m_mode == MovementMode::Noclip) {
        m_position = m_camera->Position;
    } else {
        const float standingEyeHeight = m_standingHeight * 0.95f;
        m_position = m_camera->Position - glm::vec3(0.0f, standingEyeHeight, 0.0f);
        if (m_physicsSystem) {
            m_physicsSystem->create_player_controller(m_position);
            m_hasInitializedPhysicsPlayer = true;
        }
    }
}

void PlayerController::ApplyKeyBindings(const luminumbra::core::SystemConfig& cfg) {
    for (const auto& def : kInputActionDefs) {
        m_keys[static_cast<std::size_t>(def.action)] = cfg.keybind(def.name, def.default_key);
    }
}

void PlayerController::RenderDebugUI() {
    // Set the initial position and size of the overlay.
    // ImGuiCond_FirstUseEver ensures this only applies the very first time.
    const float DISTANCE = 10.0f;
    ImVec2 window_pos = ImVec2(DISTANCE, DISTANCE);
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.65f); // Make the window semi-transparent

    // Define window flags for a simple, non-interactive overlay
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("Player State", nullptr, window_flags)) {
        // Display Movement Mode
        const char* modeStr = (m_mode == MovementMode::Walking) ? "Walking" : "Noclip";
        ImGui::Text("Mode: %s (V to toggle)", modeStr);

        ImGui::Separator();

        // Display Position Info
        ImGui::Text("Controller Pos: %.2f, %.2f, %.2f", m_position.x, m_position.y, m_position.z);
        ImGui::Text("Camera Pos:     %.2f, %.2f, %.2f",
                    m_camera->Position.x,
                    m_camera->Position.y,
                    m_camera->Position.z);

        ImGui::Separator();

        // Display Action States
        bool is_sprinting = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        ImGui::Text("Sprinting: %s", is_sprinting && !m_isCrouching ? "Yes" : "No");

        // Display mode-specific info
        if (m_mode == MovementMode::Walking) {
            ImGui::Text("Crouching: %s", m_isCrouching ? "Yes" : "No");
            // For this to work, you may need to add a simple `is_player_on_ground`
            // function to your PhysicsSystem.
            if (m_physicsSystem) {
                ImGui::Text("On Ground: %s", m_physicsSystem->is_player_grounded() ? "Yes" : "No");
            }
        } else { // Noclip
            float current_speed = m_noclipBaseSpeed * m_noclipSpeedMultiplier;
            ImGui::Text("Noclip Speed: %.2fx (Scroll Wheel)", m_noclipSpeedMultiplier);
        }
    }
    ImGui::End();
}

void PlayerController::Update(float deltaTime) {
    ApplyReplayInput(deltaTime, ReadLiveInputFrame());
    m_wantsToJump = false;
    m_wantsToCrouch = false;
}

PlayerReplayInputFrame PlayerController::ReadLiveInputFrame() const {
    PlayerReplayInputFrame input{};

    glm::vec3 wishDir(0.0f);
    glm::vec3 forward(m_camera->Front.x, 0.0f, m_camera->Front.z);
    if (glm::length(forward) > 0.0f) {
        forward = glm::normalize(forward);
    }

    glm::vec3 right = m_camera->Right;

    if (glfwGetKey(m_window, key(InputAction::MoveForward)) == GLFW_PRESS)
        wishDir += forward;
    if (glfwGetKey(m_window, key(InputAction::MoveBack)) == GLFW_PRESS)
        wishDir -= forward;
    if (glfwGetKey(m_window, key(InputAction::MoveLeft)) == GLFW_PRESS)
        wishDir -= right;
    if (glfwGetKey(m_window, key(InputAction::MoveRight)) == GLFW_PRESS)
        wishDir += right;

    if (m_mode == MovementMode::Noclip) {
        if (glfwGetKey(m_window, key(InputAction::NoclipUp)) == GLFW_PRESS) {
            wishDir += m_camera->WorldUp;
        }
        if (glfwGetKey(m_window, key(InputAction::NoclipDown)) == GLFW_PRESS) {
            wishDir -= m_camera->WorldUp;
        }
    }

    input.wishDirection = wishDir;
    input.jumpPressed = m_wantsToJump;
    input.crouchPressed = m_wantsToCrouch;
    input.sprintHeld = glfwGetKey(m_window, key(InputAction::Sprint)) == GLFW_PRESS;
    return input;
}

void PlayerController::ApplyReplayInput(float deltaTime, const PlayerReplayInputFrame& inputFrame) {
    glm::vec3 wishDir = inputFrame.wishDirection;
    if (glm::length(wishDir) > 0.0f) {
        wishDir = glm::normalize(wishDir);
    }

    if (m_mode == MovementMode::Walking) {
        UpdateWalking(deltaTime,
                      wishDir,
                      inputFrame.jumpPressed,
                      inputFrame.crouchPressed,
                      inputFrame.sprintHeld);
    } else { // Noclip
        UpdateNoclip(deltaTime, wishDir, inputFrame.sprintHeld);
    }

    UpdateCameraFromControllerPosition();
    ++m_replayFrameCounter;
}

PlayerReplaySnapshot PlayerController::CaptureReplaySnapshot() const {
    PlayerReplaySnapshot snapshot{};
    snapshot.frame = m_replayFrameCounter;
    snapshot.mode = m_mode;
    snapshot.position = m_position;
    snapshot.velocity = m_velocity;
    snapshot.isCrouching = m_isCrouching;
    snapshot.wantsToJump = m_wantsToJump;
    snapshot.wantsToCrouch = m_wantsToCrouch;
    snapshot.hasInitializedPhysicsPlayer = m_hasInitializedPhysicsPlayer;
    snapshot.noclipSpeedMultiplier = m_noclipSpeedMultiplier;
    return snapshot;
}

void PlayerController::ResetReplayFrameCounter(std::uint64_t frame) {
    m_replayFrameCounter = frame;
}

void PlayerController::UpdateCameraFromControllerPosition() {
    if (m_mode == MovementMode::Walking) {
        const float standingEyeHeight = m_standingHeight * 0.95f;
        const float crouchingEyeHeight = m_crouchHeight * 0.9f;
        const float currentEyeHeight = m_isCrouching ? crouchingEyeHeight : standingEyeHeight;
        m_camera->Position = m_position + glm::vec3(0.0f, currentEyeHeight, 0.0f);
    } else {
        m_camera->Position = m_position;
    }
}

void PlayerController::ProcessKeyInput(int key, int action) {
    // --- Global Controls (work in any mode) ---
    if (key == this->key(InputAction::ToggleNoclip) && action == GLFW_PRESS) {
        if (m_mode == MovementMode::Walking) {
            m_mode = MovementMode::Noclip;
            // When entering noclip, the camera now leads.
            m_position = m_camera->Position;
            LUMINUMBRA_CORE_INFO("Noclip Enabled. Use Scroll Wheel to change speed.");
        } else { // m_mode == MovementMode::Noclip
            m_mode = MovementMode::Walking;
            // When exiting noclip, sync the physics body to the camera's position.
            const float standingEyeHeight = m_standingHeight * 0.95f;
            m_position = m_camera->Position - glm::vec3(0.0f, standingEyeHeight, 0.0f);

            if (m_physicsSystem) {
                // If this is the first time entering walking mode, create the physics body.
                if (!m_hasInitializedPhysicsPlayer) {
                    LUMINUMBRA_CORE_INFO("Creating physics player controller for the first time.");
                    m_physicsSystem->create_player_controller(m_position);
                    m_hasInitializedPhysicsPlayer = true;
                } else {
                    // Otherwise, just teleport the existing body.
                    m_physicsSystem->set_player_position(m_position);
                }
            }
            LUMINUMBRA_CORE_INFO("Noclip Disabled. Switched to Walking mode.");
        }
    }

    // --- photography photo mode (feature) — read-only w.r.t. sim ---
    // Toggle photo mode on/off; while active the shutter + lens nudges are armed.
    // None of these touch the physics body, the registry, or a tick — they only set
    // client-only flags the main loop reads after render.
    if (key == this->key(InputAction::TogglePhotoMode) && action == GLFW_PRESS) {
        m_photoModeActive = !m_photoModeActive;
        LUMINUMBRA_CORE_INFO(m_photoModeActive
                                 ? "Photo mode ON. [/] aperture, -/= focus, Enter to shutter."
                                 : "Photo mode OFF.");
    }
    // Codex screen toggle (client-only overlay): edge-triggered, consumed by the main loop.
    if (key == this->key(InputAction::ToggleCodex) && action == GLFW_PRESS) {
        m_codexToggleRequested = true;
    }
    if (m_photoModeActive) {
        if (key == this->key(InputAction::Shutter) && action == GLFW_PRESS) {
            m_shutterRequested = true;
        }
        // Lens nudges fire on press AND repeat so holding ramps the value.
        const bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);
        if (pressed && key == this->key(InputAction::LensApertureUp))
            m_apertureNudge += 0.3f;
        if (pressed && key == this->key(InputAction::LensApertureDown))
            m_apertureNudge -= 0.3f;
        if (pressed && key == this->key(InputAction::LensFocusUp))
            m_focusNudge += 0.25f;
        if (pressed && key == this->key(InputAction::LensFocusDown))
            m_focusNudge -= 0.25f;
        // Manual exposure: shutter speed + ISO in 1/3-stop steps.
        if (pressed && key == this->key(InputAction::LensShutterUp))
            m_shutterSpeedNudge += 0.333f;
        if (pressed && key == this->key(InputAction::LensShutterDown))
            m_shutterSpeedNudge -= 0.333f;
        if (pressed && key == this->key(InputAction::LensIsoUp))
            m_isoNudge += 0.333f;
        if (pressed && key == this->key(InputAction::LensIsoDown))
            m_isoNudge -= 0.333f;
        // photo-mode environment scrub. TOD ramps on press+repeat (smooth scrub);
        // weather cycles once per discrete PRESS (not repeat).
        if (pressed && key == this->key(InputAction::PhotoTodBack))
            m_todNudge -= 0.02f;
        if (pressed && key == this->key(InputAction::PhotoTodForward))
            m_todNudge += 0.02f;
        if (action == GLFW_PRESS && key == this->key(InputAction::PhotoWeatherCycle))
            ++m_weatherCycle;
    }

    // --- Mode-Specific Controls ---
    if (m_mode == MovementMode::Walking) {
        // These actions are events; they set a flag that is consumed in the Update loop.
        if (key == this->key(InputAction::Jump) && action == GLFW_PRESS) {
            m_wantsToJump = true;
        }
        if (key == this->key(InputAction::Crouch) && action == GLFW_PRESS) {
            m_wantsToCrouch = true;
        }
    }
}

// NEW: Add a method to process scroll wheel input for noclip speed
void PlayerController::ProcessMouseScroll(double yoffset) {
    if (m_mode == MovementMode::Noclip) {
        // Increase or decrease the speed multiplier
        m_noclipSpeedMultiplier += yoffset * 0.25f; // Adjust sensitivity as needed
        // Clamp the multiplier to a reasonable range to prevent it from becoming zero or
        // excessively fast
        m_noclipSpeedMultiplier = glm::clamp(m_noclipSpeedMultiplier, 0.1f, 20.0f);
        LUMINUMBRA_CORE_INFO("Noclip speed multiplier: {:.2f}x", m_noclipSpeedMultiplier);
    }
}

void PlayerController::UpdateWalking(float deltaTime,
                                     const glm::vec3& wishDir,
                                     bool jumpPressed,
                                     bool crouchPressed,
                                     bool sprintHeld) {
    if (!m_physicsSystem) {
        m_velocity = glm::vec3(0.0f);
        return;
    }

    // Crouching is a toggle. Check if the player wants to change state.
    if (crouchPressed) {
        if (m_isCrouching) { // If currently crouching, try to stand up
            if (m_physicsSystem->player_has_space_to_stand()) {
                m_isCrouching = false;
                m_physicsSystem->set_player_crouched(false);
            }
            // If there's no space, do nothing and remain crouched.
        } else { // If standing, crouch down
            m_isCrouching = true;
            m_physicsSystem->set_player_crouched(true);
        }
    }

    // Sprinting is only possible when not crouched
    bool is_sprinting = sprintHeld && !m_isCrouching;
    float current_speed =
        m_isCrouching ? m_crouchSpeed : (is_sprinting ? m_sprintSpeed : m_walkSpeed);

    // Let the physics system handle the actual movement
    const glm::vec3 previousPosition = m_position;
    m_physicsSystem->update_player(wishDir * current_speed, jumpPressed, m_jumpForce, deltaTime);

    // Update our controller's position from the physics simulation
    m_position = m_physicsSystem->get_player_position();
    m_velocity = (deltaTime > 0.0f) ? (m_position - previousPosition) / deltaTime : glm::vec3(0.0f);
}

void PlayerController::UpdateNoclip(float deltaTime, const glm::vec3& wishDir, bool sprintHeld) {
    // Calculate speed based on the base speed, the scroll-wheel multiplier, and a sprint key boost
    float current_speed = m_noclipBaseSpeed * m_noclipSpeedMultiplier;
    if (sprintHeld) {
        current_speed *= 2.0f; // Sprinting doubles the current noclip speed
    }

    // Noclip directly modifies the position, bypassing physics
    m_velocity = wishDir * current_speed;
    m_position += m_velocity * deltaTime;
}

} // namespace Luminumbra::Client
