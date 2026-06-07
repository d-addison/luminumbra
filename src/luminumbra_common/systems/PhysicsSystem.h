#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h> // <<< NEW: Added for JPH::Ref

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>

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
    std::vector<glm::vec3> calculate_audio_reflection_points(const glm::vec3& source, const glm::vec3& listener, int max_bounces = 3) const;
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
        int QueueRaycast(const glm::vec3& from, const glm::vec3& to, 
                        std::function<void(const AudioRaycastResult&)> callback,
                        float priority = 1.0f);
        
        // Process all queued raycasts in an optimized batch
        void ProcessBatch(const PhysicsSystem* physics_system);
        
        // Clear completed queries and prepare for next frame
        void ClearCompleted();
        
        // Get statistics
        int GetQueuedQueryCount() const { return m_queued_queries.size(); }
        int GetProcessedQueryCount() const { return m_processed_this_frame; }
        
        // Performance tuning
        void SetMaxQueriesPerFrame(int max_queries) { m_max_queries_per_frame = max_queries; }
        
    private:
        std::vector<BatchedRaycastQuery> m_queued_queries;
        std::vector<BatchedRaycastResult> m_results_buffer;
        int m_next_query_id = 1;
        int m_processed_this_frame = 0;
        int m_max_queries_per_frame = 64; // Limit to prevent frame spikes
        
        // Spatial sorting for cache efficiency
        void SortQueriesSpatially(std::vector<BatchedRaycastQuery>& queries);
    };
    
    // Get the batched query system (for audio and other systems to use)
    BatchedPhysicsQueries& GetBatchedQueries() { return m_batched_queries; }
    const BatchedPhysicsQueries& GetBatchedQueries() const { return m_batched_queries; }

private:
    // Jolt core components
    std::unique_ptr<JPH::PhysicsSystem>   m_jolt_system;
    std::unique_ptr<JPH::TempAllocator>   m_temp_allocator;
    std::unique_ptr<JPH::JobSystem>       m_jolt_job_system;

    JPH::BodyInterface* m_body_interface = nullptr;

    // Player-specific objects
    std::unique_ptr<JPH::CharacterVirtual> m_player_character;
    // <<< NEW: Cached player shapes to prevent memory leaks and improve performance
    JPH::Ref<JPH::Shape> m_player_stand_shape;
    JPH::Ref<JPH::Shape> m_player_crouch_shape;

    // Track collision bodies for loaded chunks
    std::unordered_map<ChunkID, ChunkCollisionData> m_chunk_bodies;
    
    // Batched physics queries
    BatchedPhysicsQueries m_batched_queries;
};

} // namespace Systems
} // namespace Luminumbra