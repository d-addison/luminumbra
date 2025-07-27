#include "luminumbra/player/Player.h"
#include <algorithm>
#include <cmath>

namespace Luminumbra::Player {

Player::Player(float screenWidth, float screenHeight)
    : m_Camera(screenWidth, screenHeight),
      m_Position(0.0f, 75.0f, 0.0f),
      m_Velocity(0.0f),
      m_PlayerSize(0.6f, 1.8f, 0.6f) {
    reset(m_Position);
}

void Player::reset(const glm::vec3& position) {
    m_Position = position;
    m_Velocity = glm::vec3(0.0f);
    m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
    m_Stamina = m_MaxStamina;
    m_IsOnGround = false;
    m_IsSprinting = false;
    m_IsCrouching = false;
    m_NoclipEnabled = false;
    m_IsGliding = false;
    m_IsClimbing = false;
    m_IsSliding = false;
    m_FallDistance = 0.0f;
}

Core::PlayerSaveData Player::serialize() const {
    Core::PlayerSaveData data;
    data.position = m_Position;
    data.stamina = m_Stamina;
    return data;
}

void Player::applySaveData(const Core::PlayerSaveData& data) {
    // We call reset to ensure a clean state before applying loaded data
    reset(data.position); 
    m_Stamina = data.stamina;
}

void Player::processMouseMovement(float xoffset, float yoffset) {
    m_Camera.processMouseMovement(xoffset, yoffset);
}

void Player::processMouseScroll(float yoffset) {
    m_Camera.processMouseScroll(yoffset);
}

void Player::setSprinting(bool isSprinting) {
    m_WantsToSprint = isSprinting;
}

void Player::setCrouching(bool isCrouching, const World::World& world) {
    if (m_IsCrouching && !isCrouching) {
        // Attempting to stand up, check for space
        glm::vec3 standCheckPos = m_Position + glm::vec3(0.0f, m_StandingHeight - m_PlayerSize.y, 0.0f);
        if (!world.isSolid(standCheckPos)) {
            m_WantsToCrouch = false;
        }
    } else {
        m_WantsToCrouch = isCrouching;
    }
}

void Player::processAction(Action action, const World::World& world) {
    switch (action) {
        case Action::Jump:
            if (m_IsClimbing) {
                m_IsClimbing = false;
                m_Velocity = m_ClimbNormal * m_JumpForce * 0.7f;
                m_Velocity.y = m_JumpForce;
            } else {
                bool canJump = m_IsOnGround || m_TimeSinceLastGrounded < m_CoyoteTime;
                if (canJump) {
                    m_Velocity.y = m_JumpForce;
                    m_IsOnGround = false;
                    m_TimeSinceLastGrounded = m_CoyoteTime;
                }
            }
            break;
        case Action::ToggleNoclip:
            m_NoclipEnabled = !m_NoclipEnabled;
            if (m_NoclipEnabled) m_Velocity = glm::vec3(0.0f);
            break;
        case Action::ToggleGlide:
            if (!m_IsOnGround && !m_IsClimbing && m_Stamina > 0.0f) {
                m_IsGliding = !m_IsGliding;
            }
            break;
    }
}

void Player::processMovement(const std::vector<Movement>& directions, float deltaTime) {
    if (m_IsClimbing) {
        applyClimbingMovement(directions);
        return;
    }

    glm::vec3 wishDir(0.0f);
    glm::vec3 front = m_Camera.getFront();
    glm::vec3 right = m_Camera.getRight();

    if (m_NoclipEnabled) {
        for (const auto& dir : directions) {
            switch (dir) {
                case Movement::Forward: wishDir += front; break;
                case Movement::Backward: wishDir -= front; break;
                case Movement::Left: wishDir -= right; break;
                case Movement::Right: wishDir += right; break;
                case Movement::Up: wishDir += glm::vec3(0, 1, 0); break;
                case Movement::Down: wishDir -= glm::vec3(0, 1, 0); break;
            }
        }
        if (glm::length(wishDir) > 0.0f) {
            m_Position += glm::normalize(wishDir) * m_BaseMovementSpeed * 2.0f * deltaTime;
        }
    } else {
        front.y = 0;
        front = glm::normalize(front);
        right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));

