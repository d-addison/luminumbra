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
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/RegisterTypes.h>

#include "../core/Log.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

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
    Factory* factory = nullptr;
    bool owns_factory = false;
    RefConst<PhysicsMaterial> prior_material;
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    bool allocators_used = false;
    AllocateFunction used_allocate = nullptr;
    ReallocateFunction used_reallocate = nullptr;
    FreeFunction used_free = nullptr;
    AlignedAllocateFunction used_aligned_allocate = nullptr;
    AlignedFreeFunction used_aligned_free = nullptr;
#endif
};

struct JoltRuntimeIdentity {
    Factory* factory;
    bool owned;
};

JoltRuntimeState& GetJoltRuntimeState() {
    static JoltRuntimeState state;
    return state;
}

void CheckJoltAllocators(JoltRuntimeState& state) {
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    const bool any = Allocate || Reallocate || Free || AlignedAllocate || AlignedFree;
    const bool complete = Allocate && Reallocate && Free && AlignedAllocate && AlignedFree;
    if ((any && !complete) ||
        (!any && (state.allocators_used || state.users != 0 || Factory::sInstance != nullptr))) {
        throw std::logic_error("Incomplete Jolt allocator callbacks");
    }
    if (state.allocators_used && any &&
        (Allocate != state.used_allocate || Reallocate != state.used_reallocate ||
         Free != state.used_free || AlignedAllocate != state.used_aligned_allocate ||
         AlignedFree != state.used_aligned_free)) {
        throw std::logic_error("Jolt allocator callbacks changed after use");
    }
    if (!any) {
        RegisterDefaultAllocator();
    }
    // RTTI retains process-static allocations even after registration fails.
    // Never reset these callbacks to null or replace the set already in use.
    state.allocators_used = true;
    state.used_allocate = Allocate;
    state.used_reallocate = Reallocate;
    state.used_free = Free;
    state.used_aligned_allocate = AlignedAllocate;
    state.used_aligned_free = AlignedFree;
#else
    static_cast<void>(state);
#endif
}

JoltRuntimeIdentity AcquireJoltRuntime() {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if ((state.users != 0 && (state.factory == nullptr || state.factory != Factory::sInstance)) ||
        (state.users == 0 && (state.factory != nullptr || state.owns_factory))) {
        throw std::logic_error("Jolt factory ownership changed during a lease");
    }
    if (state.users == (std::numeric_limits<std::size_t>::max)()) {
        throw std::overflow_error("Jolt runtime lease count overflow");
    }
    // Refuse inconsistent external ownership before changing allocator state.
    if (state.users == 0 && Factory::sInstance != nullptr &&
        (PhysicsMaterial::sDefault == nullptr ||
         Factory::sInstance->Find("PhysicsMaterialSimple") == nullptr)) {
        throw std::logic_error("External Jolt factory is not initialized");
    }
    CheckJoltAllocators(state);
    if (state.users == 0) {
        if (Factory::sInstance == nullptr) {
            const auto prior_material = PhysicsMaterial::sDefault;
            auto factory = std::make_unique<Factory>();
            // RegisterTypes requires a published factory. This temporary
            // publication belongs exclusively to this cold acquisition.
            Factory::sInstance = factory.get();
            try {
                RegisterTypes();
            } catch (...) {
                if (Factory::sInstance != factory.get()) {
                    std::terminate();
                }
                PhysicsMaterial::sDefault = prior_material;
                if (Factory::sInstance != factory.get()) {
                    std::terminate();
                }
                Factory::sInstance = nullptr;
                // The local owner destroys both partially populated maps.
                throw;
            }
            state.prior_material = prior_material;
            state.factory = factory.release();
            state.owns_factory = true;
        } else {
            // The external owner must keep its initialized factory and
            // compatible allocators alive until every borrowed lease ends.
            state.factory = Factory::sInstance;
            state.owns_factory = false;
        }
    }
    ++state.users;
    return {state.factory, state.owns_factory};
}

