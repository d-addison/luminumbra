#include "luminumbra/player/Player.h"
#include "luminumbra/core/Debug.h"
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <string>
#include <algorithm> // For std::min/max
#include <cmath>
#include <glm/gtc/constants.hpp> // For glm::pi

namespace Luminumbra::Player {

Player::Player(float screenWidth, float screenHeight)
    : m_Camera(screenWidth, screenHeight), 
      m_Position(16.0f, 50.0f, 16.0f), 
      m_Velocity(0.0f),
      m_PlayerSize(0.6f, 1.8f, 0.6f) { // Player: 1.8m tall, 0.6m wide/deep
    m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_EyeHeight, m_Position.z));
}

void Player::processMouseMovement(float xoffset, float yoffset) {
    m_Camera.processMouseMovement(xoffset, yoffset);
}

void Player::update(GLFWwindow* window, float deltaTime, const World::World& world) {
    // Update timers
    if (!m_IsOnGround) {
        m_TimeSinceLastGrounded += deltaTime;
    } else {
        m_TimeSinceLastGrounded = 0.0f;
    }
    
    // Noclip toggle
    if (glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS && !m_NoclipKeyPressed) {
        m_NoclipEnabled = !m_NoclipEnabled;
        m_NoclipKeyPressed = true;
        if (m_NoclipEnabled) {
            m_Velocity = glm::vec3(0.0f); // Zero out velocity when entering noclip
        }
    }
    if (glfwGetKey(window, GLFW_KEY_V) == GLFW_RELEASE) {
        m_NoclipKeyPressed = false;
    }

    if (m_NoclipEnabled) {
        // Noclip mode movement
        float speed = m_BaseMovementSpeed * 2.0f * deltaTime; // Faster movement in noclip
        glm::vec3 front = m_Camera.getFront();
        glm::vec3 right = m_Camera.getRight();
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::vec3 moveDirection(0.0f);
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) moveDirection += front;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) moveDirection -= front;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) moveDirection -= right;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) moveDirection += right;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) moveDirection += up;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) moveDirection -= up;

        if (glm::length(moveDirection) > 0.0f) {
            m_Position += glm::normalize(moveDirection) * speed;
        }
    } else {
        // Normal mode
        // Check for climbing first
        if (isOnClimbableSurface(world)) {
            updateClimbing(window, deltaTime, world);
        } else {
            updateMovement(window, deltaTime, world);
            updateGliding(window, deltaTime);
            updateSliding(deltaTime);
        }
        
        updateStamina(deltaTime);
        
        // Apply gravity (modified by gliding/climbing)
        if (!m_IsClimbing) {
            float gravityMultiplier = m_IsGliding ? m_GlideGravityMultiplier : 1.0f;
            m_Velocity.y += m_Gravity * gravityMultiplier * deltaTime;
            
            // Apply terminal velocity
            if (m_Velocity.y < m_TerminalVelocity) {
                m_Velocity.y = m_TerminalVelocity;
            }
            
            // Apply air drag when gliding
            if (m_IsGliding) {
                float horizontalSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
                if (horizontalSpeed > 0.01f) {
                    float dragForce = m_GlideDragCoefficient * deltaTime;
                    float newSpeed = std::max(0.0f, horizontalSpeed - dragForce);
                    float speedRatio = newSpeed / horizontalSpeed;
                    m_Velocity.x *= speedRatio;
                    m_Velocity.z *= speedRatio;
                }
                
                // Cap minimum downward velocity while gliding
                if (m_Velocity.y < m_MinGlideVelocity) {
                    m_Velocity.y = m_MinGlideVelocity;
                }
            }
        }
        
        // Track fall distance
        if (!m_IsOnGround && !m_IsClimbing && !m_IsGliding) {
            if (!m_WasInAir) {
                m_FallStartY = m_Position.y;
                m_WasInAir = true;
            }
            m_FallDistance = std::max(0.0f, m_FallStartY - m_Position.y);
        } else {
            m_WasInAir = false;
            if (m_IsOnGround) {
                m_FallDistance = 0.0f;
            }
        }
        
        // Resolve collisions and update position
        resolveCollisions(world, deltaTime);
    }
    
    // Final camera update with smooth eye height transitions
    m_Camera.setPosition(glm::vec3(m_Position.x, m_Position.y + m_CurrentEyeHeight, m_Position.z));
}

