#include "luminumbra/player/Player.h"
#include "luminumbra/audio/AudioManager.h"
#include <algorithm>
#include <cmath>

namespace Luminumbra::Player {

Player::Player(float screenWidth, float screenHeight)
    : m_Camera(screenWidth, screenHeight),
      m_Position(0.0f, 75.0f, 0.0f),
      m_Velocity(0.0f),
      m_PlayerSize(0.6f, 1.8f, 0.6f) { // Standard player is ~1.8m tall, 0.6m wide
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
    // Prevent standing up if the space above is blocked
    if (m_IsCrouching && !isCrouching) {
        glm::vec3 headPos = m_Position + glm::vec3(0.0f, m_StandingHeight * 0.5f, 0.0f);
        if (world.isSolid(headPos)) {
            m_WantsToCrouch = true; // Force crouching to continue
            return;
        }
    }
    m_WantsToCrouch = isCrouching;
}

void Player::processAction(Action action, const World::World& world) {
    switch (action) {
        case Action::Jump:
            // Wall-kick if climbing
            if (m_IsClimbing) {
                m_IsClimbing = false;
                // Give a push away from the wall and upwards
                m_Velocity = m_ClimbNormal * m_JumpForce * 0.8f;
                m_Velocity.y = m_JumpForce;
            } else {
                // Normal jump, with coyote time
                bool canJump = m_IsOnGround || m_TimeSinceLastGrounded < m_CoyoteTime;
                if (canJump) {
                    m_Velocity.y = m_JumpForce;
                    m_IsOnGround = false;
                    m_TimeSinceLastGrounded = m_CoyoteTime; // Prevent double-jumps
                    Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::PlayerJump, m_Position);
                }
            }
            break;
        case Action::ToggleNoclip:
            m_NoclipEnabled = !m_NoclipEnabled;
            if (m_NoclipEnabled) m_Velocity = glm::vec3(0.0f); // Stop all movement when entering noclip
            break;
        case Action::ToggleGlide:
            // Can only start gliding if in the air and has stamina
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
        // Noclip movement is simple and direct
        for (const auto& dir : directions) {
            switch (dir) {
                case Movement::Forward:  wishDir += front; break;
                case Movement::Backward: wishDir -= front; break;
                case Movement::Left:     wishDir -= right; break;
                case Movement::Right:    wishDir += right; break;
                case Movement::Up:       wishDir += glm::vec3(0, 1, 0); break;
                case Movement::Down:     wishDir -= glm::vec3(0, 1, 0); break;
            }
        }
        if (glm::length(wishDir) > 0.0f) {
            m_Position += glm::normalize(wishDir) * m_BaseMovementSpeed * (m_WantsToSprint ? 3.0f : 1.0f) * deltaTime;
        }
    } else {
        // Standard movement uses wishDir for ground/air acceleration
        front.y = 0;
        front = glm::normalize(front);
        right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));

        for (const auto& dir : directions) {
            switch (dir) {
                case Movement::Forward:  wishDir += front; break;
                case Movement::Backward: wishDir -= front; break;
                case Movement::Left:     wishDir -= right; break;
                case Movement::Right:    wishDir += right; break;
                default: break; // Ignore Up/Down
            }
        }
        if (glm::length(wishDir) > 0.0f) {
            wishDir = glm::normalize(wishDir);
        }
        applyMovement(wishDir, deltaTime);
    }
}

