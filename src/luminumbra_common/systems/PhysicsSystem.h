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

#include "../world/PlayerAvatar.h"
#include "AmbientContinuation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra {

class Chunk;
namespace World {
struct PhysicsOwnerState;
}
namespace world {
class GameSession;
}

namespace Systems {

class SHIELD_WorldSystem;
class JoltRuntimeLease;

struct ChunkCollisionData {
    JPH::BodyID body_id;
};

class PhysicsSystem {
public:
    PhysicsSystem();
    ~PhysicsSystem();

    // Lifecycle calls require owner-thread exclusion of updates/producers.
    // A supplied Jolt factory must be fully initialized and remain alive,
    // with compatible complete allocators, through every runtime lease,
    // including startup preparation. Process-static RTTI also retains storage
    // that requires compatible allocators after the final lease ends.
    void startup();
    void shutdown();
    void update(float delta_time);
    struct WorkItems {
        std::uint64_t bodies = 0;
        std::uint64_t characters = 0;
    };
    // Input bodies for the latest update call, plus character steps since it.
    // Diagnostic only; static bodies count too, excluding private simulation
    // terrain slots charged to terrain headroom. No solver/timing estimates.
    [[nodiscard]] WorkItems work_items() const {
        return m_work_items;
    }
    bool is_started() const {
        return m_started;
    }

    // --- World Geometry ---
    // Streaming admits up to 8192 chunks, with at most one terrain body each.
    // Frozen capsules must leave this many body slots free after allocation.
    static constexpr std::uint32_t kTerrainBodyHeadroom = 8192;
    // False only when the body pool refuses creation; callers may retry later.
    bool add_chunk_collision(Chunk& chunk);
    bool add_chunk_collision(Chunk& chunk, JPH::BodyID restored_id);
    // Const continuation diagnostic. Missing means this chunk has no terrain body.
    std::optional<JPH::BodyID> chunk_collision_body(ChunkID chunk_id) const;
    // Explicit-save capture only; refuses above the 8192-terrain-owner bound
    // before allocating. Output is sorted by chunk ID and unchanged on refusal.
    bool capture_chunk_collision_owners(std::vector<std::pair<ChunkID, JPH::BodyID>>& output) const;
    void remove_chunk_collision(ChunkID chunk_id);
    // Explicit continuation only. At most 128 private terrain bodies and sampled
    // columns; unused bodies retain their reserved IDs outside the broadphase.
    struct SimulationTerrainState {
        struct Column {
            ChunkID id = 0;
            std::array<float, (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1)> heights{};
        };
        std::vector<std::uint32_t> bodies;
        std::vector<Column> columns;
    };
    bool capture_simulation_terrain(SimulationTerrainState& output) const;
    // Destination must be isolated and contain no simulation terrain yet.
    bool restore_simulation_terrain(const SimulationTerrainState& state);

    // Shared owner-thread barrier before rigid bodies or characters advance. Inert unless
    // region authority is enabled. A refusal publishes nothing and pauses physics until a
    // successful retry; hosts must also leave their simulation tick unconsumed.
    // A concrete tick id reuses its successful publication within one host/session step.
    // Variable-rate render hosts use the default sentinel to start a fresh preparation.
    // Fixed one-chunk X/Z radius, 64 admitted character owners and 128 distinct columns.
    // Existing overflow (including rigid bodies in the independent Jolt pool) uses the
    // first complete horizons that fit, in owner order, so lifecycle ticks can continue.
    // Uncovered owners wait without integration; rigid-body velocities resume on coverage.
    // Storage and terrain body slots are fixed; only changed shapes replace storage.
    static constexpr std::size_t kSimulationOwnerBudget = 64;
    static constexpr std::size_t kSimulationColliderBudget = 128;
    bool
    prepare_simulation_collisions(std::span<const Vec3> anchors,
                                  std::uint64_t tick = std::numeric_limits<std::uint64_t>::max());
    void clear_simulation_collisions();
    // Const diagnostics, never session hash/save inputs. Counts actual sampled columns.
    std::size_t simulation_collision_sample_count() const;
    // Positions visited by preparation; retired avatar slots must contribute no visits.
    std::size_t simulation_collision_owner_visit_count() const;
    std::uint64_t simulation_collision_hash() const;

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

