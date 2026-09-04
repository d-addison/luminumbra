#include "PhysicsSystem.h"
#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "SHIELD_WorldSystem.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/RegisterTypes.h>

#include "../core/Log.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <mutex>

using namespace JPH;

namespace Luminumbra {
namespace Systems {

// --- Jolt Layer Implementations (Unchanged) ---
namespace Layers {
static constexpr ObjectLayer NON_MOVING = 0;
static constexpr ObjectLayer MOVING = 1;
static constexpr ObjectLayer NUM_LAYERS = 2;
} // namespace Layers
namespace BroadPhaseLayers {
static constexpr BroadPhaseLayer NON_MOVING(0);
static constexpr BroadPhaseLayer MOVING(1);
static constexpr uint32 NUM_LAYERS = 2;
} // namespace BroadPhaseLayers
class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        m_object_to_broad[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_object_to_broad[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return m_object_to_broad[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
            case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
                return "NON_MOVING";
            case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
                return "MOVING";
            default:
                JPH_ASSERT(false);
                return "INVALID";
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
            case Layers::NON_MOVING:
                return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                return false;
        }
    }
};
class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
        if (inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING)
            return false;
        return true;
    }
};
class BroadPhaseLayerFilterAll final : public BroadPhaseLayerFilter {
public:
    bool ShouldCollide(BroadPhaseLayer) const override {
        return true;
    }
};
class ObjectLayerFilterAll final : public ObjectLayerFilter {
public:
    bool ShouldCollide(ObjectLayer) const override {
        return true;
    }
};

namespace {
struct JoltRuntimeState {
    std::mutex mutex;
    std::size_t users = 0;
    bool owns_factory = false;
};

JoltRuntimeState& GetJoltRuntimeState() {
    static JoltRuntimeState state;
    return state;
}

void AcquireJoltRuntime() {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if (state.users == 0 && Factory::sInstance == nullptr) {
        RegisterDefaultAllocator();
        Factory::sInstance = new Factory();
        RegisterTypes();
        state.owns_factory = true;
    }
    ++state.users;
}

void ReleaseJoltRuntime() {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if (--state.users == 0 && state.owns_factory) {
        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
        state.owns_factory = false;
    }
}
} // namespace

// --- PhysicsSystem implementation -------------------------------------------
PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() {
    shutdown();
}

void PhysicsSystem::startup() {
    if (m_started) {
        LUMINUMBRA_CORE_WARN("PhysicsSystem startup requested while already running.");
        return;
    }

    AcquireJoltRuntime();
    m_temp_allocator = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    const uint hardware_threads = std::thread::hardware_concurrency();
    const uint num_worker_threads = hardware_threads > 1 ? hardware_threads - 1 : 1;
    m_jolt_job_system = std::make_unique<JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers, num_worker_threads);
    m_jolt_system = std::make_unique<JPH::PhysicsSystem>();
    static BPLayerInterfaceImpl broad_phase_layer_interface;
    static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    static ObjectLayerPairFilterImpl object_vs_object_layer_filter;
    m_jolt_system->Init(10240,
                        0,
                        10240,
                        10240,
                        broad_phase_layer_interface,
                        object_vs_broadphase_layer_filter,
                        object_vs_object_layer_filter);
    m_body_interface = &m_jolt_system->GetBodyInterface();
    m_started = true;
    LUMINUMBRA_CORE_INFO("Jolt Physics System Initialized.");
}

void PhysicsSystem::shutdown() {
    if (!m_started) {
        return;
    }

    if (m_body_interface) {
        for (const auto& [chunk_id, collision] : m_chunk_bodies) {
            (void)chunk_id;
            m_body_interface->RemoveBody(collision.body_id);
            m_body_interface->DestroyBody(collision.body_id);
        }
    }
    m_chunk_bodies.clear();
    m_player_character.reset();
    m_avatar_characters.clear(); // Release server avatars before the Jolt system.
    // Properly release the reference-counted shapes
    m_player_stand_shape = nullptr;
    m_player_crouch_shape = nullptr;
    m_avatar_shape = nullptr;
    m_body_interface = nullptr;
    m_jolt_system.reset();
    m_jolt_job_system.reset();
    m_temp_allocator.reset();
    ReleaseJoltRuntime();
    m_started = false;
}