void Player::update(float deltaTime, const World::World& world) {
    if (m_NoclipEnabled) {
        m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
        return;
    }

    m_LastYVelocity = m_Velocity.y;

    // Update internal states (crouching, stamina, sliding, climbing, etc.)
    handleAutoStepUp(world);
    updateState(deltaTime, world);

    // Apply physics forces
    if (!m_IsClimbing) {
        float gravityMultiplier = m_IsGliding ? m_GlideGravityMultiplier : 1.0f;
        m_Velocity.y += m_Gravity * gravityMultiplier * deltaTime;
        m_Velocity.y = std::max(m_Velocity.y, m_TerminalVelocity);

        // Apply drag if gliding
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

    // Update looping sound positions
    if (m_SlideSoundID != 0) {
        Audio::AudioManager::getInstance().updateSoundPosition(m_SlideSoundID, m_Position);
    }
    if (m_GlideSoundID != 0) {
        Audio::AudioManager::getInstance().updateSoundPosition(m_GlideSoundID, m_Position);
    }
    
    // Final camera update
    m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
}

void Player::handleAutoStepUp(const World::World& world) {
    // Only perform step-up if on the ground and moving horizontally
    float horizontalSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
    if (!m_IsOnGround || horizontalSpeed < 0.1f) {
        return;
    }

    const float stepHeight = 1.01f;   // Max height the player can step up (1 block)
    const float checkDistance = 0.5f; // How far forward to check for an obstacle

    // Get the direction the player is moving, ignoring vertical movement
    glm::vec3 moveDirection = glm::normalize(glm::vec3(m_Velocity.x, 0.0f, m_Velocity.z));

    // 1. Check for a wall at foot-level directly in front of the player
    glm::vec3 footCheckPos = m_Position + moveDirection * checkDistance;
    if (!world.isSolid(footCheckPos)) {
        return; // No obstacle in the way
    }

    // 2. If there's an obstacle, check for empty space above it (at stepHeight)
    glm::vec3 stepUpCheckPos = footCheckPos + glm::vec3(0.0f, stepHeight, 0.0f);
    if (world.isSolid(stepUpCheckPos)) {
        return; // The space to step into is blocked
    }
    
    // 3. Also check for headroom at the destination to prevent clipping into a ceiling
    glm::vec3 headRoomCheckPos = m_Position + glm::vec3(0.0f, m_PlayerSize.y, 0.0f) + glm::vec3(0.0f, stepHeight, 0.0f);
    if (world.isSolid(headRoomCheckPos)) {
        return; // No headroom to make the step
    }

    // All checks passed, so perform the step-up
    m_Position.y += stepHeight;
    // We are now briefly in the air, which is fine. The next frame's collision check will ground us.
}


// --- Private Helper Methods ---

void Player::applyMovement(const glm::vec3& wishDir, float deltaTime) {
    // Determine current speed based on state
    m_IsSprinting = m_WantsToSprint && m_Stamina > 0.0f && glm::length(wishDir) > 0.0f && m_IsOnGround && !m_IsCrouching;
    float targetSpeed = m_BaseMovementSpeed;
    if (m_IsSprinting) targetSpeed *= m_SprintMultiplier;
    if (m_IsCrouching) targetSpeed *= m_CrouchMultiplier;

    // Apply friction based on state (ground, air, or sliding)
    float currentSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
    float friction = m_IsOnGround ? (m_IsSliding ? m_SlideFriction : m_GroundFriction) : m_AirFriction;
    if (currentSpeed > 0.01f) {
        float drop = currentSpeed * friction * deltaTime;
        float scale = std::max(0.0f, currentSpeed - drop) / currentSpeed;
        m_Velocity.x *= scale;
        m_Velocity.z *= scale;
    }

    // Apply acceleration towards wish direction
    float acceleration = m_IsOnGround ? m_GroundAcceleration : m_AirAcceleration;
    float currentSpeedInDir = glm::dot(glm::vec2(m_Velocity.x, m_Velocity.z), glm::vec2(wishDir.x, wishDir.z));
    float addSpeed = targetSpeed - currentSpeedInDir;
    if (addSpeed > 0) {
        float accelSpeed = std::min(addSpeed, acceleration * targetSpeed * deltaTime);
        m_Velocity.x += accelSpeed * wishDir.x;
        m_Velocity.z += accelSpeed * wishDir.z;
    }
}

void Player::updateState(float deltaTime, const World::World& world) {
    // --- Footstep Sound Logic ---
    // 1. If our footstep sound ID is valid, check if it has finished playing.
    float horizontalSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
    if (m_IsOnGround && horizontalSpeed > 1.5f) {
        m_FootstepTimer -= deltaTime;
        if (m_FootstepTimer <= 0.0f) {
            // Get the correct sound for the surface we're on
            Audio::SoundEvent stepSound = world.getFootstepSoundForPosition(m_Position);
            Audio::AudioManager::getInstance().playSound(stepSound, m_Position);
            
            // Reset timer based on sprint/walk speed
            m_FootstepTimer = m_IsSprinting ? 0.35f : 0.6f;
        }
    }

    // 2. Check if we should play a new footstep sound.
    if (m_IsOnGround && horizontalSpeed > 1.5f && m_FootstepSoundID == 0) {
        // We are on the ground, moving, and no other footstep sound is playing.
        // The sound played here is a full walking sequence, not a single step.
        m_FootstepSoundID = Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::FootstepDirt, m_Position);
    }
    m_TimeSinceLastGrounded = m_IsOnGround ? 0.0f : m_TimeSinceLastGrounded + deltaTime;
    
    updateStamina(deltaTime);
    updateCrouchState(deltaTime, world);
    
    // Check for climbing state
    bool wantsToClimb = !m_IsOnGround && checkForClimbableSurface(world);
    if (wantsToClimb && m_Stamina > 0.0f) {
        m_IsClimbing = true;
        m_Velocity = glm::vec3(0.0f); // Stop other movement when starting to climb
        m_IsGliding = false; // Cannot glide and climb
    } else {
        m_IsClimbing = false;
    }

    // Update gliding state (turn off if on ground, climbing, or out of stamina)
    if (m_IsGliding && (m_IsOnGround || m_IsClimbing || m_Stamina <= 0.0f)) {
        m_IsGliding = false;
    }

    // Update sliding state
    updateSliding(deltaTime);

    // --- Sliding Sound ---
    if (m_IsSliding && m_SlideSoundID == 0) {
        m_SlideSoundID = Audio::AudioManager::getInstance().playLoopingSound(Audio::SoundEvent::SlideLoop, m_Position);
    } else if (!m_IsSliding && m_SlideSoundID != 0) {
        Audio::AudioManager::getInstance().stopSound(m_SlideSoundID);
        m_SlideSoundID = 0;
    }

    // --- Gliding Sound ---
    if (m_IsGliding && m_GlideSoundID == 0) {
        m_GlideSoundID = Audio::AudioManager::getInstance().playLoopingSound(Audio::SoundEvent::GlideLoop, m_Position);
    } else if (!m_IsGliding && m_GlideSoundID != 0) {
        Audio::AudioManager::getInstance().stopSound(m_GlideSoundID);
        m_GlideSoundID = 0;
    }

    // Update fall distance tracking
    if (!m_IsOnGround && !m_IsClimbing && !m_IsGliding) {
        if (!m_WasInAir) {
            m_FallStartY = m_Position.y;
            m_WasInAir = true;
        }
        m_FallDistance = std::max(0.0f, m_FallStartY - m_Position.y);
    } else {
        m_WasInAir = false;
        if (m_IsOnGround) m_FallDistance = 0.0f; // Reset fall distance on landing
    }
}