        for (const auto& dir : directions) {
            switch (dir) {
                case Movement::Forward: wishDir += front; break;
                case Movement::Backward: wishDir -= front; break;
                case Movement::Left: wishDir -= right; break;
                case Movement::Right: wishDir += right; break;
                default: break; // Ignore Up/Down
            }
        }
        if (glm::length(wishDir) > 0.0f) {
            wishDir = glm::normalize(wishDir);
        }
        applyMovement(wishDir, deltaTime);
    }
}


// --- NEW: Main Physics & State Update ---

void Player::update(float deltaTime, const World::World& world) {
    if (m_NoclipEnabled) {
        // In noclip, we only update camera position. All movement is handled by processMovement.
        m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
        return;
    }

    // Update internal states based on conditions
    updateState(deltaTime, world);

    // Apply physics forces
    if (!m_IsClimbing) {
        float gravityMultiplier = m_IsGliding ? m_GlideGravityMultiplier : 1.0f;
        m_Velocity.y += m_Gravity * gravityMultiplier * deltaTime;
        m_Velocity.y = std::max(m_Velocity.y, m_TerminalVelocity);

        if (m_IsGliding) {
            m_Velocity.y = std::max(m_Velocity.y, m_MinGlideVelocity);
            float horizontalSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
            if (horizontalSpeed > 0.01f) {
                float dragForce = m_GlideDragCoefficient * deltaTime;
                m_Velocity.x *= std::max(0.0f, horizontalSpeed - dragForce) / horizontalSpeed;
                m_Velocity.z *= std::max(0.0f, horizontalSpeed - dragForce) / horizontalSpeed;
            }
        }
    }

    // Resolve collisions with the world
    resolveCollisions(world, deltaTime);
    
    // Final camera update
    m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
}


// --- Private Helper Methods ---

void Player::applyMovement(const glm::vec3& wishDir, float deltaTime) {
    // Sprinting logic
    m_IsSprinting = m_WantsToSprint && m_Stamina > 0.0f && glm::length(wishDir) > 0.0f && m_IsOnGround && !m_IsCrouching;

    // Determine target speed
    float targetSpeed = m_BaseMovementSpeed;
    if (m_IsSprinting) targetSpeed *= m_SprintMultiplier;
    else if (m_IsCrouching) targetSpeed *= m_CrouchMultiplier;

    // Apply friction
    float currentSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
    float friction = m_IsOnGround ? (m_IsSliding ? m_SlideFriction : m_GroundFriction) : m_AirFriction;
    if (currentSpeed > 0.01f) {
        float drop = currentSpeed * friction * deltaTime;
        float scale = std::max(0.0f, currentSpeed - drop) / currentSpeed;
        m_Velocity.x *= scale;
        m_Velocity.z *= scale;
    }

    // Apply acceleration
    if (glm::length(wishDir) > 0.0f) {
        float acceleration = m_IsOnGround ? m_GroundAcceleration : m_AirAcceleration;
        float currentSpeedInDir = glm::dot(glm::vec2(m_Velocity.x, m_Velocity.z), glm::vec2(wishDir.x, wishDir.z));
        float addSpeed = targetSpeed - currentSpeedInDir;
        if (addSpeed > 0) {
            float accelSpeed = std::min(addSpeed, acceleration * targetSpeed * deltaTime);
            m_Velocity.x += accelSpeed * wishDir.x;
            m_Velocity.z += accelSpeed * wishDir.z;
        }
    }
}