    // --- Player Controller (positions are feet positions in both stances) ---
    // Refusal preserves the existing controller. The caller may retry later.
    bool create_player_controller(const glm::vec3& initial_position);
    void update_player(const glm::vec3& wish_velocity,
                       bool wants_to_jump,
                       float jump_force,
                       float delta_time);
    void set_player_position(const glm::vec3& position);
    glm::vec3 get_player_position() const;
    bool has_player_controller() const {
        return m_player_character != nullptr;
    }
    bool is_player_grounded() const;
    // Returns false when collision prevents changing shape; the old stance remains.
    bool set_player_crouched(bool is_crouched);
    bool player_has_space_to_stand() const;

    // --- Server-authoritative multiplayer avatars ---
    // The authoritative server gives every connected player's avatar a Jolt
    // CharacterVirtual (capsule) so players collide with the world (and, once
    // props exist, with dynamic props -- the Garry's-Mod model). ADDITIVE: this
    // is a SEPARATE set from the singleton m_player_character the local client
    // uses, so the client path is untouched. Avatars are stepped in creation order
    // on the calling thread, including reused slots appended at the tail, so
    // the avatar positions that fold into the `entities` sub-hash are run==replay
    // reproducible. Per-avatar movement comes from the latest network usercmd;
    // avatars with no input settle against world collision under gravity.
    void clear_avatar_characters();
    // Positions use feet. Refusal returns kInvalidAvatar without creating a slot.
    static constexpr std::size_t kInvalidAvatar = std::numeric_limits<std::size_t>::max();
    std::size_t create_avatar_character(const glm::vec3& initial_position);
    // Leaves other avatar indices stable. Repeated removal of a free slot is harmless.
    // Retired indices are reused lowest-first; callers must discard removed handles.
    void remove_avatar_character(std::size_t index);
    // Frozen avatars keep zero velocity and are excluded from integration, even
    // if the body pool refuses their static capsule. Repeated freezes retry it.
    // Release every resuming avatar before staging frozen capsules, then call
    // update_avatars so movers see all available capsules regardless of order.
    // Finite feet positions also synchronize teleports while frozen or resuming;
    // invalid positions retain the last valid avatar position. Unfrozen avatars
    // that were never frozen retain the legacy movement path byte-for-byte.
    void set_avatar_frozen(std::size_t index, bool frozen, const glm::vec3& position);
    // Set an avatar's horizontal wish velocity (world XZ m/s) for the
    // next step -- the server applies the network usercmd's movement here. Persists
    // until changed (0 by default -> the avatar stands, the without movement input behaviour).
    void set_avatar_wish_velocity(std::size_t index, const glm::vec2& wish_xz);
    // Steps every avatar character by dt: the per-avatar horizontal wish velocity
    // (gravity when airborne, ground-stick when grounded) against the world geometry.
    void update_avatars(float delta_time);
    // Removal holes count as slots; reuse bounds storage by the peak live count.
    std::size_t avatar_character_slot_count() const {
        return m_avatars.size();
    }
    struct AvatarCharacterDiagnostics {
        std::size_t live_count = 0;
        std::vector<std::size_t> creation_order;
    };
    // Const owner-thread diagnostic: copies only live indices, never hash/save inputs.
    [[nodiscard]] AvatarCharacterDiagnostics avatar_character_diagnostics() const;
    glm::vec3 get_avatar_position(std::size_t index) const;
    glm::vec3 get_avatar_velocity(std::size_t index) const;
    bool is_avatar_grounded(std::size_t index) const;

