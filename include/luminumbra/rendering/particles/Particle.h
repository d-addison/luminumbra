#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <glad/gl.h>
#include "luminumbra/rendering/Shader.h"

namespace Luminumbra::World {
    class World;
}

namespace Luminumbra::Rendering {
    class Camera; 
}

namespace Luminumbra::Rendering {

// Enum for different particle behaviors, allowing for specialized updates.
enum class ParticleType {
    Leaf,
    Smoke,
    Fire,
    Spark,
    Magic,
    Rain,
    Splash,
    Snow,
    Explosion,
    Firework,
    Cloud,
    VolumetricSmoke,
    VolumetricFire,
};

// Represents the state of a single particle in the system.
struct Particle {
    glm::vec3 position      = {0.0f, 0.0f, 0.0f};
    glm::vec3 velocity      = {0.0f, 0.0f, 0.0f};
    glm::vec3 gravity       = {0.0f, 0.0f, 0.0f}; // Per-particle gravity

    glm::vec4 colorBegin    = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 colorEnd      = {1.0f, 1.0f, 1.0f, 1.0f};

    float sizeBegin         = 1.0f, sizeEnd = 0.0f;
    float rotation          = 0.0f;
    float angularVelocity   = 0.0f;

    float lifeTime          = 1.0f;
    float lifeRemaining     = 0.0f;

    ParticleType type       = ParticleType::Leaf;
    bool active             = false;

    // Volumetric properties
    bool isVolumetric       = false;
    int volumetricLayers    = 8; // Number of layers for volumetric particles
    float volumeDepth       = 2.0f; // Depth of the volumetric particle volume
};

struct ParticleDeathEvent {
    ParticleType type;
    glm::vec3 position;
};

// Properties used to spawn a new particle via the emit() function.
struct ParticleProps {
    ParticleType type           = ParticleType::Leaf;
    glm::vec3 position          = {0.0f, 0.0f, 0.0f};
    glm::vec3 velocity          = {0.0f, 0.0f, 0.0f};
    glm::vec3 velocityVariation = {0.0f, 0.0f, 0.0f};
    glm::vec3 gravity           = {0.0f, -9.8f, 0.0f};
    glm::vec4 colorBegin        = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 colorEnd          = {1.0f, 1.0f, 1.0f, 0.0f};
    float sizeBegin             = 0.1f, sizeEnd = 0.0f;
    float sizeVariation         = 0.0f;
    float lifeTime              = 1.0f;
    float angularVelocity       = 0.0f;
    float angularVelocityVariation = 0.0f;
    bool isVolumetric = false;
    int volumetricLayers = 8;
    float volumeDepth = 2.0f; 
};

// Data sent to the GPU for each particle instance. Optimized for size.
struct ParticleInstanceData {
    glm::vec3 worldPosition;
    float size;
    glm::vec4 color;
    float rotation;
    // Padding to ensure vec4 alignment
    float padding[3];
    float layer = 0.0f; // For volumetric particles, layer index
    float lifeRemaining = 0.0f; // Remaining life time of the particle
    ParticleType type = ParticleType::Leaf; // Type of the particle for behavior handling
};

class ParticleSystem {
public:
    ParticleSystem(uint32_t maxParticles = 10000);
    ~ParticleSystem();

    std::vector<ParticleDeathEvent> update(float dt, Luminumbra::World::World& world);
    void render(Shader& shader, const Luminumbra::Rendering::Camera& camera);

    void emit(const ParticleProps& particleProps);
    ParticleProps getPresetProperties(ParticleType type) const;
    uint32_t getActiveParticleCount() const { return m_ActiveParticleCount; }

private:
    void init();

    std::vector<Particle> m_ParticlePool;
    uint32_t m_ActiveParticleCount = 0;

    GLuint m_QuadVAO = 0;
    GLuint m_QuadVBO = 0;
    GLuint m_QuadEBO = 0;
    GLuint m_InstanceVBO = 0;
};

} // namespace Luminumbra::Rendering