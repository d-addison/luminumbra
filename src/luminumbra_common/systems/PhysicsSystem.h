#pragma once

// Jolt requires its umbrella header before any component header.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <luminumbra/core/Types.h>

#include <cstddef>
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Luminumbra {

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
    bool is_started() const {
        return m_started;
    }

    // --- World Geometry ---
    void add_chunk_collision(Chunk& chunk);
    void remove_chunk_collision(ChunkID chunk_id);

    // ---  : dynamic rigid bodies (projectiles e.g. an arrow) ---
    // Spawns a small dynamic sphere with an initial velocity: it falls under the
    // world gravity and COLLIDES with the chunk terrain (real ballistic physics,
    // not a hand-integrated arc). Returns the Jolt BodyID handle. Read its position
    // each tick (get_body_position), detect rest via body_is_active==false, and
    // destroy_body to despawn. Server-authoritative (the headless server owns the
    // Jolt world); the resulting transform replicates like any other entity.
    JPH::BodyID
    create_dynamic_sphere(const glm::vec3& position, const glm::vec3& velocity, float radius);
    glm::vec3 get_body_position(JPH::BodyID body) const;
    bool body_is_active(JPH::BodyID body) const;
    void destroy_body(JPH::BodyID body);

    // --- Player Controller ---
    void create_player_controller(const glm::vec3& initial_position);
    void update_player(const glm::vec3& wish_velocity,
                       bool wants_to_jump,
                       float jump_force,
                       float delta_time);
    void set_player_position(const glm::vec3& position);
    glm::vec3 get_player_position() const;
    bool is_player_grounded() const;
    void set_player_crouched(bool is_crouched);
    bool player_has_space_to_stand() const;

    // --- Server-authoritative multiplayer avatars ---
    // The authoritative server gives every connected player's avatar a Jolt
    // CharacterVirtual (capsule) so players collide with the world (and, once
    // props exist, with dynamic props -- the Garry's-Mod model). ADDITIVE: this
    // is a SEPARATE set from the singleton m_player_character the local client
    // uses, so the client path is untouched. Avatars are stepped in INDEX order
    // (player_id order) on the calling thread -> deterministic same-binary, so
    // the avatar positions that fold into the `entities` sub-hash are run==replay
    // reproducible. Per-avatar movement comes from the latest network usercmd;
    // avatars with no input settle against world collision under gravity.
    void clear_avatar_characters();
    std::size_t create_avatar_character(const glm::vec3& initial_position);
    // Set an avatar's horizontal wish velocity (world XZ m/s) for the
    // next step -- the server applies the network usercmd's movement here. Persists
    // until changed (0 by default -> the avatar stands, the without movement input behaviour).
    void set_avatar_wish_velocity(std::size_t index, const glm::vec2& wish_xz);
    // Steps every avatar character by dt: the per-avatar horizontal wish velocity
    // (gravity when airborne, ground-stick when grounded) against the world geometry.
    void update_avatars(float delta_time);
    std::size_t avatar_character_count() const {
        return m_avatar_characters.size();
    }
    glm::vec3 get_avatar_position(std::size_t index) const;
    glm::vec3 get_avatar_velocity(std::size_t index) const;
    bool is_avatar_grounded(std::size_t index) const;

    // --- Audio-Physics Integration ---
    struct AudioRaycastResult {
        bool hit = false;
        float distance = 0.0f;
        glm::vec3 hit_point = {0.0f, 0.0f, 0.0f};
        glm::vec3 surface_normal = {0.0f, 1.0f, 0.0f};
        float material_absorption = 0.1f;
        int material_type = 0; // 0=stone, 1=dirt, 2=grass, etc.
    };

    AudioRaycastResult audio_raycast(const glm::vec3& from, const glm::vec3& to) const;
    float calculate_audio_occlusion(const glm::vec3& source, const glm::vec3& listener) const;
    std::vector<glm::vec3> calculate_audio_reflection_points(const glm::vec3& source,
                                                             const glm::vec3& listener,
                                                             int max_bounces = 3) const;
    float get_material_audio_absorption(int material_type) const;

    // --- Batched Physics Query System ---
    struct BatchedRaycastQuery {
        glm::vec3 from;
        glm::vec3 to;
        std::function<void(const AudioRaycastResult&)> callback;
        int query_id;
        float priority = 1.0f; // Higher priority queries processed first
    };

    struct BatchedRaycastResult {
        AudioRaycastResult result;
        int query_id;
    };

    class BatchedPhysicsQueries {
    public:
        // Queue a raycast to be processed in the next batch
        int QueueRaycast(const glm::vec3& from,
                         const glm::vec3& to,
                         std::function<void(const AudioRaycastResult&)> callback,
                         float priority = 1.0f);

        // Process all queued raycasts in an optimized batch
        void ProcessBatch(const PhysicsSystem* physics_system);

        // Clear completed queries and prepare for next frame
        void ClearCompleted();

        // Get statistics
        std::size_t GetQueuedQueryCount() const {
            return m_queued_queries.size();
        }
        std::size_t GetProcessedQueryCount() const {
            return m_processed_this_frame;
        }

        // Performance tuning
        void SetMaxQueriesPerFrame(std::size_t max_queries) {
            m_max_queries_per_frame = max_queries;
        }

    private:
        std::vector<BatchedRaycastQuery> m_queued_queries;
        std::vector<BatchedRaycastResult> m_results_buffer;
        int m_next_query_id = 1;
        std::size_t m_processed_this_frame = 0;
        std::size_t m_max_queries_per_frame = 64; // Limit to prevent frame spikes

        // Spatial sorting for cache efficiency
        void SortQueriesSpatially(std::vector<BatchedRaycastQuery>& queries);
    };

    // Get the batched query system (for audio and other systems to use)
    BatchedPhysicsQueries& GetBatchedQueries() {
        return m_batched_queries;
    }
    const BatchedPhysicsQueries& GetBatchedQueries() const {
        return m_batched_queries;
    }

private:
    // Jolt core components
    std::unique_ptr<JPH::PhysicsSystem> m_jolt_system;
    std::unique_ptr<JPH::TempAllocator> m_temp_allocator;
    std::unique_ptr<JPH::JobSystem> m_jolt_job_system;

    JPH::BodyInterface* m_body_interface = nullptr;

    // Player-specific objects
    std::unique_ptr<JPH::CharacterVirtual> m_player_character;
    // <<< NEW: Cached player shapes to prevent memory leaks and improve performance
    JPH::Ref<JPH::Shape> m_player_stand_shape;
    JPH::Ref<JPH::Shape> m_player_crouch_shape;

    // Server-authoritative avatar characters (one per connected player)
    // + the shared avatar capsule shape. Stepped in index order for determinism.
    std::vector<std::unique_ptr<JPH::CharacterVirtual>> m_avatar_characters;
    std::vector<glm::vec2> m_avatar_wish; // per-avatar horizontal wish velocity (m/s)
    JPH::Ref<JPH::Shape> m_avatar_shape;

    // Track collision bodies for loaded chunks
    std::unordered_map<ChunkID, ChunkCollisionData> m_chunk_bodies;

    // Batched physics queries
    BatchedPhysicsQueries m_batched_queries;
    bool m_started = false;
};

} // namespace Systems
} // namespace Luminumbra