    // Internal, owner-thread continuation between ticks. The optional saved
    // continuation envelope retains these values without changing hash inputs.
    // Terrain must already have identical BodyIDs, shapes and settings in the destination, which
    // may contain static bodies but no dynamic bodies. Captured owners are
    // restored as one complete set, before any controller adoption.
    static constexpr std::size_t kMaxRestoredBodies = 128;
    static constexpr std::size_t kMaxRestoredAvatars = 4096;
    struct BodyOwnerState {
        // Bounded scalar payload, including this body's shared contact caches.
        static constexpr std::size_t kMaxBytes = 65536;
        std::array<std::uint8_t, kMaxBytes> bytes{};
        std::uint32_t size = 0;
        // Envelope metadata; the independent versioned scalar payload is unchanged.
        // Defaulted fields keep existing dynamic-sphere/capsule records compatible.
        bool suspended = false;
        glm::vec3 retained_velocity{0.0f};
        glm::vec3 retained_angular_velocity{0.0f};
        bool was_active = false;
        // Live terrain-registration slot + 1 (0 = unregistered). Restore keeps
        // this order, which swap-removal and reuse make independent of payload order.
        std::uint32_t registration_slot = 0;
    };
    struct BodySetState {
        std::vector<BodyOwnerState> owners;
        std::uint32_t slot_count = 0;
        // At most 10240 freed slots, last entry reused first. IDs retain the
        // last allocated generation, including zero after uint8 wrap. History
        // is independent of owners so even an empty set continues allocation.
        std::vector<std::uint32_t> free_slots;
    };
    // Captures a sphere or a frozen capsule; terrain is supplied separately.
    bool capture_body_owner(JPH::BodyID body, BodyOwnerState& output) const;
    bool capture_body_owners(BodySetState& output) const;
    // A refusal preserves all destination state and every output ID. The slot
    // extent cannot shrink below the destination's existing allocated extent.
    bool restore_body_owners(const BodySetState& state, std::span<JPH::BodyID> bodies);

    // Internal owner-thread continuation adapter. These operations are separate
    // from the public physics gameplay interface and never run during a tick.
    // Remaining frozen captures reserve their body slots until restored in order.
    // Only the session, which validates owners first, constructs it.
    class OwnerContinuation {
    public:
        bool capture_player_owner(World::PhysicsOwnerState& output) const;
        bool restore_player_owner(const World::PhysicsOwnerState& state);
        bool capture_avatar_owner(std::size_t index, World::PhysicsOwnerState& output) const;
        bool restore_avatar_owner(const World::PhysicsOwnerState& state,
                                  std::size_t& index,
                                  std::span<const World::PhysicsOwnerState> remaining_frozen = {});

        // Complete isolated continuation adoption without clearing restored terrain
        // or resuming suspended bodies. Only GameSession constructs this adapter.
        void attach_world_system(const SHIELD_WorldSystem* world) {
            m_physics.m_world_system = world;
        }

    private:
        friend class ::Luminumbra::world::GameSession;
        explicit OwnerContinuation(PhysicsSystem& physics)
            : m_physics(physics) {}
        PhysicsSystem& m_physics;
    };

    // --- Audio-Physics Integration ---
    // Non-owning world-generation seam for surface-material classification on
    // terrain raycast hits. Set once at session startup (GameSession creates
    // physics before the world system and destroys it first, so the pointer
    // cannot dangle). Null (standalone tests) keeps the legacy Y-band
    // classification byte-identical.
    void set_world_system(const SHIELD_WorldSystem* world_system);

    struct AudioRaycastResult {
        bool hit = false;
        float distance = 0.0f;
        glm::vec3 hit_point = {0.0f, 0.0f, 0.0f};
        glm::vec3 surface_normal = {0.0f, 1.0f, 0.0f};
        float material_absorption = 0.1f;
        // static_cast<int>(MaterialType) codes (core/Types.h) when a world
        // system is attached; legacy 0=stone/1=dirt/2=grass band codes otherwise.
        int material_type = 0;
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
    bool capture_player_owner(World::PhysicsOwnerState& output) const;
    bool restore_player_owner(const World::PhysicsOwnerState& state);
    bool capture_avatar_owner(std::size_t index, World::PhysicsOwnerState& output) const;
    bool restore_avatar_owner(const World::PhysicsOwnerState& state,
                              std::size_t& index,
                              std::span<const World::PhysicsOwnerState> remaining_frozen = {});
    struct SimulationCollisions;
    std::unique_ptr<SimulationCollisions> m_simulation_collisions;
    bool simulation_collisions_enabled() const;
    bool simulation_collisions_ready() const;
    bool can_admit_simulation_owner(const glm::vec3& position, bool replacing_player) const;
    std::uint32_t non_simulation_body_count() const;

