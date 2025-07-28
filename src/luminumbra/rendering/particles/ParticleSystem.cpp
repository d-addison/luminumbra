#include "luminumbra/rendering/particles/Particle.h"
#include "luminumbra/rendering/Camera.h"
#include "luminumbra/core/GLError.h"

#include "luminumbra/world/World.h" 
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <random>
#include <algorithm>

namespace Luminumbra::Rendering {

// Static random number generators for variations
static std::random_device rd;
static std::mt19937 s_RandomEngine(rd());
static std::uniform_real_distribution<float> s_Distribution(-1.0f, 1.0f);
static std::uniform_real_distribution<float> s_UnitDistribution(0.0f, 1.0f);


ParticleSystem::ParticleSystem(uint32_t maxParticles) {
    m_ParticlePool.resize(maxParticles);
    init();
}

ParticleSystem::~ParticleSystem() {
    glDeleteVertexArrays(1, &m_QuadVAO);
    glDeleteBuffers(1, &m_QuadVBO);
    glDeleteBuffers(1, &m_QuadEBO);
    glDeleteBuffers(1, &m_InstanceVBO);
}

void ParticleSystem::init() {
    float vertices[] = {
        // Position     // Tex Coords
        -0.5f, -0.5f,   0.0f, 0.0f,
         0.5f, -0.5f,   1.0f, 0.0f,
         0.5f,  0.5f,   1.0f, 1.0f,
        -0.5f,  0.5f,   0.0f, 1.0f,
    };
    uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

    GLCall(glGenVertexArrays(1, &m_QuadVAO));
    GLCall(glBindVertexArray(m_QuadVAO));

    GLCall(glGenBuffers(1, &m_QuadVBO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_QuadVBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW));
    
    GLCall(glGenBuffers(1, &m_QuadEBO));
    GLCall(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_QuadEBO));
    GLCall(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW));
    
    GLCall(glEnableVertexAttribArray(0));
    GLCall(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0));
    GLCall(glEnableVertexAttribArray(1));
    GLCall(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float))));

    GLCall(glGenBuffers(1, &m_InstanceVBO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVBO));
    GLCall(glBufferData(GL_ARRAY_BUFFER, m_ParticlePool.size() * sizeof(ParticleInstanceData), nullptr, GL_DYNAMIC_DRAW));

    size_t stride = sizeof(ParticleInstanceData);
    GLCall(glEnableVertexAttribArray(2));
    GLCall(glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ParticleInstanceData, worldPosition)));
    GLCall(glEnableVertexAttribArray(3));
    GLCall(glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ParticleInstanceData, size)));
    GLCall(glEnableVertexAttribArray(4));
    GLCall(glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ParticleInstanceData, color)));
    GLCall(glEnableVertexAttribArray(5));
    GLCall(glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ParticleInstanceData, rotation)));

    GLCall(glVertexAttribDivisor(2, 1));
    GLCall(glVertexAttribDivisor(3, 1));
    GLCall(glVertexAttribDivisor(4, 1));
    GLCall(glVertexAttribDivisor(5, 1));

    GLCall(glBindVertexArray(0));
}

std::vector<ParticleDeathEvent> ParticleSystem::update(float dt, Luminumbra::World::World& world) {
    std::vector<ParticleDeathEvent> deathEvents;

    for (uint32_t i = 0; i < m_ActiveParticleCount; ++i) {
        Particle& p = m_ParticlePool[i];
        bool shouldDie = false;

        // Life check based on timer
        p.lifeRemaining -= dt;
        if (p.lifeRemaining <= 0.0f) {
            shouldDie = true;
        }

        // Specific death conditions
        if (p.type == ParticleType::Rain) {
            // Check for ground collision
            if (world.isSolid(p.position)) {
                shouldDie = true;
            }
        }

        if (shouldDie) {
            deathEvents.push_back({p.type, p.position});
            m_ParticlePool[i] = m_ParticlePool[m_ActiveParticleCount - 1];
            m_ActiveParticleCount--;
            i--; 
            continue;
        }

        switch (p.type) {
            case ParticleType::Leaf:
                p.velocity.x += sin(p.lifeRemaining * 5.0f + p.position.y * 0.5f) * 2.0f * dt;
                break;
            case ParticleType::Smoke:
                p.velocity.y += 0.4f * dt;
                break;
            default: break;
        }

        p.velocity += p.gravity * dt;
        p.position += p.velocity * dt;
        p.rotation += p.angularVelocity * dt;
    }
    return deathEvents;
}