void ReleaseJoltRuntime(JoltRuntimeIdentity identity) noexcept {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if (state.users == 0 || state.factory != identity.factory ||
        state.owns_factory != identity.owned || Factory::sInstance != identity.factory) {
        // A lifetime-contract violation cannot safely unregister any factory.
        std::terminate();
    }
    if (--state.users != 0) {
        return;
    }
    if (state.owns_factory) {
        UnregisterTypes();
        PhysicsMaterial::sDefault = state.prior_material;
        state.prior_material = nullptr;
        if (Factory::sInstance != identity.factory) {
            std::terminate();
        }
        Factory::sInstance = nullptr;
        delete state.factory;
    }
    state.factory = nullptr;
    state.owns_factory = false;
}
} // namespace

class JoltRuntimeLease {
public:
    JoltRuntimeLease() {
        const auto identity = AcquireJoltRuntime();
        m_factory = identity.factory;
        m_owned = identity.owned;
    }
    ~JoltRuntimeLease() {
        ReleaseJoltRuntime({m_factory, m_owned});
    }
    JoltRuntimeLease(const JoltRuntimeLease&) = delete;
    JoltRuntimeLease& operator=(const JoltRuntimeLease&) = delete;

private:
    Factory* m_factory = nullptr;
    bool m_owned = false;
};

namespace {
struct PendingPhysicsResources {
    // Reverse destruction joins/releases jobs before their dependencies and
    // keeps the independent lease alive through every construction failure.
    std::unique_ptr<JoltRuntimeLease> lease;
    std::unique_ptr<TempAllocator> temp;
    std::unique_ptr<JPH::PhysicsSystem> physics;
    std::unique_ptr<JPH::JobSystem> jobs;
};
} // namespace

// --- PhysicsSystem implementation -------------------------------------------
// Only simulation owners enter this fixed-capacity cache. Render admission never
// consumes its body ids, so asynchronous render order cannot reorder solver inputs.
struct PhysicsSystem::SimulationCollisions {
    static constexpr std::size_t kSamples = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1);
    struct Column {
        ChunkID id = 0;
        std::array<float, kSamples> heights{};
        JPH::RefConst<JPH::Shape> shape;
    };
    JPH::RefConst<JPH::Shape> placeholder;
    std::array<Column, kSimulationColliderBudget> published{};
    std::array<Column, kSimulationColliderBudget> staged{};
    std::array<JPH::BodyID, kSimulationColliderBudget> bodies{};
    std::array<JPH::BodyID, kSimulationOwnerBudget> dynamic_owners{};
    std::size_t dynamic_count = 0;
    std::size_t body_count = 0;
    std::size_t published_count = 0;
    std::size_t sampled_count = 0;
    bool ready = false;
    std::uint64_t tick = std::numeric_limits<std::uint64_t>::max();
};

bool PhysicsSystem::simulation_collisions_enabled() const {
    return m_world_system && m_world_system->durable_residency();
}

bool PhysicsSystem::simulation_collisions_ready() const {
    return !simulation_collisions_enabled() || !m_simulation_collisions ||
           m_simulation_collisions->ready;
}

void PhysicsSystem::set_world_system(const SHIELD_WorldSystem* world_system) {
    if (world_system != m_world_system) {
        clear_simulation_collisions();
        m_simulation_collisions.reset();
    }
    m_world_system = world_system;
}

void PhysicsSystem::clear_simulation_collisions() {
    if (!m_simulation_collisions) {
        return;
    }
    auto& state = *m_simulation_collisions;
    for (std::size_t i = 0; i < state.body_count; ++i) {
        if (i < state.published_count) {
            m_body_interface->RemoveBody(state.bodies[i]);
        }
        m_body_interface->DestroyBody(state.bodies[i]);
    }
    // Dynamic owners outlive a terrain clear. Keep their fixed registration table.
    state.body_count = 0;
    state.published_count = 0;
    state.sampled_count = 0;
    state.ready = false;
    for (auto& column : state.published) {
        column.shape = nullptr;
    }
    for (auto& column : state.staged) {
        column.shape = nullptr;
    }
}