void Player::updateState(float deltaTime, const World::World& world) {
    // Update timers
    m_TimeSinceLastGrounded = m_IsOnGround ? 0.0f : m_TimeSinceLastGrounded + deltaTime;
    
    // Update stamina
    updateStamina(deltaTime);
    
    // Update crouching state
    updateCrouchState(deltaTime, world);

    // Update climbing state
    //if (!m_IsClimbing && isOnClimbableSurface(world)) {
    if (!m_IsClimbing) {
        // TODO: This is a simplification; a real implementation would check for player intent to climb.
        // For now, we assume touching a climbable surface while not grounded initiates climbing.
    }
    m_IsClimbing = checkForClimbableSurface(world);
    if (m_IsClimbing) m_Velocity = glm::vec3(0.0f); // Stop all other movement when starting to climb

    // Update gliding state
    if (m_IsGliding && (m_Stamina <= 0.0f || m_IsOnGround || m_IsClimbing)) {
        m_IsGliding = false;
    }

    // Update sliding state
    updateSliding(deltaTime);

    // Update fall distance tracking
    if (!m_IsOnGround && !m_IsClimbing && !m_IsGliding) {
        if (!m_WasInAir) {
            m_FallStartY = m_Position.y;
            m_WasInAir = true;
        }
        m_FallDistance = std::max(0.0f, m_FallStartY - m_Position.y);
    } else {
        m_WasInAir = false;
        if (m_IsOnGround) m_FallDistance = 0.0f;
    }
}

void Player::updateCrouchState(float deltaTime, const World::World& world) {
    m_IsCrouching = m_WantsToCrouch;

    // Update player height based on crouch state
    float targetHeight = m_IsCrouching ? m_CrouchingHeight : m_StandingHeight;
    m_PlayerSize.y += (targetHeight - m_PlayerSize.y) * m_CrouchTransitionSpeed * deltaTime;

    // Update eye height smoothly
    float targetEyeHeight = m_IsCrouching ? 0.7f : 1.6f;
    m_CurrentEyeHeight += (targetEyeHeight - m_CurrentEyeHeight) * m_CrouchTransitionSpeed * deltaTime;
}

void Player::applyClimbingMovement(const std::vector<Movement>& directions) {
    glm::vec3 climbDir(0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, m_ClimbNormal));

    for (const auto& dir : directions) {
        switch (dir) {
            case Movement::Forward: climbDir += up; break;
            case Movement::Backward: climbDir -= up; break;
            case Movement::Left: climbDir -= right; break;
            case Movement::Right: climbDir += right; break;
            default: break;
        }
    }

    m_Velocity = (glm::length(climbDir) > 0.0f)
        ? glm::normalize(climbDir) * m_ClimbSpeed
        : glm::vec3(0.0f);

    if (m_Stamina <= 0.0f) {
        m_IsClimbing = false;
    }
}

// A sample refactored private function:
void Player::updateStamina(float deltaTime) {
    bool isDrainingStamina = m_IsSprinting || m_IsGliding || m_IsClimbing;
    
    if (isDrainingStamina) {
        float drainRate = 0.0f;
        if (m_IsSprinting) drainRate += m_StaminaDrainRate;
        if (m_IsGliding) drainRate += m_GlideStaminaDrain;
        if (m_IsClimbing) drainRate += m_ClimbStaminaDrain;
        
        m_Stamina = std::max(0.0f, m_Stamina - drainRate * deltaTime);
        m_TimeSinceStaminaUse = 0.0f;
    } else {
        m_TimeSinceStaminaUse += deltaTime;
        if (m_TimeSinceStaminaUse >= m_StaminaRegenDelay) {
            m_Stamina = std::min(m_MaxStamina, m_Stamina + m_StaminaRegenRate * deltaTime);
        }
    }
}

void Player::updateSliding(float deltaTime) {
    if (!m_IsOnGround || glm::length(m_GroundNormal) < 0.1f) {
        m_IsSliding = false;
        return;
    }
    
    float slopeAngle = glm::degrees(glm::acos(glm::dot(m_GroundNormal, glm::vec3(0.0f, 1.0f, 0.0f))));
    
    if (slopeAngle > m_SlideThresholdAngle) {
        m_IsSliding = true;
        glm::vec3 slideDirection = glm::normalize(glm::vec3(m_GroundNormal.x, 0.0f, m_GroundNormal.z));
        float slideForce = m_SlideAcceleration * sin(glm::radians(slopeAngle)) * deltaTime;
        m_Velocity += slideDirection * slideForce;
    } else {
        m_IsSliding = false;
    }
}

