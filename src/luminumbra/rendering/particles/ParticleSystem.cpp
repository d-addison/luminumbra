#include "luminumbra/rendering/particles/Particle.h"
#include "luminumbra/core/GLError.h"

#include "luminumbra/world/World.h" 
#include <glad/gl.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <random>
#include <algorithm>

namespace Luminumbra::Rendering {

using Luminumbra::Core::GLClearError;
using Luminumbra::Core::GLCheckError;

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

void ParticleSystem::render(Shader& shader, const glm::mat4& viewMatrix) {
    if (m_ActiveParticleCount == 0) return;

    GLCall(glEnable(GL_BLEND));
    GLCall(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    GLCall(glDepthMask(GL_FALSE));

    shader.use();
    shader.setMat4("u_View", viewMatrix);
    shader.setVec3("u_CameraRight", {viewMatrix[0][0], viewMatrix[1][0], viewMatrix[2][0]});
    shader.setVec3("u_CameraUp", {viewMatrix[0][1], viewMatrix[1][1], viewMatrix[2][1]});

    GLCall(glBindBuffer(GL_ARRAY_BUFFER, m_InstanceVBO));
    ParticleInstanceData* instanceData = (ParticleInstanceData*)glMapBufferRange(GL_ARRAY_BUFFER, 0, m_ActiveParticleCount * sizeof(ParticleInstanceData), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);

    for (uint32_t i = 0; i < m_ActiveParticleCount; ++i) {
        const Particle& p = m_ParticlePool[i];
        
        float lifeRatio = glm::clamp(1.0f - (p.lifeRemaining / p.lifeTime), 0.0f, 1.0f);
        
        instanceData[i].color = glm::mix(p.colorBegin, p.colorEnd, lifeRatio);
        instanceData[i].size = glm::mix(p.sizeBegin, p.sizeEnd, lifeRatio);
        instanceData[i].worldPosition = p.position;
        instanceData[i].rotation = p.rotation;
    }

    GLCall(glUnmapBuffer(GL_ARRAY_BUFFER));

    GLCall(glBindVertexArray(m_QuadVAO));
    GLCall(glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, m_ActiveParticleCount));
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

    m_ActiveParticleCount++;
}

} // namespace Luminumbra::Rendering