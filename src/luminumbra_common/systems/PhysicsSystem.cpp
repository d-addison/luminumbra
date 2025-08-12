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

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
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
}

void PhysicsSystem::add_chunk_collision(Chunk& chunk) {
    if (!m_body_interface) return;
    const auto& verts = chunk.mesh_vertices;
    const auto& indices = chunk.mesh_indices;
    if (verts.empty() || indices.empty()) return;
    JPH::Array<JPH::Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = verts[indices[i + 0]].position;
        const auto& b = verts[indices[i + 1]].position;
        const auto& c = verts[indices[i + 2]].position;
        triangles.emplace_back(JPH::Float3(a.x, a.y, a.z), JPH::Float3(b.x, b.y, b.z), JPH::Float3(c.x, c.y, c.z));
    }
    JPH::MeshShapeSettings mesh_settings(triangles);
    auto shape_result = mesh_settings.Create();
    if (shape_result.HasError()) { LUMINUMBRA_CORE_ERROR("Mesh error: {}", shape_result.GetError()); return; }
    glm::ivec3 cc = chunk.get_coords();
    glm::vec3 base = glm::vec3(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);
    JPH::BodyCreationSettings settings(shape_result.Get(), JPH::RVec3(base.x, base.y, base.z), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
    JPH::Body* body = m_body_interface->CreateBody(settings);
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
    JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(0.9f, 0.4f);
    JPH::CharacterVirtualSettings settings;
    settings.mShape = capsule;
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.1f);
    m_player_character = std::make_unique<JPH::CharacterVirtual>(&settings, JPH::RVec3(start_pos.x, start_pos.y, start_pos.z), JPH::Quat::sIdentity(), m_jolt_system.get());
    LUMINUMBRA_CORE_INFO("Player controller created.");
}

void PhysicsSystem::update_player(const glm::vec3& wish_velocity, bool wants_to_jump, float jump_force, float dt) {
    if (!m_player_character) return;
    
    // Get the character's current state
    JPH::Vec3 current_velocity = m_player_character->GetLinearVelocity();
    bool is_grounded = m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;

    // ===================== FIX STARTS HERE =====================

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

    // ====================== FIX ENDS HERE ======================
    
    // Update the character controller, which will handle movement and collision.
    // The gravity parameter here is used for ground detection, not for applying acceleration.
    m_player_character->Update(dt, m_jolt_system->GetGravity(), BroadPhaseLayerFilterAll(), ObjectLayerFilterAll(), JPH::BodyFilter(), JPH::ShapeFilter(), *m_temp_allocator);
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

void PhysicsSystem::set_player_crouched(bool is_crouched) {
    if (!m_player_character) return;
    float half_height = is_crouched ? 0.45f : 0.9f;
    m_player_character->SetShape(new CapsuleShape(half_height, 0.4f), 1.5f, BroadPhaseLayerFilterAll(), ObjectLayerFilterAll(), JPH::BodyFilter(), JPH::ShapeFilter(), *m_temp_allocator);
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
    
    // Create the shape we want to test (the standing capsule).
    CapsuleShape standing_shape(0.9f, 0.4f);

    // Construct the full transform matrix for the query.
    JPH::Mat44 transform = JPH::Mat44::sRotationTranslation(
        m_player_character->GetRotation(),
        m_player_character->GetPosition()
    );

    // Default collision settings are fine for this query.
    CollideShapeSettings settings;

    m_jolt_system->GetNarrowPhaseQuery().CollideShape(
        &standing_shape,
        JPH::Vec3::sReplicate(1.0f),
        transform,
        settings,
        JPH::RVec3::sZero(),
        collector, // Pass our own collector instance.
        BroadPhaseLayerFilterAll(),
        ObjectLayerFilterAll(),
        BodyFilter()
    );

    // If our collector had a hit, there's no space to stand.
    return !collector.HadHit();
}



} // namespace Systems
} // namespace Luminumbra