void PhysicsSystem::update(float delta_time) {
    if (!m_jolt_system)
        return;
    m_jolt_system->Update(delta_time, 1, m_temp_allocator.get(), m_jolt_job_system.get());

    // Process batched physics queries each frame
    m_batched_queries.ProcessBatch(this);
}

void PhysicsSystem::add_chunk_collision(Chunk& chunk) {
    if (!m_body_interface)
        return;
    if (m_chunk_bodies.find(chunk.get_id()) != m_chunk_bodies.end())
        return;

    if (chunk.heightmap_data.empty()) {
        LUMINUMBRA_CORE_WARN(
            "Attempted to add chunk collision for chunk ({}, {}, {}) with no heightmap data.",
            chunk.get_coords().x,
            chunk.get_coords().y,
            chunk.get_coords().z);
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
        if (h < min_h)
            min_h = h;
        if (h > max_h)
            max_h = h;
    }

    glm::ivec3 cc = chunk.get_coords();
    const float chunk_min_y = static_cast<float>(cc.y) * static_cast<float>(CHUNK_SIZE_Y);
    const float chunk_max_y = static_cast<float>(cc.y + 1) * static_cast<float>(CHUNK_SIZE_Y);
    const int center_index = (CHUNK_SIZE_Z / 2) * resolution + (CHUNK_SIZE_X / 2);
    const float center_height = chunk.heightmap_data[center_index];
    if (center_height < chunk_min_y || center_height >= chunk_max_y) {
        return;
    }

    // 2. Create the HeightFieldShape.
    // The height samples are in absolute world coordinates. We provide Jolt with the
    // world-space position of the heightmap's origin (the corner of the chunk).
    glm::vec3 chunk_base_pos(cc.x * CHUNK_SIZE_X, 0.0f, cc.z * CHUNK_SIZE_Z);
    JPH::HeightFieldShapeSettings shape_settings(
        height_samples.data(),
        JPH::Vec3(chunk_base_pos.x, 0.0f, chunk_base_pos.z),
        JPH::Vec3(1.0f, 1.0f, 1.0f),
        resolution);
    shape_settings.mBlockSize = 2; // Recommended default for good performance

    JPH::ShapeSettings::ShapeResult result = shape_settings.Create();
    if (result.HasError()) {
        LUMINUMBRA_CORE_ERROR(
            "HeightFieldShape error for chunk ({},{},{}): {}", cc.x, cc.y, cc.z, result.GetError());
        return;
    }

    // Additional safety check
    if (!result.Get()) {
        LUMINUMBRA_CORE_ERROR(
            "HeightFieldShape creation returned null for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return;
    }

    // The position is baked into the shape, so the body itself can be created at the world origin.
    JPH::BodyCreationSettings body_settings(result.Get(),
                                            JPH::RVec3::sZero(),
                                            JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static,
                                            Layers::NON_MOVING);
    JPH::Body* body = m_body_interface->CreateBody(body_settings);
    m_body_interface->AddBody(body->GetID(), JPH::EActivation::DontActivate);
    m_chunk_bodies[chunk.get_id()] = ChunkCollisionData{body->GetID()};
}

void PhysicsSystem::remove_chunk_collision(ChunkID id) {
    if (!m_body_interface)
        return;
    auto it = m_chunk_bodies.find(id);
    if (it == m_chunk_bodies.end())
        return;
    m_body_interface->RemoveBody(it->second.body_id);
    m_body_interface->DestroyBody(it->second.body_id);
    m_chunk_bodies.erase(it);
}

void PhysicsSystem::create_player_controller(const glm::vec3& start_pos) {
    if (!m_jolt_system)
        return;

    // Create and cache shapes using reference-counted pointers
    m_player_stand_shape = JPH::CapsuleShapeSettings(0.9f, 0.4f).Create().Get();
    m_player_crouch_shape = JPH::CapsuleShapeSettings(0.45f, 0.4f).Create().Get();

    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_player_stand_shape; // Start with the standing shape
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.1f);
    m_player_character =
        std::make_unique<JPH::CharacterVirtual>(&settings,
                                                JPH::RVec3(start_pos.x, start_pos.y, start_pos.z),
                                                JPH::Quat::sIdentity(),
                                                m_jolt_system.get());
    LUMINUMBRA_CORE_INFO("Player controller created.");
}