bool Player::checkForClimbableSurface(const World::World& world) {
    if (m_IsOnGround) return false;

    glm::vec3 front = m_Camera.getFront();
    front.y = 0.0;
    front = glm::normalize(front);
    
    // Check in front of the player at eye level
    glm::vec3 checkPos = m_Position + glm::vec3(0, m_CurrentEyeHeight * 0.5f, 0) + front * 0.5f;
    
    if (world.isSolid(checkPos)) {
        // We hit a wall, store its normal. The normal is opposite to our forward direction.
        m_ClimbNormal = -front;
        return true;
    }
    
    return false;
}

glm::vec3 Player::calculateSurfaceNormal(const World::World& world, const glm::vec3& position) const {
    const float epsilon = 0.01f;
    float dx = (float)world.isSolid(position + glm::vec3(epsilon, 0, 0)) - (float)world.isSolid(position - glm::vec3(epsilon, 0, 0));
    float dy = (float)world.isSolid(position + glm::vec3(0, epsilon, 0)) - (float)world.isSolid(position - glm::vec3(0, epsilon, 0));
    float dz = (float)world.isSolid(position + glm::vec3(0, 0, epsilon)) - (float)world.isSolid(position - glm::vec3(0, 0, epsilon));
    
    glm::vec3 normal(-dx, -dy, -dz);
    return (glm::length(normal) > 0.01f) ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Replace the entire resolveCollisions function in Player.cpp with this one:

void Player::resolveCollisions(const World::World& world, float deltaTime) {
    glm::vec3 halfSize = m_PlayerSize * 0.5f;

    // --- Process Y-axis (Vertical) ---
    m_Position.y += m_Velocity.y * deltaTime;
    if (m_Velocity.y <= 0) { // Moving down (or still)
        // Check multiple points at the player's feet
        glm::vec3 feetCenter(m_Position.x, m_Position.y - halfSize.y, m_Position.z);
        if (world.isSolid(feetCenter)) {
            m_Position.y = std::floor(feetCenter.y) + 1.0f + halfSize.y;
            m_Velocity.y = 0;
            m_IsOnGround = true;
            m_GroundNormal = calculateSurfaceNormal(world, m_Position - glm::vec3(0, halfSize.y, 0));
        } else {
            m_IsOnGround = false;
        }
    } else { // Moving up
        m_IsOnGround = false;
        glm::vec3 headCenter(m_Position.x, m_Position.y + halfSize.y, m_Position.z);
        if (world.isSolid(headCenter)) {
            m_Position.y = std::floor(headCenter.y) - halfSize.y;
            m_Velocity.y = 0;
        }
    }

    // --- Process X-axis (Horizontal) ---
    m_Position.x += m_Velocity.x * deltaTime;
    float checkDirX = (m_Velocity.x > 0 ? 1.0f : -1.0f);
    // Check three points along the leading vertical edge of the player
    for (float yOffset = -1.0f; yOffset <= 1.0f; yOffset += 1.0f) {
        glm::vec3 checkPos(m_Position.x + halfSize.x * checkDirX, m_Position.y + halfSize.y * yOffset, m_Position.z);
        if (world.isSolid(checkPos)) {
            m_Position.x = std::round(checkPos.x) - halfSize.x * checkDirX;
            m_Velocity.x = 0;
            break;
        }
    }

    // --- Process Z-axis (Horizontal) ---
    m_Position.z += m_Velocity.z * deltaTime;
    float checkDirZ = (m_Velocity.z > 0 ? 1.0f : -1.0f);
    // Check three points along the leading vertical edge of the player
    for (float yOffset = -1.0f; yOffset <= 1.0f; yOffset += 1.0f) {
        glm::vec3 checkPos(m_Position.x, m_Position.y + halfSize.y * yOffset, m_Position.z + halfSize.z * checkDirZ);
        if (world.isSolid(checkPos)) {
            m_Position.z = std::round(checkPos.z) - halfSize.z * checkDirZ;
            m_Velocity.z = 0;
            break;
        }
    }
}

} // namespace Luminumbra::Player