#pragma once

#include "luminumbra/rendering/Camera.h"
#include "luminumbra/world/World.h"
#include "luminumbra/core/SaveData.h"
#include <glm/glm.hpp>
#include <vector>

namespace Luminumbra::Player {

// Enum for discrete, single-press actions
enum class Action {
    Jump,
    ToggleNoclip,
    ToggleGlide
};

// Enum for continuous movement directions
enum class Movement {
    Forward,
    Backward,
    Left,
    Right,
    Up,
    Down
};

class Player {
public:
    Player(float screenWidth, float screenHeight);

    // --- NEW: Input Processing Methods ---
    void processAction(Action action, const World::World& world);
    void processMovement(const std::vector<Movement>& directions, float deltaTime);
    void setSprinting(bool isSprinting);
    void setCrouching(bool isCrouching, const World::World& world);
    void processMouseMovement(float xoffset, float yoffset);
    void processMouseScroll(float yoffset);
    
    // --- NEW: Main Physics & State Update ---
    void update(float deltaTime, const World::World& world);

    // --- NEW: Reset Method ---
    void reset(const glm::vec3& position);

    // --- Getters (Unchanged) ---
    Rendering::Camera& getCamera() { return m_Camera; }
    const Rendering::Camera& getCamera() const { return m_Camera; }
    const glm::vec3& getPosition() const { return m_Position; }
    const glm::vec3& getVelocity() const { return m_Velocity; }
    float getGravity() const { return m_Gravity; }
    bool isNoClipMode() const { return m_NoclipEnabled; }
    float getStamina() const { return m_Stamina; }
    float getMaxStamina() const { return m_MaxStamina; }
    bool isSprinting() const { return m_IsSprinting; }
    bool isCrouching() const { return m_IsCrouching; }
    bool isGliding() const { return m_IsGliding; }
    bool isClimbing() const { return m_IsClimbing; }
    bool isSliding() const { return m_IsSliding; }
    float getFallDistance() const { return m_FallDistance; }
    void setPosition(const glm::vec3& pos) { 
        m_Position = pos;
        m_Camera.setPosition(glm::vec3(pos.x, pos.y + m_CurrentEyeHeight, pos.z));
    }

    Core::PlayerSaveData serialize() const;
    void applySaveData(const Core::PlayerSaveData& data);

private:
    void resolveCollisions(const World::World& world, float deltaTime);
    void applyMovement(const glm::vec3& wishDir, float deltaTime);
    void applyClimbingMovement(const std::vector<Movement>& directions);
    void updateState(float deltaTime, const World::World& world);
    void updateCrouchState(float deltaTime, const World::World& world);
    void updateStamina(float deltaTime);
    glm::vec3 calculateSurfaceNormal(const World::World& world, const glm::vec3& position) const;
    bool checkForClimbableSurface(const World::World& world);
    void updateSliding(float deltaTime);
    void handleAutoStepUp(const World::World& world);

    // Camera
    Rendering::Camera m_Camera;
    
    // Physics & Position
    glm::vec3 m_PlayerSize;
    glm::vec3 m_Position;
    glm::vec3 m_Velocity;
    float m_Gravity = -9.81f;
    bool m_IsOnGround = false;
    float m_TimeSinceLastGrounded = 0.0f;
    
    // --- State Variables (controlled by public methods) ---
    bool m_WantsToSprint = false;
    bool m_WantsToCrouch = false;
    
    // --- Internal State ---
    bool m_IsSprinting = false;
    bool m_IsCrouching = false;
    bool m_NoclipEnabled = false;
    bool m_IsGliding = false;
    bool m_IsClimbing = false;
    bool m_IsSliding = false;

    // Movement Properties
    float m_BaseMovementSpeed = 5.0f;
    float m_SprintMultiplier = 1.75f;
    float m_JumpForce = 5.0f;
    float m_CoyoteTime = 0.1f;
    float m_GroundAcceleration = 10.0f;
    float m_AirAcceleration = 3.0f;
    float m_GroundFriction = 8.0f;
    float m_AirFriction = 1.0f;
    
    // Stamina
    float m_Stamina = 100.0f;
    float m_MaxStamina = 100.0f;
    float m_StaminaDrainRate = 20.0f;
    float m_StaminaRegenRate = 15.0f;
    float m_StaminaRegenDelay = 1.0f;
    float m_TimeSinceStaminaUse = 0.0f;
    
    // Crouch
    float m_StandingHeight = 1.8f;
    float m_CrouchingHeight = 0.9f;
    float m_CrouchMultiplier = 0.5f;
    float m_CurrentEyeHeight = 1.6f;
    float m_CrouchTransitionSpeed = 8.0f;
    
    // Gliding
    float m_GlideGravityMultiplier = 0.15f;
    float m_GlideDragCoefficient = 2.0f;
    float m_GlideStaminaDrain = 5.0f;
    float m_MinGlideVelocity = -2.0f;
    
    // Climbing
    float m_ClimbSpeed = 3.0f;
    float m_ClimbStaminaDrain = 10.0f;
    glm::vec3 m_ClimbNormal;
    
    // Sliding
    float m_SlideThresholdAngle = 35.0f;
    float m_SlideAcceleration = 15.0f;
    float m_SlideFriction = 2.0f;
    glm::vec3 m_GroundNormal;
    
    // Fall Tracking
    float m_FallDistance = 0.0f;
    float m_FallStartY = 0.0f;
    bool m_WasInAir = false;
    float m_TerminalVelocity = -50.0f;

    float m_FootstepSoundID  = 0.0f;
    uint32_t m_SlideSoundID = 0;
    uint32_t m_GlideSoundID = 0;
    float m_LastYVelocity = 0.0f;

    float m_FootstepTimer = 0.0f;

    bool m_JustRanOutOfStamina = false;
};

} // namespace Luminumbra::Player