void PhysicsSystem::update_player(const glm::vec3& wish_velocity,
                                  bool wants_to_jump,
                                  float jump_force,
                                  float dt) {
    if (!m_player_character || !m_jolt_system)
        return;

    // Get the character's current state
    JPH::Vec3 current_velocity = m_player_character->GetLinearVelocity();
    bool is_grounded =
        m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
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
    //
    // TRAVERSAL-LITE step-up / mantle: use Jolt's ExtendedUpdate
    // instead of the plain Update. ExtendedUpdate runs WalkStairs internally, which
    // robustly lifts the capsule over short ledges (up to mWalkStairsStepUp high)
    // when forward motion is blocked by a low obstacle that has clear space above --
    // exactly the "step up a ledge without jumping" behaviour we want. This is NOT a
    // climb FSM and NOT wall-scaling: mMaxSlopeAngle still blocks steep faces taller
    // than the step, and the lift only triggers when there's horizontal wish motion.
    //
    // All settings are FIXED constants (no wall-clock / no random) so the path stays
    // deterministic. NOTE: this is the CLIENT-ONLY local player controller; the
    // server-authoritative avatars (update_avatars, which fold into the entities
    // sub-hash) are deliberately left on plain Update -> no world_hash change.
    JPH::CharacterVirtual::ExtendedUpdateSettings step_settings;
    // Lift over ledges up to ~0.7 m (knee/waist height) -- mounts short steps and
    // small ledges without a jump. Jolt default is 0.4 m; we raise it for traversal.
    step_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.7f, 0.0f);
    // Match the step-down sweep so the character settles back onto the surface after
    // cresting a step (otherwise it can briefly float off the top of the ledge).
    step_settings.mWalkStairsStepDownExtra = JPH::Vec3(0.0f, -0.7f, 0.0f);
    // Keep the default stick-to-floor sweep so it tracks down the far side of a step.
    // (mStickToFloorStepDown left at its default of {0,-0.5,0}.)
    m_player_character->ExtendedUpdate(dt,
                                       m_jolt_system->GetGravity(),
                                       step_settings,
                                       BroadPhaseLayerFilterAll(),
                                       ObjectLayerFilterAll(),
                                       JPH::BodyFilter(),
                                       JPH::ShapeFilter(),
                                       *m_temp_allocator);
}

void PhysicsSystem::set_player_crouched(bool is_crouched) {
    if (!m_player_character)
        return;

    // --- REFACTORED: Use cached shapes to avoid memory leaks/reallocation ---
    JPH::Ref<JPH::Shape> target_shape = is_crouched ? m_player_crouch_shape : m_player_stand_shape;

    // Only change the shape if it's actually different
    if (m_player_character->GetShape() != target_shape) {
        m_player_character->SetShape(target_shape,
                                     1.5f,
                                     BroadPhaseLayerFilterAll(),
                                     ObjectLayerFilterAll(),
                                     JPH::BodyFilter(),
                                     JPH::ShapeFilter(),
                                     *m_temp_allocator);
    }
}

void PhysicsSystem::set_player_position(const glm::vec3& position) {
    if (m_player_character) {
        m_player_character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    }
}