void Player::updateMovement(GLFWwindow* window, float deltaTime, const World::World& world) {
    // Get player input and calculate desired movement
    glm::vec3 front = m_Camera.getFront();
    front.y = 0;
    if (glm::length(front) > 0.0f) {
        front = glm::normalize(front);
    }

    glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));

    glm::vec3 wishDir(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) wishDir += front;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) wishDir -= front;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) wishDir -= right;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) wishDir += right;
    
    if (glm::length(wishDir) > 0.0f) {
        wishDir = glm::normalize(wishDir);
    }

    // Check for crouch input
    bool wantsCrouch = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    if (wantsCrouch != m_IsCrouching) {
        // Check if we can stand up (if transitioning from crouch to stand)
        if (m_IsCrouching && !wantsCrouch) {
            // Check if there's space above to stand
            glm::vec3 standCheck = m_Position;
            standCheck.y += m_StandingHeight * 0.5f;
            if (!world.isSolid(standCheck)) {
                m_IsCrouching = false;
            }
        } else {
            m_IsCrouching = wantsCrouch;
        }
    }
    
    // Update player height based on crouch state
    float targetHeight = m_IsCrouching ? m_CrouchingHeight : m_StandingHeight;
    float currentHeight = m_PlayerSize.y;
    if (std::abs(currentHeight - targetHeight) > 0.01f) {
        float heightDiff = targetHeight - currentHeight;
        float changeSpeed = m_CrouchTransitionSpeed * deltaTime;
        if (std::abs(heightDiff) < changeSpeed) {
            m_PlayerSize.y = targetHeight;
        } else {
            m_PlayerSize.y += (heightDiff > 0 ? changeSpeed : -changeSpeed);
        }
    }
    
    // Update eye height smoothly
    float targetEyeHeight = m_IsCrouching ? 0.7f : 1.6f;
    float eyeHeightDiff = targetEyeHeight - m_CurrentEyeHeight;
    if (std::abs(eyeHeightDiff) > 0.01f) {
        float changeSpeed = m_CrouchTransitionSpeed * deltaTime;
        if (std::abs(eyeHeightDiff) < changeSpeed) {
            m_CurrentEyeHeight = targetEyeHeight;
        } else {
            m_CurrentEyeHeight += (eyeHeightDiff > 0 ? changeSpeed : -changeSpeed);
        }
    }

    // Check for sprint input (can't sprint while crouching)
    bool wantsSprint = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    m_IsSprinting = wantsSprint && m_Stamina > 0.0f && glm::length(wishDir) > 0.0f && m_IsOnGround && !m_IsCrouching;
    
    // Calculate movement speed
    float targetSpeed = m_BaseMovementSpeed;
    if (m_IsSprinting) {
        targetSpeed *= m_SprintMultiplier;
    } else if (m_IsCrouching) {
        targetSpeed *= m_CrouchMultiplier;
    }
    
    // Apply movement with improved physics
    float currentSpeed = glm::length(glm::vec2(m_Velocity.x, m_Velocity.z));
    float acceleration = m_IsOnGround ? m_GroundAcceleration : m_AirAcceleration;
    float friction = m_IsOnGround ? m_GroundFriction : m_AirFriction;
    
    // Apply friction
    if (currentSpeed > 0.01f) {
        float drop = currentSpeed * friction * deltaTime;
        float newSpeed = std::max(0.0f, currentSpeed - drop);
        if (currentSpeed > 0) {
            m_Velocity.x *= (newSpeed / currentSpeed);
            m_Velocity.z *= (newSpeed / currentSpeed);
        }
    }
    
    // Apply acceleration
    if (glm::length(wishDir) > 0.0f) {
        float currentSpeedInDir = glm::dot(glm::vec2(m_Velocity.x, m_Velocity.z), glm::vec2(wishDir.x, wishDir.z));
        float addSpeed = targetSpeed - currentSpeedInDir;
        
        if (addSpeed > 0) {
            float accelSpeed = acceleration * targetSpeed * deltaTime;
            accelSpeed = std::min(accelSpeed, addSpeed);
            
            m_Velocity.x += accelSpeed * wishDir.x;
            m_Velocity.z += accelSpeed * wishDir.z;
        }
    }
    
    // Jumping with coyote time
    bool canJump = m_IsOnGround || m_TimeSinceLastGrounded < m_CoyoteTime;
    if (canJump && glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        // Add jump velocity with momentum preservation
        m_Velocity.y = m_JumpForce;
        
        // Add a small boost in the direction of movement
        if (glm::length(wishDir) > 0.0f && currentSpeed > 0.1f) {
            float jumpBoost = std::min(currentSpeed * 0.2f, 1.0f);
            m_Velocity.x += wishDir.x * jumpBoost;
            m_Velocity.z += wishDir.z * jumpBoost;
        }
        
        m_IsOnGround = false;
        m_TimeSinceLastGrounded = m_CoyoteTime; // Prevent double jumping
    }
}

