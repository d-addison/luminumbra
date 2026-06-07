#include "PhysicsSystem.h"
#include "../world/Chunk.h"
#include "../../../include/luminumbra/core/Types.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h> 

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include "../core/Log.h"

using namespace JPH;

namespace Luminumbra {
namespace Systems {

// --- Jolt Layer Implementations (Unchanged) ---
namespace Layers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}
namespace BroadPhaseLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint32 NUM_LAYERS = 2;
}
class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        m_object_to_broad[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_object_to_broad[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return m_object_to_broad[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
            case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
            case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:   return "MOVING";
            default: JPH_ASSERT(false); return "INVALID";
        }
    }
#endif
private:
    BroadPhaseLayer m_object_to_broad[Layers::NUM_LAYERS];
};
class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:     return true;
            default:                 return false;
        }
    }
};
class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
        if (inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING) return false;
        return true;
    }
};
class BroadPhaseLayerFilterAll final : public BroadPhaseLayerFilter {
public:
    bool ShouldCollide(BroadPhaseLayer) const override { return true; }
};
class ObjectLayerFilterAll final : public ObjectLayerFilter {
public:
    bool ShouldCollide(ObjectLayer) const override { return true; }
};

// --- PhysicsSystem Implementation (Unchanged parts omitted for brevity) ---
PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() { shutdown(); }

void PhysicsSystem::startup() {
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();
    m_temp_allocator = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    const uint num_worker_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    m_jolt_job_system = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, num_worker_threads);
    m_jolt_system = std::make_unique<JPH::PhysicsSystem>();
    static BPLayerInterfaceImpl broad_phase_layer_interface;
    static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    static ObjectLayerPairFilterImpl object_vs_object_layer_filter;
    m_jolt_system->Init(10240, 0, 10240, 10240, broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);
    m_body_interface = &m_jolt_system->GetBodyInterface();
    LUMINUMBRA_CORE_INFO("Jolt Physics System Initialized.");
}

void PhysicsSystem::shutdown() {
    m_player_character.reset();
    // Properly release the reference-counted shapes
    m_player_stand_shape = nullptr; 
    m_player_crouch_shape = nullptr;
    m_body_interface = nullptr;
    m_jolt_system.reset();
    m_jolt_job_system.reset();
    m_temp_allocator.reset();
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;
}

void PhysicsSystem::update(float delta_time) {
    if (!m_jolt_system) return;
    m_jolt_system->Update(delta_time, 1, m_temp_allocator.get(), m_jolt_job_system.get());
    
    // Process batched physics queries each frame
    m_batched_queries.ProcessBatch(this);
}


