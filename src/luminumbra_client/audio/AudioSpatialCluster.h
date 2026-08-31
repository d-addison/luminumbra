#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Systems {
class PhysicsSystem;
}

namespace Luminumbra::Client {

// Forward declarations
class MiniaudioManager;

struct AudioSource {
    glm::vec3 position;
    float volume;
    float min_distance;
    float max_distance;
    bool is_active;
    u32 event_handle; // Reference to MiniaudioManager handle
    float last_calculated_attenuation;
    int cluster_id = -1; // Which cluster this source belongs to
};

struct ClusterNode {
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;

        AABB() = default;
        AABB(const glm::vec3& min, const glm::vec3& max)
            : min(min)
            , max(max) {}

        bool contains(const glm::vec3& point) const {
            return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
                   point.z >= min.z && point.z <= max.z;
        }

        float distance_to(const glm::vec3& point) const {
            glm::vec3 closest = glm::clamp(point, min, max);
            return glm::distance(point, closest);
        }
    };

    AABB bounds;
    std::vector<AudioSource*> sources;
    std::unique_ptr<ClusterNode> children[8]; // Octree
    bool is_leaf = true;

    // Cluster properties
    float combined_volume = 0.0f;
    glm::vec3 center_of_mass = glm::vec3(0.0f);
    int cluster_id = -1;

    ClusterNode() = default;
    ClusterNode(const AABB& bounds)
        : bounds(bounds) {}
};

enum class AudioDetailLevel {
    Off = 0,    // No processing
    Low = 1,    // Simple distance attenuation only
    Medium = 2, // Distance + basic occlusion
    High = 3,   // Full 3D processing
    Ultra = 4   // Full processing + reverb/effects
};

class AudioSpatialCluster {
public:
    AudioSpatialCluster();
    ~AudioSpatialCluster();

    // Main update function - call once per frame
    void Update(const glm::vec3& listener_pos, float delta_time);

    // Source management
    void AddAudioSource(u32 event_handle,
                        const glm::vec3& position,
                        float volume,
                        float min_distance,
                        float max_distance);
    void RemoveAudioSource(u32 event_handle);
    void UpdateSourcePosition(u32 event_handle, const glm::vec3& position);
    void UpdateSourceVolume(u32 event_handle, float volume);

    // Clustering system
    void BuildClusters();
    void ProcessClusteredAudio(const glm::vec3& listener_pos);

    // Occlusion system
    void CalculateBatchedOcclusion(const std::vector<AudioSource*>& sources,
                                   const glm::vec3& listener_pos);
    void SetPhysicsRaycastCallback(
        std::function<bool(const glm::vec3&, const glm::vec3&, float&)> callback);
    void SetPhysicsSystem(::Luminumbra::Systems::PhysicsSystem* physics_system) {
        m_physics_system = physics_system;
    }

    // public, read-only occlusion query (0 = clear line of sight,
    // rising toward ~1 = fully blocked). When a physics system is set it casts a
    // ray through the world geometry (real occlusion); otherwise it uses the
    // mockable raycast callback; with neither it returns 0. Thin wrapper over the
    // internal CalculateOcclusion — exists so tests/diagnostics can probe a
    // source->listener pair without driving the whole Update pipeline.
    float QueryOcclusion(const glm::vec3& source_pos, const glm::vec3& listener_pos) const {
        return CalculateOcclusion(source_pos, listener_pos);
    }

    // Performance tuning
    void SetMaxAudioSources(int max_sources) {
        m_max_audio_sources = max_sources;
    }
    void SetClusteringEnabled(bool enabled) {
        m_clustering_enabled = enabled;
    }
    void SetOcclusionEnabled(bool enabled) {
        m_occlusion_enabled = enabled;
    }

    // Debug information
    int GetActiveSourceCount() const {
        return m_active_sources.size();
    }
    int GetClusterCount() const {
        return m_cluster_count;
    }

private:
    // Audio source storage
    std::unordered_map<u32, std::unique_ptr<AudioSource>> m_active_sources;
    std::vector<AudioSource*> m_sources_to_process; // Cached for performance

    // Clustering system
    std::unique_ptr<ClusterNode> m_root_cluster;
    int m_cluster_count = 0;
    static constexpr int MAX_SOURCES_PER_CLUSTER = 8;
    static constexpr int MAX_CLUSTER_DEPTH = 4;
    static constexpr float CLUSTER_MERGE_DISTANCE = 20.0f;

    // LOD system
    AudioDetailLevel CalculateDetailLevel(const AudioSource& source, const glm::vec3& listener_pos);
    void
    ApplyLODProcessing(AudioSource& source, AudioDetailLevel detail, const glm::vec3& listener_pos);

    // Clustering internals
    void BuildRecursive(ClusterNode* node, const std::vector<AudioSource*>& sources, int depth);
    void ProcessClusterRecursive(ClusterNode* node, const glm::vec3& listener_pos);
    void CalculateClusterProperties(ClusterNode* node);
    void ApplyClusterAttenuation(ClusterNode* node, float attenuation);

    // Occlusion system
    std::function<bool(const glm::vec3&, const glm::vec3&, float&)> m_raycast_callback;
    ::Luminumbra::Systems::PhysicsSystem* m_physics_system = nullptr;
    float CalculateOcclusion(const glm::vec3& source_pos, const glm::vec3& listener_pos) const;
    float CalculateDistanceAttenuation(const AudioSource& source, float distance);
    float CalculateAttenuation(const AudioSource& source,
                               const glm::vec3& listener_pos,
                               bool include_occlusion);

    // Batched occlusion state
    struct PendingOcclusionQuery {
        AudioSource* source;
        int query_id;
        float fallback_occlusion;
    };
    std::vector<PendingOcclusionQuery> m_pending_occlusion_queries;

    // Performance settings
    int m_max_audio_sources = 128;
    bool m_clustering_enabled = true;
    bool m_occlusion_enabled = true;
    float m_update_interval = 1.0f / 30.0f; // Update at 30Hz
    float m_time_since_last_update = 0.0f;

    // Distance-based culling
    static constexpr float MAX_AUDIO_DISTANCE = 500.0f;
    static constexpr float PRIORITY_DISTANCE = 50.0f; // High detail within 50m
    static constexpr float MEDIUM_DISTANCE = 100.0f;  // Medium detail within 100m
    static constexpr float LOW_DISTANCE = 200.0f;     // Low detail within 200m
};

} // namespace Luminumbra::Client