void Player::updateStamina(float deltaTime) {
    bool isDrainingStamina = m_IsSprinting || m_IsGliding || m_IsClimbing;
    
    if (isDrainingStamina) {
        float drainRate = 0.0f;
        if (m_IsSprinting) drainRate += m_StaminaDrainRate;
        if (m_IsGliding) drainRate += m_GlideStaminaDrain;
        if (m_IsClimbing) drainRate += m_ClimbStaminaDrain;
        
        m_Stamina -= drainRate * deltaTime;
        m_Stamina = std::max(0.0f, m_Stamina);
        m_TimeSinceSprintStop = 0.0f;
    } else {
        // Regenerate stamina after delay
        m_TimeSinceSprintStop += deltaTime;
        
        if (m_TimeSinceSprintStop >= m_StaminaRegenDelay) {
            m_Stamina += m_StaminaRegenRate * deltaTime;
            m_Stamina = std::min(m_Stamina, m_MaxStamina);
        }
    }
}

void Player::updateGliding(GLFWwindow* window, float deltaTime) {
    bool wantsGlide = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
    
    if (wantsGlide && !m_GlideKeyPressed) {
        m_GlideKeyPressed = true;
        // Toggle gliding if we're in the air and have stamina
        if (!m_IsOnGround && !m_IsClimbing && m_Stamina > 0.0f) {
            m_IsGliding = !m_IsGliding;
        }
    }
    if (!wantsGlide) {
        m_GlideKeyPressed = false;
    }
    
    // Stop gliding if we run out of stamina or hit the ground
    if (m_IsGliding && (m_Stamina <= 0.0f || m_IsOnGround || m_IsClimbing)) {
        m_IsGliding = false;
    }
}

void Player::updateClimbing(GLFWwindow* window, float deltaTime, const World::World& world) {
    m_IsClimbing = true;
    
    // Get input for climbing movement
    float climbY = 0.0f;
    float climbX = 0.0f;
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) climbY += 1.0f;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) climbY -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) climbX -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) climbX += 1.0f;
    
    // Calculate climbing movement along the wall
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(up, m_ClimbNormal));
    
    glm::vec3 climbVelocity = (up * climbY + right * climbX) * m_ClimbSpeed;
    
    // Apply climb movement
    m_Velocity = climbVelocity;
    
    // Stop climbing if we jump or run out of stamina
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS || m_Stamina <= 0.0f) {
        m_IsClimbing = false;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
            // Jump off the wall
            m_Velocity = m_ClimbNormal * m_JumpForce * 0.7f; // Jump away from wall
            m_Velocity.y = m_JumpForce;
        }
    }
}