std::size_t PhysicsSystem::simulation_collision_sample_count() const {
    return m_simulation_collisions ? m_simulation_collisions->sampled_count : 0;
}

std::uint64_t PhysicsSystem::simulation_collision_hash() const {
    // FNV-1a over specified integer words and float bits, in sorted column order.
    std::uint64_t hash = 14695981039346656037ull;
    const auto fold = [&hash](std::uint64_t word) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (word >> (byte * 8)) & 255u;
            hash *= 1099511628211ull;
        }
    };
    if (m_simulation_collisions) {
        const auto& state = *m_simulation_collisions;
        fold(state.published_count);
        for (std::size_t i = 0; i < state.published_count; ++i) {
            fold(state.published[i].id);
            for (const float height : state.published[i].heights) {
                fold(std::bit_cast<std::uint32_t>(height));
            }
        }
    } else {
        fold(0);
    }
    return hash;
}

bool PhysicsSystem::prepare_simulation_collisions(std::span<const Vec3> anchors,
                                                  std::uint64_t tick) try {
    if (!simulation_collisions_enabled()) {
        return true; // Preserve the legacy path, including body allocation order and hashes.
    }
    if (!m_simulation_collisions) {
        m_simulation_collisions = std::make_unique<SimulationCollisions>();
    }
    auto& state = *m_simulation_collisions;
    if (state.ready && tick != std::numeric_limits<std::uint64_t>::max() && state.tick == tick) {
        return true;
    }
    state.ready = false;
    state.sampled_count = 0;
    if (!m_body_interface || anchors.size() > kSimulationOwnerBudget ||
        m_live_avatar_count > kSimulationOwnerBudget) {
        return false;
    }
    // Bound the inputs as well as the distinct outputs: <=64 owners, <=576 candidates,
    // <=128 sampled columns, <=289 height samples/column. No resident-world traversal.
    std::array<ChunkID, kSimulationOwnerBudget * 9> candidates{};
    std::size_t candidate_count = 0;
    std::size_t owner_count = 0;
    const auto require_anchor = [&](const Vec3& anchor) {
        if (++owner_count > kSimulationOwnerBudget || !std::isfinite(anchor.x) ||
            !std::isfinite(anchor.y) || !std::isfinite(anchor.z)) {
            return false;
        }
        const double cx = std::floor(static_cast<double>(anchor.x) / CHUNK_SIZE_X);
        const double cz = std::floor(static_cast<double>(anchor.z) / CHUNK_SIZE_Z);
        // Leave a complete ring inside the packed coordinate domain before narrowing.
        if (cx <= Chunk::kPackedMinXz || cx >= Chunk::kPackedMaxXz || cz <= Chunk::kPackedMinXz ||
            cz >= Chunk::kPackedMaxXz) {
            return false;
        }
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                candidates[candidate_count++] = Chunk::calculate_id(
                    IVec3(static_cast<i32>(cx) + dx, 0, static_cast<i32>(cz) + dz));
            }
        }
        return true;
    };
    for (const auto& anchor : anchors) {
        if (!require_anchor(anchor)) {
            return false;
        }
    }
    if (m_player_character && !require_anchor(get_player_position())) {
        return false;
    }
    // Creature controllers share this avatar table; frozen owners participate too.
    for (auto index = m_avatar_head; index != kNoAvatar; index = m_avatars[index].next) {
        const auto position = m_avatars[index].character->GetPosition();
        if (!require_anchor(Vec3(static_cast<float>(position.GetX()),
                                 static_cast<float>(position.GetY()),
                                 static_cast<float>(position.GetZ())))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < state.dynamic_count; ++i) {
        if (!require_anchor(get_body_position(state.dynamic_owners[i]))) {
            return false;
        }
    }
    const auto candidate_end = candidates.begin() + static_cast<std::ptrdiff_t>(candidate_count);
    std::sort(candidates.begin(), candidate_end);
    const auto unique_end = std::unique(candidates.begin(), candidate_end);
    const auto count = static_cast<std::size_t>(unique_end - candidates.begin());
    if (count > kSimulationColliderBudget) {
        return false;
    }
    // Everything that can refuse happens before publication. Staged shapes have fixed
    // slots and replace old references; steady ticks allocate nothing. Changed geometry
    // holds at most two sets of 128 shapes, independent of world size or tick count.
    for (std::size_t i = 0; i < count; ++i) {
        auto& staged = state.staged[i];
        staged.id = candidates[i];
        const auto coords = Chunk::decode_id(staged.id);
        ++state.sampled_count;
        if (!m_world_system->sample_simulation_collision(coords.x, coords.z, staged.heights)) {
            return false;
        }
        const auto& published = state.published[i];
        if (i < state.published_count && staged.id == published.id &&
            std::equal(staged.heights.begin(),
                       staged.heights.end(),
                       published.heights.begin(),
                       [](float left, float right) {
                           return std::bit_cast<std::uint32_t>(left) ==
                                  std::bit_cast<std::uint32_t>(right);
                       })) {
            staged.shape = published.shape;
            continue;
        }
        JPH::HeightFieldShapeSettings settings(staged.heights.data(),
                                               JPH::Vec3::sZero(),
                                               JPH::Vec3::sReplicate(1.0f),
                                               CHUNK_SIZE_X + 1);
        settings.mBlockSize = 2;
        const auto result = settings.Create();
        if (result.HasError() || !result.Get()) {
            return false;
        }
        staged.shape = result.Get();
    }
    if (count != 0 && state.placeholder == nullptr) {
        state.placeholder = new JPH::SphereShape(0.01f);
    }
    // Reserve stable body ids once. These bodies remain outside the broadphase until
    // the entire horizon succeeds. A pool refusal retains already reserved slots.
    while (state.body_count < kSimulationColliderBudget && count != 0) {
        const JPH::BodyCreationSettings settings(state.placeholder.GetPtr(),
                                                 JPH::RVec3::sZero(),
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        auto* body = m_body_interface->CreateBody(settings);
        if (!body) {
            return false;
        }
        state.bodies[state.body_count++] = body->GetID();
    }
    // Publication cannot refuse. Physics waits on any failure above, on every host;
    // it never advances against a partially prepared horizon or render-only bodies.
    for (std::size_t i = 0; i < state.published_count; ++i) {
        if (i >= count || state.published[i].id != state.staged[i].id ||
            state.published[i].shape != state.staged[i].shape) {
            m_body_interface->RemoveBody(state.bodies[i]);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i < state.published_count && state.published[i].id == state.staged[i].id &&
            state.published[i].shape == state.staged[i].shape) {
            continue;
        }
        const auto coords = Chunk::decode_id(state.staged[i].id);
        m_body_interface->SetShape(
            state.bodies[i], state.staged[i].shape.GetPtr(), false, JPH::EActivation::DontActivate);
        m_body_interface->SetPosition(state.bodies[i],
                                      JPH::RVec3(static_cast<float>(coords.x * CHUNK_SIZE_X),
                                                 0.0f,
                                                 static_cast<float>(coords.z * CHUNK_SIZE_Z)),
                                      JPH::EActivation::DontActivate);
        m_body_interface->AddBody(state.bodies[i], JPH::EActivation::DontActivate);
        state.published[i] = state.staged[i];
    }
    for (std::size_t i = count; i < state.published_count; ++i) {
        m_body_interface->SetShape(
            state.bodies[i], state.placeholder.GetPtr(), false, JPH::EActivation::DontActivate);
        state.published[i].shape = nullptr;
    }
    for (auto& staged : state.staged) {
        staged.shape = nullptr;
    }
    state.published_count = count;
    state.tick = tick;
    state.ready = true;
    return true;
} catch (const std::exception&) {
    // Allocation/shape preparation failures have the same no-publication policy.
    return false;
}

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() {
    shutdown();
}

void PhysicsSystem::startup() {
    if (m_started) {
        LUMINUMBRA_CORE_WARN("PhysicsSystem startup requested while already running.");
        return;
    }

    PendingPhysicsResources pending;
    pending.lease = std::make_unique<JoltRuntimeLease>();
    pending.temp = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    pending.physics = std::make_unique<JPH::PhysicsSystem>();
    static BPLayerInterfaceImpl broad_phase_layer_interface;
    static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    static ObjectLayerPairFilterImpl object_vs_object_layer_filter;
    pending.physics->Init(10240,
                          0,
                          10240,
                          10240,
                          broad_phase_layer_interface,
                          object_vs_broadphase_layer_filter,
                          object_vs_object_layer_filter);
    const uint hardware_threads = std::thread::hardware_concurrency();
    const uint num_worker_threads = hardware_threads > 1 ? hardware_threads - 1 : 1;
    pending.jobs = std::make_unique<JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers, num_worker_threads);
    auto* const body_interface = &pending.physics->GetBodyInterface();
    LUMINUMBRA_CORE_INFO("Jolt Physics System Initialized.");

    // Publication is nonthrowing; no borrowed interface escapes preparation.
    m_runtime_lease.swap(pending.lease);
    m_temp_allocator.swap(pending.temp);
    m_jolt_system.swap(pending.physics);
    m_jolt_job_system.swap(pending.jobs);
    m_body_interface = body_interface;
    m_started = true;
}