void Player::updateCrouchState(float deltaTime, const World::World& world) {
    m_IsCrouching = m_WantsToCrouch;
    // Smoothly transition player collision height
    float targetHeight = m_IsCrouching ? m_CrouchingHeight : m_StandingHeight;
    m_PlayerSize.y += (targetHeight - m_PlayerSize.y) * m_CrouchTransitionSpeed * deltaTime;
    // Smoothly transition camera eye height
    float targetEyeHeight = m_IsCrouching ? 0.7f : 1.6f;
    m_CurrentEyeHeight += (targetEyeHeight - m_CurrentEyeHeight) * m_CrouchTransitionSpeed * deltaTime;
}

void Player::applyClimbingMovement(const std::vector<Movement>& directions) {
    glm::vec3 climbDir(0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, m_ClimbNormal)); // Right vector parallel to the wall

    for (const auto& dir : directions) {
        switch (dir) {
            case Movement::Forward:  climbDir += up; break;
            case Movement::Backward: climbDir -= up; break;
            case Movement::Left:     climbDir -= right; break;
            case Movement::Right:    climbDir += right; break;
            default: break;
        }
    }

    m_Velocity = (glm::length(climbDir) > 0.0f) ? glm::normalize(climbDir) * m_ClimbSpeed : glm::vec3(0.0f);
}

void Player::updateStamina(float deltaTime) {
    bool isDrainingStamina = m_IsSprinting || m_IsGliding || (m_IsClimbing && glm::length(m_Velocity) > 0.1f);
    
    if (isDrainingStamina) {
        float drainRate = 0.0f;
        if (m_IsSprinting) drainRate += m_StaminaDrainRate;
        if (m_IsGliding)   drainRate += m_GlideStaminaDrain;
        if (m_IsClimbing)  drainRate += m_ClimbStaminaDrain;
        
        m_Stamina = std::max(0.0f, m_Stamina - drainRate * deltaTime);
        m_TimeSinceStaminaUse = 0.0f;
    } else {
        m_TimeSinceStaminaUse += deltaTime;
        if (m_TimeSinceStaminaUse >= m_StaminaRegenDelay) {
            m_Stamina = std::min(m_MaxStamina, m_Stamina + m_StaminaRegenRate * deltaTime);
        }
    }

    if (m_Stamina <= 0.0f && !m_JustRanOutOfStamina) {
        Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::PlayerOutOfStamina, m_Position);
        m_JustRanOutOfStamina = true;
    } else if (m_Stamina > 0.0f) {
        // Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::PlayerSprint, m_Position);
        // Reset the flag once stamina starts regenerating.
        m_JustRanOutOfStamina = false;
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
        // Project the down vector onto the slope plane to get the slide direction
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
    
    // Check in front of the player at chest height
    glm::vec3 checkPos = m_Position + glm::vec3(0, m_PlayerSize.y * 0.5f, 0) + front * (m_PlayerSize.x * 0.6f);
    
    if (world.isSolid(checkPos)) {
        // We hit a wall, store its normal. The normal is opposite to our forward direction.
        m_ClimbNormal = -front;
        return true;
    }
    
    return false;
}