void ParticleSystem::render(Shader& shader, const Luminumbra::Rendering::Camera& camera) {
    if (m_ActiveParticleCount == 0) return;

    // Setup state
    GLCall(glEnable(GL_BLEND));
    GLCall(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GLCall(glDepthMask(GL_FALSE));

    // Bind shader and set uniforms
    shader.use();
    shader.setMat4("u_Projection", camera.getProjectionMatrix());
    shader.setMat4("u_View", camera.getViewMatrix()); 
    shader.setVec3("u_CameraRight", camera.getRight());
    shader.setVec3("u_CameraUp", camera.getUp());
    shader.setVec3("u_ViewPos", camera.getPosition());
    shader.setFloat("u_Time", static_cast<float>(glfwGetTime()));

    // Bind VAO and prepare instance buffer
    GLCall(glBindVertexArray(m_QuadVAO));
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVBO));

    // Prepare instance data on CPU side first
    std::vector<ParticleInstanceData> regularInstances;
    std::vector<ParticleInstanceData> volumetricInstances;
    regularInstances.reserve(m_ActiveParticleCount);
    volumetricInstances.reserve(m_ActiveParticleCount * 12); // Max layers

    // First pass: Regular particles
    for (uint32_t i = 0; i < m_ActiveParticleCount; ++i) {
        const Particle& p = m_ParticlePool[i];
        if (p.isVolumetric) continue;
        
        ParticleInstanceData instance;
        float lifeRatio = glm::clamp(1.0f - (p.lifeRemaining / p.lifeTime), 0.0f, 1.0f);
        instance.color = glm::mix(p.colorBegin, p.colorEnd, lifeRatio);
        instance.size = glm::mix(p.sizeBegin, p.sizeEnd, lifeRatio);
        instance.worldPosition = p.position;
        instance.rotation = p.rotation;
        regularInstances.push_back(instance);
    }

    // Draw regular particles
    if (!regularInstances.empty()) {
        GLCall(glBufferData(GL_ARRAY_BUFFER, regularInstances.size() * sizeof(ParticleInstanceData), 
            regularInstances.data(), GL_DYNAMIC_DRAW));
        GLCall(glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, 
            static_cast<GLsizei>(regularInstances.size())));
    }

    // Second pass: Volumetric particles
    for (uint32_t i = 0; i < m_ActiveParticleCount; ++i) {
        const Particle& p = m_ParticlePool[i];
        if (!p.isVolumetric) continue;

        float lifeRatio = glm::clamp(1.0f - (p.lifeRemaining / p.lifeTime), 0.0f, 1.0f);
        glm::vec4 color = glm::mix(p.colorBegin, p.colorEnd, lifeRatio);
        float size = glm::mix(p.sizeBegin, p.sizeEnd, lifeRatio);

        for (int layer = 0; layer < p.volumetricLayers; layer++) {
            ParticleInstanceData instance;
            float layerRatio = static_cast<float>(layer) / static_cast<float>(p.volumetricLayers);
            instance.color = color;
            instance.size = size;
            instance.worldPosition = p.position + 
                camera.getRight() * (layerRatio - 0.5f) * p.volumeDepth;
            instance.rotation = p.rotation;
            instance.layer = layerRatio;
            volumetricInstances.push_back(instance);
        }
    }

    // Draw volumetric particles
    if (!volumetricInstances.empty()) {
        GLCall(glBufferData(GL_ARRAY_BUFFER, volumetricInstances.size() * sizeof(ParticleInstanceData), 
            volumetricInstances.data(), GL_DYNAMIC_DRAW));
        GLCall(glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, 
            static_cast<GLsizei>(volumetricInstances.size())));
    }

    // Cleanup state
    GLCall(glBindBuffer(GL_ARRAY_BUFFER, 0));
    GLCall(glBindVertexArray(0));
    GLCall(glDepthMask(GL_TRUE));
    GLCall(glDisable(GL_BLEND));
}

