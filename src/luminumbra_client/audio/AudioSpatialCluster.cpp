#include "AudioSpatialCluster.h"
#include "../../luminumbra_common/systems/PhysicsSystem.h"
#include "core/Log.h"
#include <algorithm>
#include <cfloat> // FLT_MAX (BuildClusters) — was transitively pulled in via the
                  // (now-removed) MiniaudioManager.h include; make it explicit.
#include <cmath>

namespace Luminumbra::Client {

AudioSpatialCluster::AudioSpatialCluster() {
    m_sources_to_process.reserve(m_max_audio_sources);
    LUMINUMBRA_CORE_INFO("Audio Spatial Clustering System Initialized");
}

AudioSpatialCluster::~AudioSpatialCluster() {
    m_active_sources.clear();
}

void AudioSpatialCluster::Update(const glm::vec3& listener_pos, float delta_time) {
    m_time_since_last_update += delta_time;

    // Update at reduced frequency for performance
    if (m_time_since_last_update < m_update_interval) {
        return;
    }

    m_time_since_last_update = 0.0f;

    // Remove inactive sources
    for (auto it = m_active_sources.begin(); it != m_active_sources.end();) {
        if (!it->second->is_active) {
            it = m_active_sources.erase(it);
        } else {
            ++it;
        }
    }

    // Build list of sources to process (distance culling)
    m_sources_to_process.clear();
    for (auto& [handle, source] : m_active_sources) {
        float distance = glm::distance(source->position, listener_pos);
        if (distance <= MAX_AUDIO_DISTANCE) {
            m_sources_to_process.push_back(source.get());
        } else {
            // Mark distant sources as very quiet
            source->last_calculated_attenuation = 0.0f;
        }
    }

    // Sort by distance for priority processing
    std::sort(m_sources_to_process.begin(),
              m_sources_to_process.end(),
              [&listener_pos](const AudioSource* a, const AudioSource* b) {
                  float dist_a = glm::distance(a->position, listener_pos);
                  float dist_b = glm::distance(b->position, listener_pos);
                  return dist_a < dist_b;
              });

    // Limit processing to max sources
    if (m_sources_to_process.size() > m_max_audio_sources) {
        m_sources_to_process.resize(m_max_audio_sources);
    }

    if (m_clustering_enabled && m_sources_to_process.size() > MAX_SOURCES_PER_CLUSTER) {
        // Use clustering for many sources
        BuildClusters();
        ProcessClusteredAudio(listener_pos);
    } else {
        // Process sources individually for small counts
        for (auto* source : m_sources_to_process) {
            AudioDetailLevel detail = CalculateDetailLevel(*source, listener_pos);
            ApplyLODProcessing(*source, detail, listener_pos);
        }
    }

    // Batch occlusion calculations
    if (m_occlusion_enabled && m_raycast_callback) {
        CalculateBatchedOcclusion(m_sources_to_process, listener_pos);
    }
}

void AudioSpatialCluster::AddAudioSource(u32 event_handle,
                                         const glm::vec3& position,
                                         float volume,
                                         float min_distance,
                                         float max_distance) {
    auto source = std::make_unique<AudioSource>();
    source->position = position;
    source->volume = volume;
    source->min_distance = min_distance;
    source->max_distance = max_distance;
    source->is_active = true;
    source->event_handle = event_handle;
    source->last_calculated_attenuation = 1.0f;

    m_active_sources[event_handle] = std::move(source);
}

void AudioSpatialCluster::RemoveAudioSource(u32 event_handle) {
    auto it = m_active_sources.find(event_handle);
    if (it != m_active_sources.end()) {
        it->second->is_active = false; // Mark for removal on next update
    }
}

void AudioSpatialCluster::UpdateSourcePosition(u32 event_handle, const glm::vec3& position) {
    auto it = m_active_sources.find(event_handle);
    if (it != m_active_sources.end()) {
        it->second->position = position;
    }
}

void AudioSpatialCluster::UpdateSourceVolume(u32 event_handle, float volume) {
    auto it = m_active_sources.find(event_handle);
    if (it != m_active_sources.end()) {
        it->second->volume = volume;
    }
}

void AudioSpatialCluster::BuildClusters() {
    if (m_sources_to_process.empty()) {
        m_root_cluster.reset();
        m_cluster_count = 0;
        return;
    }

    // Calculate bounding box of all sources
    glm::vec3 min_bounds(FLT_MAX);
    glm::vec3 max_bounds(-FLT_MAX);

    for (const auto* source : m_sources_to_process) {
        min_bounds = glm::min(min_bounds, source->position);
        max_bounds = glm::max(max_bounds, source->position);
    }

    // Add padding to bounding box
    glm::vec3 padding(10.0f);
    min_bounds -= padding;
    max_bounds += padding;

    ClusterNode::AABB root_bounds(min_bounds, max_bounds);
    m_root_cluster = std::make_unique<ClusterNode>(root_bounds);
    m_cluster_count = 0;

    BuildRecursive(m_root_cluster.get(), m_sources_to_process, 0);
}

void AudioSpatialCluster::BuildRecursive(ClusterNode* node,
                                         const std::vector<AudioSource*>& sources,
                                         int depth) {
    // Base cases: too few sources or maximum depth reached
    if (sources.size() <= MAX_SOURCES_PER_CLUSTER || depth >= MAX_CLUSTER_DEPTH) {
        node->sources = sources;
        node->is_leaf = true;
        node->cluster_id = m_cluster_count++;
        CalculateClusterProperties(node);

        // Assign cluster ID to sources
        for (auto* source : sources) {
            source->cluster_id = node->cluster_id;
        }
        return;
    }

    // Split the node into 8 octants
    glm::vec3 center = (node->bounds.min + node->bounds.max) * 0.5f;

    // Create child bounds
    ClusterNode::AABB child_bounds[8];
    child_bounds[0] = ClusterNode::AABB(node->bounds.min, center);
    child_bounds[1] = ClusterNode::AABB(glm::vec3(center.x, node->bounds.min.y, node->bounds.min.z),
                                        glm::vec3(node->bounds.max.x, center.y, center.z));
    child_bounds[2] = ClusterNode::AABB(glm::vec3(node->bounds.min.x, center.y, node->bounds.min.z),
                                        glm::vec3(center.x, node->bounds.max.y, center.z));
    child_bounds[3] =
        ClusterNode::AABB(glm::vec3(center.x, center.y, node->bounds.min.z),
                          glm::vec3(node->bounds.max.x, node->bounds.max.y, center.z));
    child_bounds[4] = ClusterNode::AABB(glm::vec3(node->bounds.min.x, node->bounds.min.y, center.z),
                                        glm::vec3(center.x, center.y, node->bounds.max.z));
    child_bounds[5] =
        ClusterNode::AABB(glm::vec3(center.x, node->bounds.min.y, center.z),
                          glm::vec3(node->bounds.max.x, center.y, node->bounds.max.z));
    child_bounds[6] =
        ClusterNode::AABB(glm::vec3(node->bounds.min.x, center.y, center.z),
                          glm::vec3(center.x, node->bounds.max.y, node->bounds.max.z));
    child_bounds[7] = ClusterNode::AABB(center, node->bounds.max);

    // Distribute sources to children
    std::vector<std::vector<AudioSource*>> child_sources(8);

    for (auto* source : sources) {
        int child_index = 0;
        if (source->position.x >= center.x)
            child_index += 1;
        if (source->position.y >= center.y)
            child_index += 2;
        if (source->position.z >= center.z)
            child_index += 4;

        child_sources[child_index].push_back(source);
    }

    // Create children for non-empty octants
    node->is_leaf = false;
    for (int i = 0; i < 8; ++i) {
        if (!child_sources[i].empty()) {
            node->children[i] = std::make_unique<ClusterNode>(child_bounds[i]);
            BuildRecursive(node->children[i].get(), child_sources[i], depth + 1);
        }
    }
}

void AudioSpatialCluster::ProcessClusteredAudio(const glm::vec3& listener_pos) {
    if (m_root_cluster) {
        ProcessClusterRecursive(m_root_cluster.get(), listener_pos);
    }
}

void AudioSpatialCluster::ProcessClusterRecursive(ClusterNode* node,
                                                  const glm::vec3& listener_pos) {
    if (!node)
        return;

    float distance_to_cluster = node->bounds.distance_to(listener_pos);

    if (node->is_leaf) {
        // Process all sources in this leaf cluster
        for (auto* source : node->sources) {
            AudioDetailLevel detail = CalculateDetailLevel(*source, listener_pos);
            ApplyLODProcessing(*source, detail, listener_pos);
        }
    } else {
        // Decide whether to process children individually or as a group
        if (distance_to_cluster > CLUSTER_MERGE_DISTANCE) {
            // Far away - use simplified cluster processing
            // Calculate single attenuation for the whole cluster
            float cluster_distance = glm::distance(node->center_of_mass, listener_pos);
            float cluster_attenuation =
                1.0f / (1.0f + cluster_distance * cluster_distance * 0.0001f);

            // Apply to all sources in this cluster
            for (int i = 0; i < 8; ++i) {
                if (node->children[i]) {
                    ApplyClusterAttenuation(node->children[i].get(), cluster_attenuation);
                }
            }
        } else {
            // Close enough - process children individually
            for (int i = 0; i < 8; ++i) {
                if (node->children[i]) {
                    ProcessClusterRecursive(node->children[i].get(), listener_pos);
                }
            }
        }
    }
}

void AudioSpatialCluster::ApplyClusterAttenuation(ClusterNode* node, float attenuation) {
    if (node->is_leaf) {
        for (auto* source : node->sources) {
            source->last_calculated_attenuation = attenuation;
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            if (node->children[i]) {
                ApplyClusterAttenuation(node->children[i].get(), attenuation);
            }
        }
    }
}

void AudioSpatialCluster::CalculateClusterProperties(ClusterNode* node) {
    if (node->sources.empty())
        return;

    node->combined_volume = 0.0f;
    glm::vec3 weighted_position(0.0f);
    float total_weight = 0.0f;

    for (const auto* source : node->sources) {
        node->combined_volume += source->volume;
        float weight = source->volume;
        weighted_position += source->position * weight;
        total_weight += weight;
    }

    if (total_weight > 0.0f) {
        node->center_of_mass = weighted_position / total_weight;
    }
}

AudioDetailLevel AudioSpatialCluster::CalculateDetailLevel(const AudioSource& source,
                                                           const glm::vec3& listener_pos) {
    float distance = glm::distance(source.position, listener_pos);

    if (distance < PRIORITY_DISTANCE) {
        return AudioDetailLevel::Ultra; // Full processing + effects
    } else if (distance < MEDIUM_DISTANCE) {
        return AudioDetailLevel::High; // Full 3D processing
    } else if (distance < LOW_DISTANCE) {
        return AudioDetailLevel::Medium; // Distance + basic occlusion
    } else {
        return AudioDetailLevel::Low; // Simple distance attenuation only
    }
}

void AudioSpatialCluster::ApplyLODProcessing(AudioSource& source,
                                             AudioDetailLevel detail,
                                             const glm::vec3& listener_pos) {
    float distance = glm::distance(source.position, listener_pos);

    switch (detail) {
        case AudioDetailLevel::Ultra:
            // Distance and geometry occlusion. The audio backend owns native
            // Doppler processing and the environmental bus owns reverb.
            source.last_calculated_attenuation = CalculateAttenuation(source, listener_pos, true);
            break;

        case AudioDetailLevel::High:
            source.last_calculated_attenuation = CalculateAttenuation(source, listener_pos, true);
            break;

        case AudioDetailLevel::Medium:
            // Distance + basic occlusion
            source.last_calculated_attenuation = CalculateAttenuation(source, listener_pos, true);
            break;

        case AudioDetailLevel::Low:
            // Simple distance attenuation only
            source.last_calculated_attenuation = CalculateDistanceAttenuation(source, distance);
            break;

        case AudioDetailLevel::Off:
        default:
            source.last_calculated_attenuation = 0.0f;
            break;
    }
}

float AudioSpatialCluster::CalculateDistanceAttenuation(const AudioSource& source, float distance) {
    if (distance <= source.min_distance)
        return 1.0f;
    if (distance >= source.max_distance)
        return 0.0f;

    // Inverse square law with linear rolloff
    float normalized_distance =
        (distance - source.min_distance) / (source.max_distance - source.min_distance);
    return 1.0f - normalized_distance;
}

float AudioSpatialCluster::CalculateAttenuation(const AudioSource& source,
                                                const glm::vec3& listener_pos,
                                                bool include_occlusion) {
    float distance = glm::distance(source.position, listener_pos);
    float attenuation = CalculateDistanceAttenuation(source, distance);

    // apply occlusion whenever we have SOME occlusion source — a real
    // physics system (raycast against world geometry) OR the mockable callback.
    // When no physics system is set this is byte-identical to the old gate (the
    // callback term is unchanged), so the distance-only fallback is preserved.
    if (include_occlusion && m_occlusion_enabled && (m_physics_system || m_raycast_callback)) {
        float occlusion = CalculateOcclusion(source.position, listener_pos);
        attenuation *= (1.0f - occlusion * 0.8f); // Reduce volume by up to 80% when occluded
    }

    return attenuation;
}

void AudioSpatialCluster::CalculateBatchedOcclusion(const std::vector<AudioSource*>& sources,
                                                    const glm::vec3& listener_pos) {
    // Use new batched physics system if available, otherwise fallback to old system
    if (m_physics_system) {
        // Clear previous pending queries
        m_pending_occlusion_queries.clear();

        // Queue all occlusion queries in the physics system's batch
        auto& batch_queries = m_physics_system->GetBatchedQueries();

        for (auto* source : sources) {
            float distance = glm::distance(source->position, listener_pos);

            // Only calculate occlusion for close sources
            if (distance < MEDIUM_DISTANCE) {
                // Calculate fallback occlusion for immediate use
                float fallback_occlusion =
                    std::clamp(1.0f - (distance / MEDIUM_DISTANCE), 0.0f, 0.8f);

                // Queue the precise occlusion query
                float priority =
                    1.0f / (1.0f + distance * 0.01f); // Closer sources get higher priority

                int query_id = batch_queries.QueueRaycast(
                    source->position,
                    listener_pos,
                    [this, source, listener_pos](
                        const ::Luminumbra::Systems::PhysicsSystem::AudioRaycastResult& result) {
                        // Callback when the raycast completes
                        float occlusion = 0.0f;
                        if (result.hit) {
                            float total_distance =
                                glm::distance(source->position, result.hit_point) +
                                glm::distance(result.hit_point, listener_pos);
                            float direct_distance = glm::distance(source->position, listener_pos);
                            float path_factor = total_distance / direct_distance;

                            occlusion =
                                result.material_absorption * std::min(path_factor - 1.0f, 1.0f);
                        }

                        // Apply the calculated occlusion to the source's attenuation
                        source->last_calculated_attenuation *=
                            (1.0f - std::clamp(occlusion, 0.0f, 0.95f));
                    },
                    priority);

                // Store the query for fallback handling
                m_pending_occlusion_queries.push_back({source, query_id, fallback_occlusion});

                // Apply fallback occlusion immediately for this frame
                source->last_calculated_attenuation *=
                    (1.0f - fallback_occlusion * 0.3f); // Reduced impact until precise result
            }
        }

        return;
    }

    // Fallback to old raycast callback system
    if (!m_raycast_callback)
        return;

    // Process sources in batches to reduce raycast overhead
    const int BATCH_SIZE = 8;

    for (size_t i = 0; i < sources.size(); i += BATCH_SIZE) {
        size_t batch_end = std::min(i + BATCH_SIZE, sources.size());

        for (size_t j = i; j < batch_end; ++j) {
            auto* source = sources[j];

            // Only calculate occlusion for close sources
            float distance = glm::distance(source->position, listener_pos);
            if (distance < MEDIUM_DISTANCE) {
                float hit_distance;
                bool is_occluded = m_raycast_callback(listener_pos, source->position, hit_distance);

                if (is_occluded) {
                    // Apply occlusion factor based on how much of the ray was blocked
                    float occlusion_factor = std::min(hit_distance / distance, 1.0f);
                    source->last_calculated_attenuation *= (1.0f - occlusion_factor * 0.6f);
                }
            }
        }
    }
}

float AudioSpatialCluster::CalculateOcclusion(const glm::vec3& source_pos,
                                              const glm::vec3& listener_pos) const {
    // real geometry occlusion. When a physics system is present, cast a
    // ray through the world from the source to the listener; solid geometry in the
    // path muffles the sound. PhysicsSystem::calculate_audio_occlusion returns 0.0
    // for a clear line of sight and rises toward ~0.95 as blockage/material
    // absorption grows (it fires the primary + offset rays and clamps the result),
    // which is exactly the 0..1 occlusion scalar this function contracts to.
    if (m_physics_system) {
        return m_physics_system->calculate_audio_occlusion(source_pos, listener_pos);
    }

    // Fallback (no physics system): the pre-existing mockable raycast-callback
    // seam. Kept BYTE-IDENTICAL — same ray direction, same hit_distance/total ratio
    // so worlds without a physics system behave exactly as before.
    if (!m_raycast_callback)
        return 0.0f;

    float hit_distance;
    bool hit = m_raycast_callback(listener_pos, source_pos, hit_distance);

    if (!hit)
        return 0.0f;

    float total_distance = glm::distance(listener_pos, source_pos);
    return hit_distance / total_distance;
}

void AudioSpatialCluster::SetPhysicsRaycastCallback(
    std::function<bool(const glm::vec3&, const glm::vec3&, float&)> callback) {
    m_raycast_callback = std::move(callback);
}

} // namespace Luminumbra::Client