glm::vec3 Player::calculateSurfaceNormal(const World::World& world, const glm::vec3& position) const {
    // Sample points around the collision point to determine the surface normal
    const float epsilon = 0.01f;
    float dx = (float)world.isSolid(position + glm::vec3(epsilon, 0, 0)) - (float)world.isSolid(position - glm::vec3(epsilon, 0, 0));
    float dy = (float)world.isSolid(position + glm::vec3(0, epsilon, 0)) - (float)world.isSolid(position - glm::vec3(0, epsilon, 0));
    float dz = (float)world.isSolid(position + glm::vec3(0, 0, epsilon)) - (float)world.isSolid(position - glm::vec3(0, 0, epsilon));
    
    glm::vec3 normal(-dx, -dy, -dz);
    return (glm::length(normal) > 0.01f) ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
}

void Player::resolveCollisions(const World::World& world, float deltaTime) {
    glm::vec3 halfSize = m_PlayerSize * 0.5f;
    bool wasOnGround = m_IsOnGround;

    // --- Y-axis (Vertical) ---
    m_Position.y += m_Velocity.y * deltaTime;
    
    if (m_Velocity.y <= 0) { // Moving down or still
        // Check 5 points at the player's base for robust ground detection
        glm::vec3 check_points[] = {
            m_Position,                                                 // Center
            m_Position + glm::vec3(halfSize.x * 0.9f, 0, 0),             // Front
            m_Position - glm::vec3(halfSize.x * 0.9f, 0, 0),             // Back
            m_Position + glm::vec3(0, 0, halfSize.z * 0.9f),             // Right
            m_Position - glm::vec3(0, 0, halfSize.z * 0.9f)              // Left
        };

        bool on_ground = false;
        for (const auto& point : check_points) {
            if (world.isSolid(point)) {
                // If a collision is found, snap the player to the top of the block
                m_Position.y = std::floor(point.y) + 1.0f;
                m_Velocity.y = 0;
                m_GroundNormal = calculateSurfaceNormal(world, m_Position);
                on_ground = true;
                break; // A single ground contact is enough
            }
        }
        m_IsOnGround = on_ground;

    } else { // Moving up
        m_IsOnGround = false;
        // Check for head collision
        glm::vec3 headPos = m_Position + glm::vec3(0.0f, m_PlayerSize.y, 0.0f);
        if (world.isSolid(headPos)) {
            m_Position.y = std::floor(headPos.y) - m_PlayerSize.y - 0.01f; // Snap below ceiling
            m_Velocity.y = 0;
        }
    }

    // --- Landing Sound Logic ---
    if (!wasOnGround && m_IsOnGround) {
        if (m_FallDistance > 20.0f) { // A long, damaging fall
            Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::PlayerHurt, m_Position);
            // You would also apply damage here
        } else if (m_FallDistance > 2.0f) { // A standard landing
            Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::PlayerLand, m_Position);
        }
        // A tiny hop (fallDistance <= 2.0f) makes no sound.
    }

    // --- X-axis (Horizontal) ---
    m_Position.x += m_Velocity.x * deltaTime;
    float checkDirX = m_Velocity.x > 0 ? 1.0f : -1.0f;
    // Check along the full height of the leading vertical edge
    for (float y_offset = 0.1f; y_offset < m_PlayerSize.y; y_offset += 0.8f) { // 3 checks: feet, middle, head
        glm::vec3 checkPos(m_Position.x + halfSize.x * checkDirX, m_Position.y + y_offset, m_Position.z);
        if (world.isSolid(checkPos)) {
            m_Position.x = std::round(checkPos.x) - (halfSize.x * checkDirX);
            m_Velocity.x = 0;
            break;
        }
    }
    
    // --- Z-axis (Horizontal) ---
    m_Position.z += m_Velocity.z * deltaTime;
    float checkDirZ = m_Velocity.z > 0 ? 1.0f : -1.0f;
    // Check along the full height of the leading vertical edge
    for (float y_offset = 0.1f; y_offset < m_PlayerSize.y; y_offset += 0.8f) {
        glm::vec3 checkPos(m_Position.x, m_Position.y + y_offset, m_Position.z + halfSize.z * checkDirZ);
        if (world.isSolid(checkPos)) {
            m_Position.z = std::round(checkPos.z) - (halfSize.z * checkDirZ);
            m_Velocity.z = 0;
            break;
        }
    }
}

} // namespace Luminumbra::Player