void PhysicsSystem::shutdown() {
    if (!m_started) {
        return;
    }

    clear_simulation_collisions();
    m_simulation_collisions.reset();

    // Owner-thread lifecycle: no update or new producer may overlap shutdown.
    // Barrier completion alone does not release queue-owned job captures.
    m_jolt_job_system.reset();
    if (m_body_interface) {
        for (const auto& [chunk_id, collision] : m_chunk_bodies) {
            (void)chunk_id;
            m_body_interface->RemoveBody(collision.body_id);
            m_body_interface->DestroyBody(collision.body_id);
        }
    }
    m_chunk_bodies.clear();
    m_player_character.reset();
    clear_avatar_characters(); // Release server avatars before the Jolt system.
    // Properly release the reference-counted shapes
    m_player_stand_shape = nullptr;
    m_player_crouch_shape = nullptr;
    m_avatar_shape = nullptr;
    m_body_interface = nullptr;
    m_jolt_system.reset();
    m_temp_allocator.reset();
    m_runtime_lease.reset();
    m_started = false;
}

void PhysicsSystem::update(float delta_time) {
    m_work_items = {};
    if (!m_jolt_system || !simulation_collisions_ready())
        return;
    m_work_items.bodies = m_jolt_system->GetNumBodies();
    m_jolt_system->Update(delta_time, 1, m_temp_allocator.get(), m_jolt_job_system.get());

    // Process batched physics queries each frame
    m_batched_queries.ProcessBatch(this);
}