void Player::updateSliding(float deltaTime) {
    if (!m_IsOnGround || glm::length(m_GroundNormal) < 0.1f) {
        m_IsSliding = false;
        return;
    }
    
    // Calculate slope angle
    float slopeAngle = glm::degrees(glm::acos(glm::dot(m_GroundNormal, glm::vec3(0.0f, 1.0f, 0.0f))));
    
    if (slopeAngle > m_SlideThresholdAngle) {
        m_IsSliding = true;
        
        // Calculate slide direction (down the slope)
        glm::vec3 slideDirection = glm::normalize(glm::vec3(m_GroundNormal.x, 0.0f, m_GroundNormal.z));
        
        // Apply sliding acceleration
        float slideForce = m_SlideAcceleration * sin(glm::radians(slopeAngle)) * deltaTime;
        m_Velocity += slideDirection * slideForce;
        
        // Reduce player control while sliding
        m_GroundFriction = m_SlideFriction;
    } else {
        m_IsSliding = false;
        m_GroundFriction = 8.0f; // Reset to normal friction
    }
}

bool Player::isOnClimbableSurface(const World::World& world) const {
    // Check multiple points around the player for climbable surfaces
    glm::vec3 halfSize = m_PlayerSize / 2.0f;
    const float checkDistance = 0.3f;
    
    // Check forward, back, left, right
    glm::vec3 checkDirections[] = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 0.0f, -1.0f)
    };
    
    for (const auto& dir : checkDirections) {
        glm::vec3 checkPos = m_Position + dir * (halfSize.x + checkDistance);
        
        // Check at multiple heights
        for (float heightOffset = -halfSize.y * 0.5f; heightOffset <= halfSize.y * 0.5f; heightOffset += 0.5f) {
            glm::vec3 pos = checkPos;
            pos.y += heightOffset;
            
            if (world.isSolid(pos)) {
                // Found a wall - for now, all walls are climbable
                // In the future, you could check for specific block types
                const_cast<Player*>(this)->m_ClimbNormal = -dir; // Store the wall normal
                return true;
            }
        }
    }
    
    return false;
}

glm::vec3 Player::calculateSurfaceNormal(const World::World& world, const glm::vec3& position) const {
    // Sample points around the position to estimate surface normal
    const float epsilon = 0.1f;
    
    float dx = world.isSolid(position + glm::vec3(epsilon, 0, 0)) - 
               world.isSolid(position - glm::vec3(epsilon, 0, 0));
    float dy = world.isSolid(position + glm::vec3(0, epsilon, 0)) - 
               world.isSolid(position - glm::vec3(0, epsilon, 0));
    float dz = world.isSolid(position + glm::vec3(0, 0, epsilon)) - 
               world.isSolid(position - glm::vec3(0, 0, epsilon));
    
    glm::vec3 normal(-dx, -dy, -dz);
    
    if (glm::length(normal) > 0.01f) {
        return glm::normalize(normal);
    }
    
    return glm::vec3(0.0f, 1.0f, 0.0f); // Default to up
}