void PhysicsSystem::add_chunk_collision(Chunk& chunk) {
    if (!m_body_interface) return;
    
    // TEMPORARY: Disable collision creation to prevent crashes while debugging
    LUMINUMBRA_CORE_WARN("TEMP: Collision creation disabled for chunk ({},{},{})", 
        chunk.get_coords().x, chunk.get_coords().y, chunk.get_coords().z);
    return;
    if (chunk.heightmap_data.empty()) {
        LUMINUMBRA_CORE_WARN("Attempted to add chunk collision for chunk ({}, {}, {}) with no heightmap data.", 
            chunk.get_coords().x, chunk.get_coords().y, chunk.get_coords().z);
        return;
    }

    // 1. Copy the cached heightmap data into a Jolt-compatible array.
    // The resolution of our heightmap includes the +1 padding for seamless chunk borders.
    const int resolution = CHUNK_SIZE_X + 1; 
    JPH::Array<float> height_samples;
    height_samples.resize(resolution * resolution);

    float min_h = 99999.0f;
    float max_h = -99999.0f;

    for (int i = 0; i < resolution * resolution; ++i) {
        float h = chunk.heightmap_data[i];
        height_samples[i] = h;
        if (h < min_h) min_h = h;
        if (h > max_h) max_h = h;
    }
    
    // If the entire surface of the chunk is below its own volume, it's effectively empty space (e.g. a high-altitude air chunk).
    glm::ivec3 cc = chunk.get_coords();
    float chunk_min_y = cc.y * CHUNK_SIZE_Y;
    if (max_h < chunk_min_y) {
        return; // Nothing to collide with
    }

    // 2. Create the HeightFieldShape.
    // The height samples are in absolute world coordinates. We provide Jolt with the
    // world-space position of the heightmap's origin (the corner of the chunk).
    glm::vec3 chunk_base_pos(cc.x * CHUNK_SIZE_X, 0.0f, cc.z * CHUNK_SIZE_Z);
    JPH::HeightFieldShapeSettings shape_settings(height_samples.data(), 
                                                 JPH::Vec3(chunk_base_pos.x, 0.0f, chunk_base_pos.z), 
                                                 JPH::Vec3(1.0f, 1.0f, 1.0f), 
                                                 resolution);
    shape_settings.mBlockSize = 2; // Recommended default for good performance
    
    JPH::ShapeSettings::ShapeResult result = shape_settings.Create();
    if (result.HasError()) {
        LUMINUMBRA_CORE_ERROR("HeightFieldShape error for chunk ({},{},{}): {}", 
            cc.x, cc.y, cc.z, result.GetError());
        return;
    }
    
    // Additional safety check
    if (!result.Get()) {
        LUMINUMBRA_CORE_ERROR("HeightFieldShape creation returned null for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return;
    }
    
    // The position is baked into the shape, so the body itself can be created at the world origin.
    JPH::BodyCreationSettings body_settings(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
    JPH::Body* body = m_body_interface->CreateBody(body_settings);
    m_body_interface->AddBody(body->GetID(), JPH::EActivation::DontActivate);
    m_chunk_bodies[chunk.get_id()] = ChunkCollisionData{ body->GetID() };
}

void PhysicsSystem::remove_chunk_collision(ChunkID id) {
    if (!m_body_interface) return;
    auto it = m_chunk_bodies.find(id);
    if (it == m_chunk_bodies.end()) return;
    m_body_interface->RemoveBody(it->second.body_id);
    m_body_interface->DestroyBody(it->second.body_id);
    m_chunk_bodies.erase(it);
}

void PhysicsSystem::create_player_controller(const glm::vec3& start_pos) {
    if (!m_jolt_system) return;

    // Create and cache shapes using reference-counted pointers
    m_player_stand_shape = JPH::CapsuleShapeSettings(0.9f, 0.4f).Create().Get();
    m_player_crouch_shape = JPH::CapsuleShapeSettings(0.45f, 0.4f).Create().Get();
    
    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_player_stand_shape; // Start with the standing shape
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.1f);
    m_player_character = std::make_unique<JPH::CharacterVirtual>(&settings, JPH::RVec3(start_pos.x, start_pos.y, start_pos.z), JPH::Quat::sIdentity(), m_jolt_system.get());
    LUMINUMBRA_CORE_INFO("Player controller created.");
}

void PhysicsSystem::update_player(const glm::vec3& wish_velocity, bool wants_to_jump, float jump_force, float dt) {
    if (!m_player_character || !m_jolt_system) return;
    
    // Get the character's current state
    JPH::Vec3 current_velocity = m_player_character->GetLinearVelocity();
    bool is_grounded = m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    // Start with the desired horizontal velocity from player input.
    // The vertical component will be calculated next.
    JPH::Vec3 desired_velocity(wish_velocity.x, current_velocity.GetY(), wish_velocity.z);

    if (is_grounded) {
        // The character is on the ground.
        if (wants_to_jump) {
            // Apply jump impulse to the vertical velocity.
            desired_velocity.SetY(jump_force);
        } else {
            // When on the ground and not jumping, reset vertical velocity.
            // A small negative value helps the character "stick" to slopes.
            desired_velocity.SetY(-1.0f);
        }
    } else {
        // The character is in the air. Manually apply gravity.
        // Jolt's Update function does NOT do this for us.
        JPH::Vec3 gravity = m_jolt_system->GetGravity();
        desired_velocity.SetY(current_velocity.GetY() + gravity.GetY() * dt);
    }
    
    // Set the calculated velocity on the character controller.
    m_player_character->SetLinearVelocity(desired_velocity);
    
    // Update the character controller, which will handle movement and collision.
    // The gravity parameter here is used for ground detection, not for applying acceleration.
    m_player_character->Update(dt, m_jolt_system->GetGravity(), BroadPhaseLayerFilterAll(), ObjectLayerFilterAll(), JPH::BodyFilter(), JPH::ShapeFilter(), *m_temp_allocator);
}

void PhysicsSystem::set_player_crouched(bool is_crouched) {
    if (!m_player_character) return;

    // --- REFACTORED: Use cached shapes to avoid memory leaks/reallocation ---
    JPH::Ref<JPH::Shape> target_shape = is_crouched ? m_player_crouch_shape : m_player_stand_shape;
    
    // Only change the shape if it's actually different
    if (m_player_character->GetShape() != target_shape) {
        m_player_character->SetShape(target_shape, 1.5f, BroadPhaseLayerFilterAll(), ObjectLayerFilterAll(), JPH::BodyFilter(), JPH::ShapeFilter(), *m_temp_allocator);
    }
}

void PhysicsSystem::set_player_position(const glm::vec3& position) {
    if (m_player_character) {
        m_player_character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    }
}

glm::vec3 PhysicsSystem::get_player_position() const {
    if (!m_player_character) return glm::vec3(0.0f);
    const JPH::RVec3 p = m_player_character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

bool PhysicsSystem::is_player_grounded() const {
    if (!m_player_character) return false;
    return m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

bool PhysicsSystem::player_has_space_to_stand() const {
    if (!m_player_character) return true;

    // A small, local class that implements Jolt's abstract collector interface.
    // This is the intended use pattern for the library.
    class StandUpCollector final : public CollideShapeCollector
    {
    public:
        StandUpCollector() = default;

        // The virtual function we MUST implement. It's called for each hit.
        virtual void AddHit(const CollideShapeResult& inResult) override
        {
            // A hit was found, so there is no space to stand.
            mHadHit = true;
            // Force the query to stop immediately. This is a critical optimization.
            ForceEarlyOut();
        }

        bool HadHit() const { return mHadHit; }

    private:
        bool mHadHit = false;
    };

    // Create an instance of our custom collector.
    StandUpCollector collector;
    
    // Note: We use the cached m_player_stand_shape here for consistency
    const JPH::Shape* standing_shape = m_player_stand_shape.GetPtr();

    JPH::Mat44 transform = JPH::Mat44::sRotationTranslation(
        m_player_character->GetRotation(),
        m_player_character->GetPosition()
    );

    CollideShapeSettings settings;

    m_jolt_system->GetNarrowPhaseQuery().CollideShape(
        standing_shape,
        JPH::Vec3::sReplicate(1.0f),
        transform,
        settings,
        JPH::RVec3::sZero(),
        collector,
        BroadPhaseLayerFilterAll(),
        ObjectLayerFilterAll(),
        BodyFilter()
    );

    // If our collector had a hit, there's no space to stand.
    return !collector.HadHit();
}

// === AUDIO-PHYSICS INTEGRATION ===

PhysicsSystem::AudioRaycastResult PhysicsSystem::audio_raycast(const glm::vec3& from, const glm::vec3& to) const {
    AudioRaycastResult result;
    
    if (!m_jolt_system) return result;
    
    JPH::Vec3 ray_start(from.x, from.y, from.z);
    JPH::Vec3 ray_direction = JPH::Vec3(to.x, to.y, to.z) - ray_start;
    float ray_length = ray_direction.Length();
    
    if (ray_length < 0.001f) return result; // Too short
    
    ray_direction = ray_direction.Normalized();
    
    JPH::RRayCast ray(ray_start, ray_direction * ray_length);
    JPH::RayCastResult closest_hit;
    
    if (m_jolt_system->GetNarrowPhaseQuery().CastRay(ray, closest_hit, 
                                                   BroadPhaseLayerFilterAll(), 
                                                   ObjectLayerFilterAll(), 
                                                   JPH::BodyFilter())) {
        result.hit = true;
        result.distance = closest_hit.mFraction * ray_length;
        JPH::Vec3 hit_pos = ray_start + ray_direction * result.distance;
        result.hit_point = {hit_pos.GetX(), hit_pos.GetY(), hit_pos.GetZ()};
        
        // RayCastResult doesn't have surface normal - we need to get it differently
        // For now, use a default upward normal (this should be improved with proper surface queries)
        result.surface_normal = glm::vec3(0.0f, 1.0f, 0.0f);
        
        // Determine material type based on hit point (simplified - could be enhanced with material system)
        if (result.hit_point.y < 10.0f) {
            result.material_type = 0; // Stone
            result.material_absorption = 0.15f;
        } else if (result.hit_point.y < 50.0f) {
            result.material_type = 1; // Dirt
            result.material_absorption = 0.25f;
        } else {
            result.material_type = 2; // Grass/vegetation
            result.material_absorption = 0.4f;
        }
    }
    
    return result;
}

float PhysicsSystem::calculate_audio_occlusion(const glm::vec3& source, const glm::vec3& listener) const {
    // Primary line-of-sight check
    AudioRaycastResult primary_ray = audio_raycast(source, listener);
    
    if (!primary_ray.hit) {
        return 0.0f; // Clear line of sight
    }
    
    float total_occlusion = 0.0f;
    float source_listener_distance = glm::distance(source, listener);
    
    // If we hit something, calculate occlusion based on material and geometry
    float obstruction_factor = primary_ray.distance / source_listener_distance;
    total_occlusion += primary_ray.material_absorption * obstruction_factor;
    
    // Additional rays for more accurate occlusion (performance vs accuracy trade-off)
    const std::vector<glm::vec3> offsets = {
        {0.3f, 0.0f, 0.0f}, {-0.3f, 0.0f, 0.0f},  // Left/right
        {0.0f, 0.3f, 0.0f}, {0.0f, -0.3f, 0.0f},   // Up/down
    };
    
    int clear_paths = 0;
    for (const auto& offset : offsets) {
        AudioRaycastResult ray = audio_raycast(source + offset, listener + offset);
        if (!ray.hit) {
            clear_paths++;
        } else {
            float offset_obstruction = ray.distance / glm::distance(source + offset, listener + offset);
            total_occlusion += ray.material_absorption * offset_obstruction * 0.2f; // Reduced weight
        }
    }
    
    // Reduce occlusion if we have alternative paths
    float path_factor = 1.0f - (static_cast<float>(clear_paths) / offsets.size() * 0.6f);
    total_occlusion *= path_factor;
    
    return std::clamp(total_occlusion, 0.0f, 0.95f); // Max 95% occlusion
}

std::vector<glm::vec3> PhysicsSystem::calculate_audio_reflection_points(const glm::vec3& source, const glm::vec3& listener, int max_bounces) const {
    std::vector<glm::vec3> reflection_points;
    
    if (!m_jolt_system || max_bounces <= 0) return reflection_points;
    
    glm::vec3 current_pos = source;
    glm::vec3 target = listener;
    
    for (int bounce = 0; bounce < max_bounces; ++bounce) {
        AudioRaycastResult ray = audio_raycast(current_pos, target);
        
        if (!ray.hit) {
            break; // Direct path found, no more reflections
        }
        
        // Calculate reflection point
        glm::vec3 incident = glm::normalize(ray.hit_point - current_pos);
        glm::vec3 reflected = incident - 2.0f * glm::dot(incident, ray.surface_normal) * ray.surface_normal;
        
        reflection_points.push_back(ray.hit_point);
        
        // Set up for next bounce
        current_pos = ray.hit_point + ray.surface_normal * 0.01f; // Small offset to avoid self-intersection
        
        // Reflect towards listener
        glm::vec3 to_listener = glm::normalize(listener - current_pos);
        target = current_pos + reflected * 10.0f; // Extend reflection ray
        
        // Early termination if reflection quality becomes too poor
        if (ray.material_absorption > 0.8f) break;
    }
    
    return reflection_points;
}

float PhysicsSystem::get_material_audio_absorption(int material_type) const {
    switch (material_type) {
        case 0: return 0.15f; // Stone - hard, reflective
        case 1: return 0.25f; // Dirt - moderate absorption
        case 2: return 0.4f;  // Grass - soft, absorbing
        case 3: return 0.6f;  // Sand - high absorption
        case 4: return 0.8f;  // Fabric/organic - very absorbing
        case 5: return 0.05f; // Metal - highly reflective
        case 6: return 0.9f;  // Water - high absorption for airborne sound
        default: return 0.2f; // Default medium absorption
    }
}

// === BATCHED PHYSICS QUERY SYSTEM ===

int PhysicsSystem::BatchedPhysicsQueries::QueueRaycast(const glm::vec3& from, const glm::vec3& to, 
                                                      std::function<void(const AudioRaycastResult&)> callback,
                                                      float priority) {
    int query_id = m_next_query_id++;
    m_queued_queries.push_back({from, to, callback, query_id, priority});
    return query_id;
}

void PhysicsSystem::BatchedPhysicsQueries::ProcessBatch(const PhysicsSystem* physics_system) {
    if (m_queued_queries.empty()) {
        m_processed_this_frame = 0;
        return;
    }
    
    // Sort queries by priority (higher priority first) and then spatially
    std::sort(m_queued_queries.begin(), m_queued_queries.end(), 
              [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
                  return a.priority > b.priority;
              });
    
    // Limit processing to avoid frame spikes
    int queries_to_process = std::min(static_cast<int>(m_queued_queries.size()), m_max_queries_per_frame);
    
    // Process highest priority queries first
    std::vector<BatchedRaycastQuery> high_priority_queries;
    std::vector<BatchedRaycastQuery> remaining_queries;
    
    for (int i = 0; i < queries_to_process; ++i) {
        high_priority_queries.push_back(m_queued_queries[i]);
    }
    
    for (size_t i = queries_to_process; i < m_queued_queries.size(); ++i) {
        remaining_queries.push_back(m_queued_queries[i]);
    }
    
    // Sort the queries we're processing spatially for better cache performance
    SortQueriesSpatially(high_priority_queries);
    
    m_results_buffer.clear();
    m_results_buffer.reserve(high_priority_queries.size());
    
    // Batch process raycasts
    for (const auto& query : high_priority_queries) {
        AudioRaycastResult result = physics_system->audio_raycast(query.from, query.to);
        m_results_buffer.push_back({result, query.query_id});
        
        // Immediately invoke callback for this result
        if (query.callback) {
            query.callback(result);
        }
    }
    
    m_processed_this_frame = high_priority_queries.size();
    
    // Replace queued queries with remaining ones
    m_queued_queries = std::move(remaining_queries);
}

void PhysicsSystem::BatchedPhysicsQueries::ClearCompleted() {
    m_results_buffer.clear();
    m_processed_this_frame = 0;
    // Note: We don't clear m_queued_queries here as they represent pending work
}

void PhysicsSystem::BatchedPhysicsQueries::SortQueriesSpatially(std::vector<BatchedRaycastQuery>& queries) {
    // Simple spatial sorting based on query start position
    // This improves cache coherency when accessing the physics world
    std::sort(queries.begin(), queries.end(), [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
        // Morton encoding for better spatial locality
        auto morton_encode = [](float x, float y, float z) -> uint64_t {
            // Simple 3D morton encoding (interleave bits)
            uint32_t ix = static_cast<uint32_t>(x * 100.0f) & 0x3FF; // 10 bits
            uint32_t iy = static_cast<uint32_t>(y * 100.0f) & 0x3FF;
            uint32_t iz = static_cast<uint32_t>(z * 100.0f) & 0x3FF;
            
            uint64_t result = 0;
            for (int i = 0; i < 10; ++i) {
                result |= ((ix & (1u << i)) << (2 * i)) |
                         ((iy & (1u << i)) << (2 * i + 1)) |
                         ((iz & (1u << i)) << (2 * i + 2));
            }
            return result;
        };
        
        uint64_t morton_a = morton_encode(a.from.x, a.from.y, a.from.z);
        uint64_t morton_b = morton_encode(b.from.x, b.from.y, b.from.z);
        
        return morton_a < morton_b;
    });
}

} // namespace Systems
} // namespace Luminumbra