glm::vec3 PhysicsSystem::get_player_position() const {
    if (!m_player_character)
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_player_character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

bool PhysicsSystem::is_player_grounded() const {
    if (!m_player_character)
        return false;
    return m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

// ---  : dynamic rigid-body projectiles ---
JPH::BodyID PhysicsSystem::create_dynamic_sphere(const glm::vec3& position,
                                                 const glm::vec3& velocity,
                                                 float radius) {
    if (!m_body_interface)
        return JPH::BodyID();
    JPH::BodyCreationSettings settings(new JPH::SphereShape(radius),
                                       JPH::RVec3(position.x, position.y, position.z),
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic,
                                       Layers::MOVING);
    settings.mLinearVelocity = JPH::Vec3(velocity.x, velocity.y, velocity.z);
    // A light, lively projectile: low gravity factor would float it; keep 1.0 so it
    // arcs naturally and rests on the terrain. Continuous collision avoids tunnelling
    // through thin geometry at speed.
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    const JPH::BodyID id = m_body_interface->CreateAndAddBody(settings, JPH::EActivation::Activate);
    return id;
}

glm::vec3 PhysicsSystem::get_body_position(JPH::BodyID body) const {
    if (!m_body_interface || body.IsInvalid())
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_body_interface->GetPosition(body);
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

bool PhysicsSystem::body_is_active(JPH::BodyID body) const {
    if (!m_body_interface || body.IsInvalid())
        return false;
    return m_body_interface->IsActive(body);
}

void PhysicsSystem::destroy_body(JPH::BodyID body) {
    if (!m_body_interface || body.IsInvalid())
        return;
    m_body_interface->RemoveBody(body);
    m_body_interface->DestroyBody(body);
}

// ---  : server-authoritative avatar characters ---
void PhysicsSystem::clear_avatar_characters() {
    m_avatar_characters.clear();
    m_avatar_wish.clear();
}

void PhysicsSystem::set_avatar_wish_velocity(std::size_t index, const glm::vec2& wish_xz) {
    if (index < m_avatar_wish.size())
        m_avatar_wish[index] = wish_xz;
}

std::size_t PhysicsSystem::create_avatar_character(const glm::vec3& start_pos) {
    if (!m_jolt_system)
        return 0;
    // Shared capsule (same dimensions as the standing player) created once.
    if (m_avatar_shape == nullptr) {
        m_avatar_shape = JPH::CapsuleShapeSettings(0.9f, 0.4f).Create().Get();
    }
    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_avatar_shape;
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.1f);
    m_avatar_characters.push_back(
        std::make_unique<JPH::CharacterVirtual>(&settings,
                                                JPH::RVec3(start_pos.x, start_pos.y, start_pos.z),
                                                JPH::Quat::sIdentity(),
                                                m_jolt_system.get()));
    m_avatar_wish.emplace_back(0.0f, 0.0f);
    return m_avatar_characters.size() - 1;
}

void PhysicsSystem::update_avatars(float dt) {
    if (!m_jolt_system)
        return;
    // Step every avatar IN INDEX (player_id) ORDER so the per-character collide-
    // and-slide sequence is deterministic same-binary. : no horizontal input
    // yet -- gravity when airborne, ground-stick when grounded (the avatars fall
    // and settle on the terrain). Mirrors update_player's vertical handling.
    const JPH::Vec3 gravity = m_jolt_system->GetGravity();
    for (std::size_t i = 0; i < m_avatar_characters.size(); ++i) {
        auto& character = m_avatar_characters[i];
        if (!character)
            continue;
        const glm::vec2 wish = i < m_avatar_wish.size() ? m_avatar_wish[i] : glm::vec2(0.0f);
        const JPH::Vec3 current_velocity = character->GetLinearVelocity();
        const bool grounded =
            character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        JPH::Vec3 desired_velocity(wish.x, current_velocity.GetY(), wish.y);
        if (grounded) {
            desired_velocity.SetY(-1.0f); // stick to slopes / ground
        } else {
            desired_velocity.SetY(current_velocity.GetY() + gravity.GetY() * dt);
        }
        character->SetLinearVelocity(desired_velocity);
        character->Update(dt,
                          gravity,
                          BroadPhaseLayerFilterAll(),
                          ObjectLayerFilterAll(),
                          JPH::BodyFilter(),
                          JPH::ShapeFilter(),
                          *m_temp_allocator);
    }
}

glm::vec3 PhysicsSystem::get_avatar_position(std::size_t index) const {
    if (index >= m_avatar_characters.size() || !m_avatar_characters[index])
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_avatar_characters[index]->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

glm::vec3 PhysicsSystem::get_avatar_velocity(std::size_t index) const {
    if (index >= m_avatar_characters.size() || !m_avatar_characters[index])
        return glm::vec3(0.0f);
    const JPH::Vec3 v = m_avatar_characters[index]->GetLinearVelocity();
    return glm::vec3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ());
}

bool PhysicsSystem::is_avatar_grounded(std::size_t index) const {
    if (index >= m_avatar_characters.size() || !m_avatar_characters[index])
        return false;
    return m_avatar_characters[index]->GetGroundState() ==
           JPH::CharacterBase::EGroundState::OnGround;
}

bool PhysicsSystem::player_has_space_to_stand() const {
    if (!m_player_character)
        return true;

    // A small, local class that implements Jolt's abstract collector interface.
    // This is the intended use pattern for the library.
    class StandUpCollector final : public CollideShapeCollector {
    public:
        StandUpCollector() = default;

        // The virtual function we MUST implement. It's called for each hit.
        virtual void AddHit(const CollideShapeResult&) override {
            // A hit was found, so there is no space to stand.
            mHadHit = true;
            // Force the query to stop immediately. This is a critical optimization.
            ForceEarlyOut();
        }

        bool HadHit() const {
            return mHadHit;
        }

    private:
        bool mHadHit = false;
    };

    // Create an instance of our custom collector.
    StandUpCollector collector;

    // Note: We use the cached m_player_stand_shape here for consistency
    const JPH::Shape* standing_shape = m_player_stand_shape.GetPtr();

    JPH::Mat44 transform = JPH::Mat44::sRotationTranslation(m_player_character->GetRotation(),
                                                            m_player_character->GetPosition());

    CollideShapeSettings settings;

    m_jolt_system->GetNarrowPhaseQuery().CollideShape(standing_shape,
                                                      JPH::Vec3::sReplicate(1.0f),
                                                      transform,
                                                      settings,
                                                      JPH::RVec3::sZero(),
                                                      collector,
                                                      BroadPhaseLayerFilterAll(),
                                                      ObjectLayerFilterAll(),
                                                      BodyFilter());

    // If our collector had a hit, there's no space to stand.
    return !collector.HadHit();
}

// ===  INTEGRATION ===

PhysicsSystem::AudioRaycastResult PhysicsSystem::audio_raycast(const glm::vec3& from,
                                                               const glm::vec3& to) const {
    AudioRaycastResult result;

    if (!m_jolt_system)
        return result;

    JPH::Vec3 ray_start(from.x, from.y, from.z);
    JPH::Vec3 ray_direction = JPH::Vec3(to.x, to.y, to.z) - ray_start;
    float ray_length = ray_direction.Length();

    if (ray_length < 0.001f)
        return result; // Too short

    ray_direction = ray_direction.Normalized();

    JPH::RRayCast ray(ray_start, ray_direction * ray_length);
    JPH::RayCastResult closest_hit;

    if (m_jolt_system->GetNarrowPhaseQuery().CastRay(ray,
                                                     closest_hit,
                                                     BroadPhaseLayerFilterAll(),
                                                     ObjectLayerFilterAll(),
                                                     JPH::BodyFilter())) {
        result.hit = true;
        result.distance = closest_hit.mFraction * ray_length;
        JPH::Vec3 hit_pos = ray_start + ray_direction * result.distance;
        result.hit_point = {hit_pos.GetX(), hit_pos.GetY(), hit_pos.GetZ()};

        // Query the hit body for the real surface normal and its object layer.
        // Terrain heightfields live on NON_MOVING; dynamic spheres are the only
        // other bodies (MOVING), so the layer identifies terrain hits.
        bool terrain_hit = false;
        JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(), closest_hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(closest_hit.mSubShapeID2, hit_pos);
            // Opposition invariant: the normal must face AGAINST the incoming
            // ray (load-bearing for from-below casts, where the heightfield's
            // up-facing triangle normal points along the ray).
            if (normal.Dot(ray_direction) > 0.0f) {
                normal = -normal;
            }
            result.surface_normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
            terrain_hit = body.GetObjectLayer() == Layers::NON_MOVING;
        }

        if (terrain_hit && m_world_system) {
            // Terrain hit with the worldgen seam attached: biome-aware surface
            // material. For a heightfield hit the hit y IS the cached terrain
            // height, so no shaped-height recompute is needed.
            const MaterialType material = m_world_system->SurfaceVertexMaterial(
                result.hit_point.x, result.hit_point.z, result.hit_point.y);
            result.material_type = static_cast<int>(material);
            result.material_absorption = get_material_audio_absorption(result.material_type);
        } else if (m_world_system) {
            // Non-terrain (dynamic body): no worldgen material applies; classify
            // as stone.
            result.material_type = static_cast<int>(MaterialType::Stone);
            result.material_absorption = get_material_audio_absorption(result.material_type);
        } else {
            // Legacy Y-band classification (no world system attached, e.g.
            // standalone tests) - byte-identical to the pre-seam behaviour.
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
    }

    return result;
}

float PhysicsSystem::calculate_audio_occlusion(const glm::vec3& source,
                                               const glm::vec3& listener) const {
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
        {0.3f, 0.0f, 0.0f},
        {-0.3f, 0.0f, 0.0f}, // Left/right
        {0.0f, 0.3f, 0.0f},
        {0.0f, -0.3f, 0.0f}, // Up/down
    };

    int clear_paths = 0;
    for (const auto& offset : offsets) {
        AudioRaycastResult ray = audio_raycast(source + offset, listener + offset);
        if (!ray.hit) {
            clear_paths++;
        } else {
            float offset_obstruction =
                ray.distance / glm::distance(source + offset, listener + offset);
            total_occlusion +=
                ray.material_absorption * offset_obstruction * 0.2f; // Reduced weight
        }
    }

    // Reduce occlusion if we have alternative paths
    float path_factor = 1.0f - (static_cast<float>(clear_paths) / offsets.size() * 0.6f);
    total_occlusion *= path_factor;

    return std::clamp(total_occlusion, 0.0f, 0.95f); // Max 95% occlusion
}

std::vector<glm::vec3> PhysicsSystem::calculate_audio_reflection_points(const glm::vec3& source,
                                                                        const glm::vec3& listener,
                                                                        int max_bounces) const {
    std::vector<glm::vec3> reflection_points;

    if (!m_jolt_system || max_bounces <= 0)
        return reflection_points;

    glm::vec3 current_pos = source;
    glm::vec3 target = listener;

    for (int bounce = 0; bounce < max_bounces; ++bounce) {
        AudioRaycastResult ray = audio_raycast(current_pos, target);

        if (!ray.hit) {
            break; // Direct path found, no more reflections
        }

        // Calculate reflection point
        glm::vec3 incident = glm::normalize(ray.hit_point - current_pos);
        glm::vec3 reflected =
            incident - 2.0f * glm::dot(incident, ray.surface_normal) * ray.surface_normal;

        reflection_points.push_back(ray.hit_point);

        // Set up for next bounce
        current_pos =
            ray.hit_point + ray.surface_normal * 0.01f; // Small offset to avoid self-intersection

        target = current_pos + reflected * 10.0f; // Extend reflection ray

        // Early termination if reflection quality becomes too poor
        if (ray.material_absorption > 0.8f)
            break;
    }

    return reflection_points;
}

float PhysicsSystem::get_material_audio_absorption(int material_type) const {
    // Keyed by static_cast<int>(MaterialType) codes (core/Types.h).
    switch (material_type) {
        case static_cast<int>(MaterialType::Stone):
            return 0.15f; // Stone - hard, reflective
        case static_cast<int>(MaterialType::Soil):
            return 0.25f; // Soil - moderate absorption
        case static_cast<int>(MaterialType::Grass):
            return 0.4f; // Grass - soft, absorbing
        case static_cast<int>(MaterialType::Sand):
            return 0.6f; // Sand - high absorption
        case static_cast<int>(MaterialType::Deepslate):
            return 0.1f; // Deepslate - very hard, highly reflective
        case static_cast<int>(MaterialType::LuminCrystal):
            return 0.05f; // LuminCrystal - crystalline, highly reflective
        case static_cast<int>(MaterialType::Water):
            return 0.9f; // Water - high absorption for airborne sound
        default:
            return 0.2f; // Default medium absorption
    }
}

// === BATCHED PHYSICS QUERY SYSTEM ===

int PhysicsSystem::BatchedPhysicsQueries::QueueRaycast(
    const glm::vec3& from,
    const glm::vec3& to,
    std::function<void(const AudioRaycastResult&)> callback,
    float priority) {
    int query_id = m_next_query_id++;
    m_queued_queries.push_back({from, to, std::move(callback), query_id, priority});
    return query_id;
}

void PhysicsSystem::BatchedPhysicsQueries::ProcessBatch(const PhysicsSystem* physics_system) {
    if (m_queued_queries.empty()) {
        m_processed_this_frame = 0;
        return;
    }

    // Sort queries by priority (higher priority first) and then spatially
    std::sort(m_queued_queries.begin(),
              m_queued_queries.end(),
              [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
                  return a.priority > b.priority;
              });

    // Limit processing to avoid frame spikes
    const std::size_t queries_to_process =
        std::min(m_queued_queries.size(), m_max_queries_per_frame);

    // Process highest priority queries first
    std::vector<BatchedRaycastQuery> high_priority_queries;
    std::vector<BatchedRaycastQuery> remaining_queries;

    for (std::size_t i = 0; i < queries_to_process; ++i) {
        high_priority_queries.push_back(m_queued_queries[i]);
    }

    for (std::size_t i = queries_to_process; i < m_queued_queries.size(); ++i) {
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

void PhysicsSystem::BatchedPhysicsQueries::SortQueriesSpatially(
    std::vector<BatchedRaycastQuery>& queries) {
    // Simple spatial sorting based on query start position
    // This improves cache coherency when accessing the physics world
    std::sort(queries.begin(),
              queries.end(),
              [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
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