bool PhysicsSystem::add_chunk_collision(Chunk& chunk) {
    if (simulation_collisions_enabled()) {
        return true; // Render/warm-up/edit calls cannot mutate simulation's private bodies.
    }
    if (!m_body_interface)
        return true;
    if (m_chunk_bodies.find(chunk.get_id()) != m_chunk_bodies.end())
        return true;

    constexpr std::size_t kHeightmapSide = CHUNK_SIZE_X + 1;
    if (chunk.heightmap_data.size() != kHeightmapSide * kHeightmapSide) {
        LUMINUMBRA_CORE_WARN("Attempted to add chunk collision for chunk ({}, {}, {}) with "
                             "incomplete heightmap data.",
                             chunk.get_coords().x,
                             chunk.get_coords().y,
                             chunk.get_coords().z);
        return true;
    }

    // 1. Copy the cached heightmap data into a Jolt-compatible array.
    // The resolution of our heightmap includes the +1 padding for seamless chunk borders.
    const int resolution = CHUNK_SIZE_X + 1;
    JPH::Array<float> height_samples;
    height_samples.resize(resolution * resolution);

    float min_h = 99999.0f;
    float max_h = -99999.0f;

    for (int i = 0; i < resolution * resolution; ++i) {
        float h = chunk.heightmap_data[static_cast<std::size_t>(i)];
        height_samples[static_cast<std::size_t>(i)] = h;
        if (h < min_h)
            min_h = h;
        if (h > max_h)
            max_h = h;
    }

    glm::ivec3 cc = chunk.get_coords();
    const float chunk_min_y = static_cast<float>(cc.y) * static_cast<float>(CHUNK_SIZE_Y);
    const float chunk_max_y = static_cast<float>(cc.y + 1) * static_cast<float>(CHUNK_SIZE_Y);
    const int center_index = (CHUNK_SIZE_Z / 2) * resolution + (CHUNK_SIZE_X / 2);
    const float center_height = chunk.heightmap_data[static_cast<std::size_t>(center_index)];
    if (center_height < chunk_min_y || center_height >= chunk_max_y) {
        return true;
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
        return true;
    }

    // Additional safety check
    if (!result.Get()) {
        LUMINUMBRA_CORE_ERROR(
            "HeightFieldShape creation returned null for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return true;
    }

    // The position is baked into the shape, so the body itself can be created at the world origin.
    JPH::BodyCreationSettings body_settings(result.Get(),
                                            JPH::RVec3::sZero(),
                                            JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static,
                                            Layers::NON_MOVING);
    JPH::Body* body = m_body_interface->CreateBody(body_settings);
    if (body == nullptr) {
        LUMINUMBRA_CORE_WARN(
            "Body pool refused chunk collision for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return false;
    }
    m_body_interface->AddBody(body->GetID(), JPH::EActivation::DontActivate);
    m_chunk_bodies[chunk.get_id()] = ChunkCollisionData{body->GetID()};
    return true;
}

void PhysicsSystem::remove_chunk_collision(ChunkID id) {
    if (simulation_collisions_enabled()) {
        return;
    }
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

    // The local controller passes feet positions. Keep both capsule bases at that
    // origin so spawning and changing stance cannot embed the capsule in terrain.
    const JPH::Ref<JPH::Shape> standing_capsule =
        JPH::CapsuleShapeSettings(0.9f, 0.4f).Create().Get();
    const JPH::Ref<JPH::Shape> crouching_capsule =
        JPH::CapsuleShapeSettings(0.45f, 0.4f).Create().Get();
    m_player_stand_shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, 1.3f, 0.0f),
                                                               JPH::Quat::sIdentity(),
                                                               standing_capsule.GetPtr())
                               .Create()
                               .Get();
    m_player_crouch_shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, 0.85f, 0.0f),
                                                                JPH::Quat::sIdentity(),
                                                                crouching_capsule.GetPtr())
                                .Create()
                                .Get();

    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_player_stand_shape; // Start with the standing shape
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    // Supporting contacts are relative to the feet, and may touch the lower
    // sphere above its bottom when standing on a walkable slope.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.4f);
    m_player_character =
        std::make_unique<JPH::CharacterVirtual>(&settings,
                                                JPH::RVec3(start_pos.x, start_pos.y, start_pos.z),
                                                JPH::Quat::sIdentity(),
                                                m_jolt_system.get());
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    LUMINUMBRA_CORE_INFO("Player controller created.");
}

