#include "PhysicsSystem.h"
#include "../world/Chunk.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/TriangleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/JobSystemThreadPool.h> // REQUIRED

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

using namespace JPH;

namespace Luminumbra {
namespace Systems {

// Layers
namespace Layers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}

// BroadPhase layers
namespace BroadPhaseLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint32 NUM_LAYERS = 2;
}

// BroadPhase layer interface
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
        switch (static_cast<uint32>(inLayer.GetValue())) {
            case 0: return "NON_MOVING";
            case 1: return "MOVING";
            default: return "UNKNOWN";
        }
    }
#endif

private:
    BroadPhaseLayer m_object_to_broad[Layers::NUM_LAYERS];
};

// Object vs BroadPhase filter
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

// Object layer pair filter
class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
        if (inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING) return false;
        return true;
    }
};

// Accept all broadphase layers
class BroadPhaseLayerFilterAll final : public BroadPhaseLayerFilter {
public:
    bool ShouldCollide(BroadPhaseLayer) const override { return true; }
};

// Accept all object layers
class ObjectLayerFilterAll final : public ObjectLayerFilter {
public:
    bool ShouldCollide(ObjectLayer) const override { return true; }
};

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() { shutdown(); }

void PhysicsSystem::startup() {
    // Register types (once per process)
    JPH::RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();

    // Allocator and Job system
    m_temp_allocator = std::unique_ptr<TempAllocator>(new TempAllocatorImpl(10 * 1024 * 1024));

    const uint num_worker_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    m_jolt_job_system = std::unique_ptr<JobSystem>(new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, num_worker_threads));

    // Physics system
    m_jolt_system = std::make_unique<JPH::PhysicsSystem>();

    // Broadphase and filters
    static BPLayerInterfaceImpl broad_phase_layer_interface;
    static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    static ObjectLayerPairFilterImpl object_vs_object_layer_filter;

    const uint max_bodies = 10240;
    const uint num_body_mutexes = 0;
    const uint max_body_pairs = 10240;
    const uint max_contact_constraints = 10240;

    m_jolt_system->Init(max_bodies, num_body_mutexes, max_body_pairs, max_contact_constraints,
                        broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);

    m_body_interface = &m_jolt_system->GetBodyInterface();

    spdlog::info("Jolt Physics System Initialized.");
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

    // Build triangles for Jolt
    JPH::Array<JPH::Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = verts[indices[i + 0]].position;
        const auto& b = verts[indices[i + 1]].position;
        const auto& c = verts[indices[i + 2]].position;
        triangles.emplace_back(
            JPH::Float3(a.x, a.y, a.z),
            JPH::Float3(b.x, b.y, b.z),
            JPH::Float3(c.x, c.y, c.z)
        );
    }

    // Create MeshShape via settings
    JPH::MeshShapeSettings mesh_settings(triangles);
    auto shape_result = mesh_settings.Create();
    if (shape_result.HasError()) {
        spdlog::error("Failed to create chunk collision mesh: {}", shape_result.GetError().c_str());
        return;
    }
    JPH::Ref<JPH::Shape> shape = shape_result.Get();

    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3::sZero(),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static,
        Layers::NON_MOVING
    );

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

    JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(0.9f /*half-height*/, 0.5f /*radius*/);

    JPH::CharacterVirtualSettings settings;
    settings.mShape = capsule;
    settings.mMass = 70.0f;
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.1f);

    m_player_character = std::make_unique<JPH::CharacterVirtual>(
        &settings, // pointer required
        JPH::RVec3(start_pos.x, start_pos.y, start_pos.z),
        JPH::Quat::sIdentity(),
        m_jolt_system.get()
    );

    spdlog::info("Player controller created.");
}

void PhysicsSystem::update_player(const glm::vec3& movement, float delta_time) {
    if (!m_player_character) return;

    const JPH::Vec3 gravity(0.0f, -9.81f, 0.0f);
    m_player_character->SetLinearVelocity(JPH::Vec3(movement.x, movement.y, movement.z));

    static BroadPhaseLayerFilterAll bp_filter;
    static ObjectLayerFilterAll obj_filter;
    JPH::BodyFilter body_filter;
    JPH::ShapeFilter shape_filter;

    // Pass TempAllocator by reference (dereference unique_ptr)
    m_player_character->Update(delta_time, gravity, bp_filter, obj_filter, body_filter, shape_filter, *m_temp_allocator);
}

glm::mat4 PhysicsSystem::get_player_transform() const {
    if (!m_player_character) return glm::mat4(1.0f);
    const JPH::RMat44 xf = m_player_character->GetWorldTransform();
    const JPH::RVec3 pos = xf.GetTranslation();
    const JPH::Quat rot = xf.GetRotation().GetQuaternion(); // note GetQuaternion()

    const glm::mat4 T = glm::translate(glm::mat4(1.0f),
                                       glm::vec3((float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ()));
    const glm::quat q((float)rot.GetW(), (float)rot.GetX(), (float)rot.GetY(), (float)rot.GetZ());
    const glm::mat4 R = glm::mat4_cast(q);
    return T * R;
}

glm::vec3 PhysicsSystem::get_player_position() const {
    if (!m_player_character) return glm::vec3(0.0f);
    const RVec3 p = m_player_character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

} // namespace Systems
} // namespace Luminumbra