ParticleProps ParticleSystem::getPresetProperties(ParticleType type) const {
    ParticleProps props;
    props.type = type;

    switch (type) {
        case ParticleType::Leaf:
            props.velocity = {0.0f, -0.5f, 0.0f};
            props.velocityVariation = {0.3f, 0.2f, 0.3f};
            props.gravity = {0.0f, -0.6f, 0.0f}; // Gentle gravity/drag
            props.colorBegin = {0.2f, 0.8f, 0.1f, 1.0f}; // Green
            props.colorEnd = {0.6f, 0.4f, 0.1f, 0.0f};   // Brown and fade
            props.sizeBegin = 0.2f;
            props.sizeEnd = 0.15f;
            props.lifeTime = 5.0f;
            props.angularVelocityVariation = 3.0f;
            break;

        case ParticleType::Smoke:
            props.velocity = {0.0f, 1.0f, 0.0f};
            props.velocityVariation = {0.4f, 0.2f, 0.4f};
            props.gravity = {0.0f, 0.0f, 0.0f}; // Smoke rises
            props.colorBegin = {0.1f, 0.1f, 0.1f, 0.7f}; // Dark grey
            props.colorEnd = {0.3f, 0.3f, 0.3f, 0.0f};   // Fades out
            props.sizeBegin = 0.4f;
            props.sizeEnd = 2.5f; // Expands significantly
            props.lifeTime = 4.5f;
            props.angularVelocityVariation = 1.0f;
            break;

        case ParticleType::Fire:
            props.velocity = {0.0f, 2.5f, 0.0f};
            props.velocityVariation = {0.8f, 0.8f, 0.8f};
            props.gravity = {0.0f, 0.0f, 0.0f}; // Rises quickly
            props.colorBegin = {1.0f, 1.0f, 0.5f, 1.0f}; // Hot yellow
            props.colorEnd = {0.8f, 0.1f, 0.0f, 0.0f};   // Dark red and fade
            props.sizeBegin = 1.0f;
            props.sizeEnd = 0.0f; // Burns out
            props.lifeTime = 1.2f;
            props.angularVelocityVariation = 2.0f;
            break;

        case ParticleType::Spark:
            props.velocity = {0.0f, 5.0f, 0.0f};
            props.velocityVariation = {6.0f, 3.0f, 6.0f}; // Ejected outwards
            props.gravity = {0.0f, -15.0f, 0.0f}; // Strong gravity
            props.colorBegin = {1.0f, 0.7f, 0.1f, 1.0f}; // Bright orange
            props.colorEnd = {0.4f, 0.1f, 0.0f, 1.0f};   // Dulls but doesn't fade
            props.sizeBegin = 0.1f;
            props.sizeEnd = 0.0f;
            props.lifeTime = 0.8f;
            break;

        case ParticleType::Magic:
            props.velocityVariation = {0.2f, 0.2f, 0.2f}; // Meanders slowly
            props.gravity = {0.0f, 0.0f, 0.0f};
            props.colorBegin = {0.8f, 0.2f, 1.0f, 1.0f}; // Vibrant purple
            props.colorEnd = {0.4f, 0.1f, 0.5f, 0.0f};   // Fades to a darker shade
            props.sizeBegin = 0.2f;
            props.sizeEnd = 0.3f; // Pulses larger
            props.lifeTime = 3.0f;
            props.angularVelocityVariation = 0.5f;
            break;

        case ParticleType::Rain:
            props.velocity = {0.0f, -40.0f, 0.0f}; // Very fast
            props.velocityVariation = {1.0f, 5.0f, 1.0f};
            props.gravity = {0.0f, 0.0f, 0.0f}; // Constant terminal velocity
            props.colorBegin = {0.6f, 0.7f, 1.0f, 0.6f}; // Translucent blue
            props.colorEnd = {0.6f, 0.7f, 1.0f, 0.2f};
            props.sizeBegin = 0.05f;
            props.sizeEnd = 0.05f;
            props.lifeTime = 1.5f;
            break;

        case ParticleType::Splash:
            props.velocityVariation = {3.0f, 0.5f, 3.0f}; // Bursts outwards
            props.gravity = {0.0f, -8.0f, 0.0f}; // Falls back down
            props.colorBegin = {0.6f, 0.7f, 1.0f, 0.8f};
            props.colorEnd = {0.8f, 0.9f, 1.0f, 0.0f};
            props.sizeBegin = 0.04f;
            props.sizeEnd = 0.0f;
            props.lifeTime = 0.4f;
            break;

        case ParticleType::Snow:
            props.velocity = {0.0f, -1.0f, 0.0f}; // Fall slowly
            props.velocityVariation = {0.5f, 0.5f, 0.5f};
            props.gravity = {0.0f, -0.5f, 0.0f}; // Light downward force
            props.colorBegin = {1.0f, 1.0f, 1.0f, 1.0f}; // White
            props.colorEnd = {1.0f, 1.0f, 1.0f, 0.0f};   // Fades out
            props.sizeBegin = 0.1f;
            props.sizeEnd = 0.2f; // Slightly larger
            props.lifeTime = 3.0f;
            break;
        
        case ParticleType::Explosion:
            props.velocity = {0.0f, 0.0f, 0.0f}; // Ejects in all directions
            props.velocityVariation = {10.0f, 10.0f, 10.0f};
            props.gravity = {0.0f, -5.0f, 0.0f}; // Falls back down
            props.colorBegin = {1.0f, 0.5f, 0.1f, 1.0f}; // Bright orange
            props.colorEnd = {1.0f, 0.2f, 0.1f, 0.0f};   // Fades to red
            props.sizeBegin = 1.5f;
            props.sizeEnd = 3.0f; // Expands significantly
            props.lifeTime = 1.5f;
            break;
        
        case ParticleType::Firework:
            props.velocity = {0.0f, 20.0f, 0.0f}; // Fast upward burst
            props.velocityVariation = {5.0f, 5.0f, 5.0f};
            props.gravity = {0.0f, -10.0f, 0.0f}; // Falls back down
            props.colorBegin = {1.0f, 1.0f, 0.2f, 1.0f}; // Bright yellow
            props.colorEnd = {1.0f, 0.2f, 0.2f, 0.0f};   // Fades to red
            props.sizeBegin = 1.2f;
            props.sizeEnd = 2.5f; // Expands significantly
            props.lifeTime = 2.5f;
            break;

        case ParticleType::Cloud:
            props.isVolumetric = true;
            props.volumetricLayers = 24;           // Increased from 12 to 24 for more depth
            props.volumeDepth = 8.0f;             // Doubled from 4.0f for wider clouds
            props.velocity = {0.3f, 0.0f, 0.0f};  // Slowed horizontal movement
            props.velocityVariation = {0.2f, 0.15f, 0.2f};
            props.gravity = {0.0f, 0.0f, 0.0f};
            
            // Base color with slight blue tint for sky reflection
            props.colorBegin = {
                0.95f + s_Distribution(s_RandomEngine) * 0.05f,  // Red
                0.95f + s_Distribution(s_RandomEngine) * 0.05f,  // Green
                1.00f + s_Distribution(s_RandomEngine) * 0.05f,  // Blue (slightly higher)
                0.2f + s_Distribution(s_RandomEngine) * 0.1f     // Variable opacity
            };
            
            // Slightly darker end color
            props.colorEnd = {
                0.85f + s_Distribution(s_RandomEngine) * 0.05f,
                0.85f + s_Distribution(s_RandomEngine) * 0.05f,
                0.90f + s_Distribution(s_RandomEngine) * 0.05f,
                0.0f
            };
            
            props.sizeBegin = 35.0f + s_Distribution(s_RandomEngine) * 10.0f;  // 20.0f -> 35.0f, with variation
            props.sizeEnd = 40.0f + s_Distribution(s_RandomEngine) * 15.0f;    // 25.0f -> 40.0f, with variation
            props.sizeVariation = 5.0f;  // Add size variation
            props.lifeTime = 30.0f;      // Increased from 20.0f for longer-lasting clouds
            break;

        case ParticleType::VolumetricSmoke:
            props.isVolumetric = true;
            props.volumetricLayers = 8;
            props.volumeDepth = 2.0f;
            props.velocity = {0.0f, 1.0f, 0.0f};
            props.velocityVariation = {0.3f, 0.2f, 0.3f};
            props.gravity = {0.0f, 0.0f, 0.0f};
            props.colorBegin = {0.2f, 0.2f, 0.2f, 0.2f};
            props.colorEnd = {0.4f, 0.4f, 0.4f, 0.0f};
            props.sizeBegin = 2.0f;
            props.sizeEnd = 4.0f;
            props.lifeTime = 4.0f;
            break;

        case ParticleType::VolumetricFire:
            props.isVolumetric = true;
            props.volumetricLayers = 10;
            props.volumeDepth = 3.0f;
            props.velocity = {0.0f, 2.0f, 0.0f};
            props.velocityVariation = {1.0f, 1.0f, 1.0f};
            props.gravity = {0.0f, -1.0f, 0.0f};
            props.colorBegin = {1.0f, 0.5f, 0.2f, 1.0f};
            props.colorEnd = {1.0f, 0.2f, 0.1f, 0.0f};
            props.sizeBegin = 1.5f;
            props.sizeEnd = 3.5f;
            props.lifeTime = 3.5f;
            break;
    }
    return props;
}