    struct BodySlotHistory;
    std::unique_ptr<BodySlotHistory> m_body_slots;
    void record_body_allocation(JPH::BodyID id);
    void record_body_release(JPH::BodyID id);
    WorkItems m_work_items{};
    // Independent ownership, released only after all Jolt resources.
    std::unique_ptr<JoltRuntimeLease> m_runtime_lease;
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
    // + the shared avatar capsule shape. The live list preserves creation order;
    // slot indices remain stable handles and are independent of traversal order.
    // A single, nonthrowing-movable element keeps ownership and movement state
    // together if vector growth refuses an allocation.
    static constexpr std::size_t kNoAvatar = std::numeric_limits<std::size_t>::max();
    struct AvatarState {
        std::unique_ptr<JPH::CharacterVirtual> character;
        glm::vec2 wish{0.0f};
        JPH::BodyID frozen_body;
        bool frozen = false;
        std::size_t previous = kNoAvatar;
        std::size_t next = kNoAvatar;
    };
    std::vector<AvatarState> m_avatars;
    // Min-heap, at most one entry per retired slot. Capacity is reserved before
    // publishing avatars so removal (including registry destruction) cannot allocate.
    std::vector<std::size_t> m_free_avatar_slots;
    std::size_t m_avatar_head = kNoAvatar;
    std::size_t m_avatar_tail = kNoAvatar;
    std::size_t m_live_avatar_count = 0;
    JPH::Ref<JPH::Shape> m_avatar_shape;

    // Track collision bodies for loaded chunks
    std::unordered_map<ChunkID, ChunkCollisionData> m_chunk_bodies;

    // Batched physics queries
    BatchedPhysicsQueries m_batched_queries;

    // Non-owning worldgen seam for audio-raycast material classification
    // (see set_world_system). Null in standalone tests.
    const SHIELD_WorldSystem* m_world_system = nullptr;

    bool m_started = false;
};

} // namespace Systems
} // namespace Luminumbra

namespace Luminumbra::Persistence {

// Explicit-save work only: 128 bodies, 4096 avatars and 1 MiB aggregate C2 payload.
// The 1536 KiB envelope shares the existing 2 MiB metadata admission budget.
// Larger captures refuse before any writer; no legacy compatibility limit changes.
inline constexpr std::size_t kOwnerContinuationPayloadBytes = 1024u * 1024u;
inline constexpr std::size_t kOwnerContinuationRecordBytes = 1536u * 1024u;
struct TerrainOwnerContinuation {
    ChunkID chunk_id = 0;
    std::uint32_t body_id = 0;
};
struct PhysicsOwnerContinuation {
    std::uint32_t owner_id = 0;
    Vec3 anchor{};
    Systems::PhysicsSystem::BodyOwnerState state;
};
struct ControllerOwnerContinuation {
    std::uint32_t owner_id = 0;
    Vec3 anchor{};
    World::PhysicsOwnerState state;
};
// Explicit saves retain at most 8192 resident IDs; check counts before allocation.
inline constexpr std::size_t kOwnerContinuationResidentChunks = 8192;
struct OwnerContinuation {
    std::uint32_t version = 2;
    std::vector<ChunkID> resident_chunks;
    std::optional<Systems::AmbientContinuation> ambient;
    std::uint32_t world_seed = 0;
    std::string preset_identity;
    std::uint64_t simulation_tick = 0;
    std::uint64_t stream_tick = 0;
    bool moving_anchor = false;
    bool water_smoke = false;
    Vec3 origin{};
    Vec3 anchor{};
    std::vector<World::PlayerAvatarOwnerState> avatars;
    std::vector<PhysicsOwnerContinuation> bodies;
    std::optional<ControllerOwnerContinuation> player;
    std::uint32_t slot_count = 0;
    std::vector<std::uint32_t> free_slots;
    Systems::PhysicsSystem::SimulationTerrainState simulation_terrain;
    std::vector<TerrainOwnerContinuation> terrain;
};
bool EncodeOwnerContinuation(const OwnerContinuation& record, std::string& bytes);
bool DecodeOwnerContinuation(const std::string& bytes, OwnerContinuation& record);

} // namespace Luminumbra::Persistence