void PhysicsSystem::update_player(const glm::vec3& wish_velocity,
                                  bool wants_to_jump,
                                  float jump_force,
                                  float dt) {
    if (!m_player_character || !m_jolt_system)
        return;
    // A teleport can occur inside the input driver after the host's frame barrier.
    // Prepare its destination before any character velocity or position changes.
    if (!simulation_collisions_ready() && !prepare_simulation_collisions({})) {
        return;
    }

    ++m_work_items.characters;

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

bool PhysicsSystem::set_player_crouched(bool is_crouched) {
    if (!m_player_character || !m_jolt_system)
        return false;

    // --- REFACTORED: Use cached shapes to avoid memory leaks/reallocation ---
    JPH::Ref<JPH::Shape> target_shape = is_crouched ? m_player_crouch_shape : m_player_stand_shape;

    // Only change the shape if it's actually different
    if (m_player_character->GetShape() != target_shape) {
        return m_player_character->SetShape(
            target_shape,
            1.5f * m_jolt_system->GetPhysicsSettings().mPenetrationSlop,
            BroadPhaseLayerFilterAll(),
            ObjectLayerFilterAll(),
            JPH::BodyFilter(),
            JPH::ShapeFilter(),
            *m_temp_allocator);
    }
    return true;
}

void PhysicsSystem::set_player_position(const glm::vec3& position) {
    if (m_player_character) {
        m_player_character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
        if (m_simulation_collisions) {
            m_simulation_collisions->ready = false;
        }
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
    if (simulation_collisions_enabled()) {
        if (!m_simulation_collisions) {
            m_simulation_collisions = std::make_unique<SimulationCollisions>();
        }
        if (m_simulation_collisions->dynamic_count == kSimulationOwnerBudget) {
            return JPH::BodyID();
        }
    }
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
    if (simulation_collisions_enabled() && !id.IsInvalid()) {
        auto& state = *m_simulation_collisions;
        state.dynamic_owners[state.dynamic_count++] = id;
        state.ready = false;
    }
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
    if (m_simulation_collisions) {
        auto& state = *m_simulation_collisions;
        for (std::size_t i = 0; i < state.dynamic_count; ++i) {
            if (state.dynamic_owners[i] == body) {
                state.dynamic_owners[i] = state.dynamic_owners[--state.dynamic_count];
                break;
            }
        }
    }
}

// ---  : server-authoritative avatar characters ---
void PhysicsSystem::clear_avatar_characters() {
    for (std::size_t index = 0; index < m_avatars.size(); ++index) {
        remove_avatar_character(index);
    }
    m_avatars.clear();
}

void PhysicsSystem::remove_avatar_character(std::size_t index) {
    if (index >= m_avatars.size() || !m_avatars[index].character) {
        return;
    }
    const auto previous = m_avatars[index].previous;
    const auto next = m_avatars[index].next;
    if (previous != kNoAvatar) {
        m_avatars[previous].next = next;
    } else {
        m_avatar_head = next;
    }
    if (next != kNoAvatar) {
        m_avatars[next].previous = previous;
    } else {
        m_avatar_tail = previous;
    }
    --m_live_avatar_count;
    destroy_body(m_avatars[index].frozen_body);
    m_avatars[index] = AvatarState{};
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
}

void PhysicsSystem::set_avatar_frozen(std::size_t index, bool frozen, const glm::vec3& position) {
    if (index >= m_avatars.size() || !m_avatars[index].character) {
        return;
    }
    auto& avatar = m_avatars[index];
    if (!frozen && !avatar.frozen) {
        return;
    }
    auto& body = avatar.frozen_body;
    auto& character = *avatar.character;
    const JPH::RVec3 feet =
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
            ? JPH::RVec3(position.x, position.y, position.z)
            : character.GetPosition();
    if (frozen && body.IsInvalid()) {
        const JPH::BodyCreationSettings settings(m_avatar_shape,
                                                 feet,
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        // Count admission is O(1), in the caller's existing entity order. Leave
        // terrain headroom even after this capsule takes a slot. Both headroom
        // refusal and a null CreateBody leave an invalid ID for the next retry.
        const auto max_bodies = m_jolt_system->GetMaxBodies();
        if (max_bodies > kTerrainBodyHeadroom &&
            m_jolt_system->GetNumBodies() < max_bodies - kTerrainBodyHeadroom) {
            body = m_body_interface->CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        }
    } else if (!frozen) {
        destroy_body(body);
        body = JPH::BodyID();
    } else if (feet != character.GetPosition()) {
        m_body_interface->SetPosition(body, feet, JPH::EActivation::DontActivate);
    }
    avatar.frozen = frozen;
    character.SetPosition(feet);
    character.SetLinearVelocity(JPH::Vec3::sZero());
    avatar.wish = glm::vec2(0.0f);
}

void PhysicsSystem::set_avatar_wish_velocity(std::size_t index, const glm::vec2& wish_xz) {
    if (index < m_avatars.size()) {
        m_avatars[index].wish = m_avatars[index].frozen ? glm::vec2(0.0f) : wish_xz;
    }
}

std::size_t PhysicsSystem::create_avatar_character(const glm::vec3& start_pos) {
    if (!m_jolt_system)
        return 0;
    // Shared capsule (same dimensions as the standing player) created once.
    if (m_avatar_shape == nullptr) {
        const JPH::Ref<JPH::Shape> capsule = JPH::CapsuleShapeSettings(0.9f, 0.4f).Create().Get();
        // Character positions and replicated creature transforms are feet.
        // A centered capsule starts below the floor and may resolve through it.
        m_avatar_shape = JPH::RotatedTranslatedShapeSettings(
                             JPH::Vec3(0.0f, 1.3f, 0.0f), JPH::Quat::sIdentity(), capsule.GetPtr())
                             .Create()
                             .Get();
    }
    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_avatar_shape;
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.4f);
    m_avatars.push_back({.character = std::make_unique<JPH::CharacterVirtual>(
                             &settings,
                             JPH::RVec3(start_pos.x, start_pos.y, start_pos.z),
                             JPH::Quat::sIdentity(),
                             m_jolt_system.get()),
                         .wish = glm::vec2(0.0f),
                         .frozen_body = JPH::BodyID(),
                         .frozen = false,
                         .previous = m_avatar_tail,
                         .next = kNoAvatar});
    const auto index = m_avatars.size() - 1;
    if (m_avatar_tail != kNoAvatar) {
        m_avatars[m_avatar_tail].next = index;
    } else {
        m_avatar_head = index;
    }
    m_avatar_tail = index;
    ++m_live_avatar_count;
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    return index;
}

void PhysicsSystem::update_avatars(float dt) {
    if (!m_jolt_system || !simulation_collisions_ready())
        return;
    // Step every avatar IN INDEX (player_id) ORDER so the per-character collide-
    // and-slide sequence is deterministic same-binary. : no horizontal input
    // yet -- gravity when airborne, ground-stick when grounded (the avatars fall
    // and settle on the terrain). Mirrors update_player's vertical handling.
    const JPH::Vec3 gravity = m_jolt_system->GetGravity();
    for (std::size_t i = 0; i < m_avatars.size(); ++i) {
        auto& character = m_avatars[i].character;
        if (!character || m_avatars[i].frozen)
            continue;
        ++m_work_items.characters;
        const glm::vec2 wish = m_avatars[i].wish;
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
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_avatars[index].character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

glm::vec3 PhysicsSystem::get_avatar_velocity(std::size_t index) const {
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return glm::vec3(0.0f);
    const JPH::Vec3 v = m_avatars[index].character->GetLinearVelocity();
    return glm::vec3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ());
}

bool PhysicsSystem::is_avatar_grounded(std::size_t index) const {
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return false;
    return m_avatars[index].character->GetGroundState() ==
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

    // CollideShape expects the target shape's center-of-mass transform, including
    // the same offset and padding used by CharacterVirtual's own shape query.
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(m_player_character->GetRotation(),
                                          m_player_character->GetPosition())
            .PreTranslated(m_player_character->GetShapeOffset() + standing_shape->GetCenterOfMass())
            .PostTranslated(m_player_character->GetCharacterPadding() *
                            m_player_character->GetUp());

    CollideShapeSettings settings;
    settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;

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

        // Query the hit body for the real surface normal and terrain shape identity.
        // Chunk terrain uses heightfields; frozen capsules share its object layer.
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
            terrain_hit = body.GetShape()->GetSubType() == JPH::EShapeSubType::HeightField;
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
            // Non-terrain body: no worldgen material applies; classify
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
    float path_factor =
        1.0f - (static_cast<float>(clear_paths) / static_cast<float>(offsets.size()) * 0.6f);
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