void ParticleSystem::emit(const ParticleProps& props) {
    if (m_ActiveParticleCount >= m_ParticlePool.size()) return;

    Particle& particle = m_ParticlePool[m_ActiveParticleCount];
    particle.active = true;
    particle.type = props.type;
    particle.position = props.position;
    
    particle.velocity = props.velocity;
    particle.velocity.x += props.velocityVariation.x * s_Distribution(s_RandomEngine);
    particle.velocity.y += props.velocityVariation.y * s_Distribution(s_RandomEngine);
    particle.velocity.z += props.velocityVariation.z * s_Distribution(s_RandomEngine);

    particle.gravity = props.gravity;
    
    particle.angularVelocity = props.angularVelocity + props.angularVelocityVariation * s_Distribution(s_RandomEngine);
    particle.rotation = s_UnitDistribution(s_RandomEngine) * 2.0f * glm::pi<float>();

    particle.colorBegin = props.colorBegin;
    particle.colorEnd = props.colorEnd;

    particle.lifeTime = props.lifeTime;
    particle.lifeRemaining = props.lifeTime;
    
    particle.sizeBegin = props.sizeBegin + props.sizeVariation * s_Distribution(s_RandomEngine);
    particle.sizeEnd = props.sizeEnd;

    particle.isVolumetric = props.isVolumetric;
    particle.volumetricLayers = props.volumetricLayers;
    particle.volumeDepth = props.volumeDepth;

    m_ActiveParticleCount++;
}

} // namespace Luminumbra::Rendering