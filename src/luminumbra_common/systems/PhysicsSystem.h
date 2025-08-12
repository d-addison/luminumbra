#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace Luminumbra {

using ChunkID = unsigned long long;
class Chunk;

namespace Systems {

struct ChunkCollisionData {
    JPH::BodyID body_id;
};

class PhysicsSystem {
public:
    PhysicsSystem();
    ~PhysicsSystem();

    void startup();
    void shutdown();
    void update(float delta_time);

    // --- World Geometry ---
    void add_chunk_collision(Chunk& chunk);
    void remove_chunk_collision(ChunkID chunk_id);

    // --- Player Controller ---
    void create_player_controller(const glm::vec3& initial_position);
    void update_player(const glm::vec3& wish_velocity, bool wants_to_jump, float jump_force, float delta_time);
    void set_player_position(const glm::vec3& position);
    glm::vec3 get_player_position() const;
    bool is_player_grounded() const;
    void set_player_crouched(bool is_crouched);
    bool player_has_space_to_stand() const;

private:
    // Jolt core components
    std::unique_ptr<JPH::PhysicsSystem>    m_jolt_system;
    std::unique_ptr<JPH::TempAllocator>    m_temp_allocator;
    std::unique_ptr<JPH::JobSystem>        m_jolt_job_system;

    JPH::BodyInterface* m_body_interface = nullptr;

    // Player-specific objects
    std::unique_ptr<JPH::CharacterVirtual> m_player_character;

    // Track collision bodies for loaded chunks
    std::unordered_map<ChunkID, ChunkCollisionData> m_chunk_bodies;
};

} // namespace Systems
} // namespace Luminumbra