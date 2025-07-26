#pragma once

#include "luminumbra/rendering/Camera.h"
#include "luminumbra/world/World.h"
#include <glm/glm.hpp>

// Forward-declare GLFWwindow
struct GLFWwindow;

namespace Luminumbra::Player {

class Player {
public:
    Player(float screenWidth, float screenHeight);

    void processMouseMovement(float xoffset, float yoffset);
    void update(GLFWwindow* window, float deltaTime, const World::World& world);

    const Rendering::Camera& getCamera() const { return m_Camera; }
    const glm::vec3& getPosition() const { return m_Position; }
    const glm::vec3& getVelocity() const { return m_Velocity; }
    float getGravity() const { return m_Gravity; }
    bool isNoClipMode() const { return m_NoclipEnabled; }
    float getStamina() const { return m_Stamina; }
    float getMaxStamina() const { return m_MaxStamina; }
    bool isSprinting() const { return m_IsSprinting; }
    bool isCrouching() const { return m_IsCrouching; }
    float getCurrentEyeHeight() const { return m_CurrentEyeHeight; }
    bool isGliding() const { return m_IsGliding; }
    bool isClimbing() const { return m_IsClimbing; }
    bool isSliding() const { return m_IsSliding; }
    float getFallDistance() const { return m_FallDistance; }

private:
    void resolveCollisions(const World::World& world, float deltaTime);
    void updateMovement(GLFWwindow* window, float deltaTime, const World::World& world);
    void updateStamina(float deltaTime);
    glm::vec3 calculateSurfaceNormal(const World::World& world, const glm::vec3& position) const;
    bool isOnClimbableSurface(const World::World& world) const;
    void updateGliding(GLFWwindow* window, float deltaTime);
    void updateClimbing(GLFWwindow* window, float deltaTime, const World::World& world);
    void updateSliding(float deltaTime);

    // Camera
    Rendering::Camera m_Camera;
    float m_EyeHeight = 1.6f;
    
    // Physics
    glm::vec3 m_PlayerSize;
    glm::vec3 m_Position;
    glm::vec3 m_Velocity;
    float m_Gravity = -9.81f;
    bool m_IsOnGround = false;
    float m_TimeSinceLastGrounded = 0.0f; // For coyote time
    
    // Movement
    float m_BaseMovementSpeed = 5.0f;
    float m_SprintMultiplier = 1.75f;
    float m_JumpForce = 5.0f;
    float m_CoyoteTime = 0.1f; // Grace period for jumping after leaving ground
    
    // Movement smoothing
    float m_GroundAcceleration = 10.0f;
    float m_AirAcceleration = 3.0f;
    float m_GroundFriction = 8.0f;
    float m_AirFriction = 1.0f;
    
    // Sprint & Stamina
    bool m_IsSprinting = false;
    float m_Stamina = 100.0f;
    float m_MaxStamina = 100.0f;
    float m_StaminaDrainRate = 20.0f; // Per second while sprinting
    float m_StaminaRegenRate = 15.0f; // Per second while not sprinting
    float m_StaminaRegenDelay = 1.0f; // Delay before regen starts
    float m_TimeSinceSprintStop = 0.0f;
    
    // Crouch
    bool m_IsCrouching = false;
    float m_CrouchMultiplier = 0.5f; // Speed reduction while crouched
    float m_StandingHeight = 1.8f;
    float m_CrouchingHeight = 0.9f;
    float m_CurrentEyeHeight = 1.6f; // Current eye height (for smooth transitions)
    float m_CrouchTransitionSpeed = 8.0f; // How fast to transition between stand/crouch
    
    // Noclip
    bool m_NoclipEnabled = false;
    bool m_NoclipKeyPressed = false;
    
    // Gliding
    bool m_IsGliding = false;
    bool m_GlideKeyPressed = false;
    float m_GlideGravityMultiplier = 0.15f; // Reduced gravity while gliding
    float m_GlideDragCoefficient = 2.0f; // Air resistance while gliding
    float m_GlideStaminaDrain = 5.0f; // Stamina drain per second while gliding
    float m_MinGlideVelocity = -2.0f; // Minimum downward velocity while gliding
    
    // Climbing
    bool m_IsClimbing = false;
    float m_ClimbSpeed = 3.0f;
    float m_ClimbStaminaDrain = 10.0f; // Stamina drain per second while climbing
    glm::vec3 m_ClimbNormal; // Normal of the surface we're climbing
    
    // Sliding
    bool m_IsSliding = false;
    float m_SlideThresholdAngle = 35.0f; // Degrees - slopes steeper than this cause sliding
    float m_SlideAcceleration = 15.0f; // How fast we accelerate down slopes
    float m_SlideFriction = 2.0f; // Friction while sliding
    glm::vec3 m_GroundNormal; // Normal of the ground surface
    
    // Fall tracking
    float m_FallDistance = 0.0f;
    float m_FallStartY = 0.0f;
    bool m_WasInAir = false;
    float m_TerminalVelocity = -50.0f; // Maximum fall speed
};

} // namespace Luminumbra::Player