void Player::resolveCollisions(const World::World& world, float deltaTime) {
    glm::vec3 nextPosition = m_Position + m_Velocity * deltaTime;
    glm::vec3 halfSize = m_PlayerSize / 2.0f;

    // More sophisticated collision detection with multiple sample points
    const float epsilon = 0.001f; // Small offset to prevent floating point issues
    const int samplesPerAxis = 3; // Check multiple points along each edge
    
    // Y-axis (vertical) collision
    bool groundHit = false;
    float closestGroundY = -1000.0f;
    
    // Sample multiple points on the bottom of the player
    for (int sx = 0; sx < samplesPerAxis; ++sx) {
        for (int sz = 0; sz < samplesPerAxis; ++sz) {
            float fx = (sx / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            float fz = (sz / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            
            glm::vec3 samplePos(
                nextPosition.x + fx * halfSize.x * 0.9f,
                nextPosition.y - halfSize.y,
                nextPosition.z + fz * halfSize.z * 0.9f
            );
            
            // Check if this point would be inside solid terrain
            if (world.isSolid(samplePos)) {
                groundHit = true;
                // Binary search to find the exact ground position
                float low = m_Position.y - halfSize.y;
                float high = nextPosition.y - halfSize.y;
                for (int i = 0; i < 5; ++i) {
                    float mid = (low + high) * 0.5f;
                    glm::vec3 testPos(samplePos.x, mid, samplePos.z);
                    if (world.isSolid(testPos)) {
                        high = mid;
                    } else {
                        low = mid;
                    }
                }
                closestGroundY = std::max(closestGroundY, high + halfSize.y + epsilon);
            }
        }
    }
    
    if (groundHit && m_Velocity.y <= 0) {
        m_Position.y = closestGroundY;
        m_Velocity.y = 0;
        m_IsOnGround = true;
        
        // Calculate ground normal at the landing position
        glm::vec3 groundPos(m_Position.x, m_Position.y - halfSize.y - 0.1f, m_Position.z);
        m_GroundNormal = calculateSurfaceNormal(world, groundPos);
    } else {
        m_Position.y = nextPosition.y;
        m_IsOnGround = false;
        m_GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        
        // Check for ground proximity (for coyote time, etc.)
        glm::vec3 groundCheck(m_Position.x, m_Position.y - halfSize.y - 0.1f, m_Position.z);
        if (world.isSolid(groundCheck)) {
            m_IsOnGround = true;
            m_GroundNormal = calculateSurfaceNormal(world, groundCheck);
        }
    }
    
    // Check ceiling collision
    if (m_Velocity.y > 0) {
        for (int sx = 0; sx < samplesPerAxis; ++sx) {
            for (int sz = 0; sz < samplesPerAxis; ++sz) {
                float fx = (sx / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
                float fz = (sz / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
                
                glm::vec3 samplePos(
                    m_Position.x + fx * halfSize.x * 0.9f,
                    m_Position.y + halfSize.y,
                    m_Position.z + fz * halfSize.z * 0.9f
                );
                
                if (world.isSolid(samplePos)) {
                    m_Velocity.y = 0;
                    break;
                }
            }
        }
    }
    
    // X-axis collision with sliding
    float desiredX = m_Position.x + m_Velocity.x * deltaTime;
    bool xBlocked = false;
    
    for (int sy = 0; sy < samplesPerAxis; ++sy) {
        for (int sz = 0; sz < samplesPerAxis; ++sz) {
            float fy = (sy / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            float fz = (sz / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            
            glm::vec3 samplePos(
                desiredX + (m_Velocity.x > 0 ? halfSize.x : -halfSize.x),
                m_Position.y + fy * halfSize.y * 0.9f,
                m_Position.z + fz * halfSize.z * 0.9f
            );
            
            if (world.isSolid(samplePos)) {
                xBlocked = true;
                break;
            }
        }
        if (xBlocked) break;
    }
    
    if (!xBlocked) {
        m_Position.x = desiredX;
    } else {
        m_Velocity.x = 0;
    }
    
    // Z-axis collision with sliding
    float desiredZ = m_Position.z + m_Velocity.z * deltaTime;
    bool zBlocked = false;
    
    for (int sy = 0; sy < samplesPerAxis; ++sy) {
        for (int sx = 0; sx < samplesPerAxis; ++sx) {
            float fy = (sy / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            float fx = (sx / (float)(samplesPerAxis - 1)) * 2.0f - 1.0f;
            
            glm::vec3 samplePos(
                m_Position.x + fx * halfSize.x * 0.9f,
                m_Position.y + fy * halfSize.y * 0.9f,
                desiredZ + (m_Velocity.z > 0 ? halfSize.z : -halfSize.z)
            );
            
            if (world.isSolid(samplePos)) {
                zBlocked = true;
                break;
            }
        }
        if (zBlocked) break;
    }
    
    if (!zBlocked) {
        m_Position.z = desiredZ;
    } else {
        m_Velocity.z = 0;
    }
}

} // namespace Luminumbra::Player
