#pragma once

#include "FarVolumeRenderMesh.h"
#include "luminumbra_common/core/JobSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Luminumbra::Rendering {

inline constexpr std::size_t kFarVolumeRequestCapacity = 64;
inline constexpr std::size_t kFarVolumeResultCapacity = 2;
inline constexpr std::size_t kFarVolumeSlotReservation = 256u * 1024u * 1024u;

enum class FarVolumeBuildStatus {
    Ready,
    ReadyEmpty,
    BudgetRefused,
    Invalid,
    Cancelled,
    Stale,
    Failed
};

struct FarVolumeBuildIdentity {
    std::uint64_t binding_epoch = 0;
    std::uint64_t request_generation = 0;
    std::uint64_t params_hash = 0;
    std::uint64_t authority_revision = 0;
    int seed = 0;
    World::FarVolumeRequest submitted_request;
    World::FarVolumeRequest effective_request; // union of same-tile span requests
};

struct FarVolumeBuildResult {
    FarVolumeBuildIdentity identity;
    FarVolumeBuildStatus status = FarVolumeBuildStatus::Failed;
    // No unbounded diagnostic string allocations on a worker failure path.
    std::array<char, 256> error{};
    bool span_complete = false;
    std::int32_t first_brick_y = 0, last_brick_y = 0;
    std::uint64_t sampled_bricks = 0, density_samples = 0;
    std::uint32_t tile_crc32 = 0;
    std::size_t tile_capacity_bytes = 0, geometry_capacity_bytes = 0;
    FarVolumeRenderMesh mesh;
};

struct FarVolumeBuildLimits {
    World::FarVolumeLimits generation;
    World::FarVolumeMeshLimits geometry;
    std::size_t converted_bytes = kFarVolumeRenderMeshBudget;
};

enum class FarVolumeSubmitStatus {
    Queued,
    Duplicate,
    Full,
    Invalid,
    Unbound,
    IdentityChanged,
    SequenceExhausted
};
struct FarVolumeSubmission {
    FarVolumeSubmitStatus status;
    std::uint64_t request_generation = 0;
};

struct FarVolumeQueueStats {
    std::size_t descriptors = 0, pending = 0, running = 0, completed = 0, leased = 0;
    std::size_t occupied_slots = 0, payload_reserved_bytes = 0;
    std::size_t retained_mesh_bytes = 0, control_storage_bytes = 0;
    std::size_t descriptor_stride = 0, result_slot_stride = 0;
    std::size_t vertex_stride = sizeof(VoxelVertex);
    std::size_t index_stride = sizeof(std::uint32_t);
    std::size_t brick_stride = sizeof(World::FarVolumeBrick);
    std::uint64_t dispatches = 0, stale_results = 0;
};

struct FarVolumeQueueShared;

// Move-only, read-only mesh ownership. Releasing a lease releases its reservation;
// an arbitrary caller-made copy is outside this queue's accounting. A lease can
// outlive the queue/world; it retains geometry and metadata, never a world pointer.
class FarVolumeResultLease {
public:
    FarVolumeResultLease(FarVolumeResultLease&& other) noexcept;
    FarVolumeResultLease& operator=(FarVolumeResultLease&& other) noexcept;
    ~FarVolumeResultLease();
    FarVolumeResultLease(const FarVolumeResultLease&) = delete;
    FarVolumeResultLease& operator=(const FarVolumeResultLease&) = delete;

    const FarVolumeBuildResult& result() const;
    void release() noexcept;

private:
    friend class FarVolumeBuildQueue;
    FarVolumeResultLease(std::shared_ptr<FarVolumeQueueShared> state, std::size_t slot);
    std::shared_ptr<FarVolumeQueueShared> m_state;
    std::size_t m_slot = 0;
};

// Compiled consumer with controlled pristine fixture callers only. No production
// rendering/save/GL caller exists. Zero authority revision is an additional guard,
// NOT proof that an arbitrary world or save is pristine.
//
// All queue methods belong to the constructing thread; lease destruction may
// occur elsewhere. The bound world and JobSystem must live through drain().
class FarVolumeBuildQueue {
public:
    explicit FarVolumeBuildQueue(JobSystem& jobs, FarVolumeBuildLimits limits = {});
    ~FarVolumeBuildQueue();
    FarVolumeBuildQueue(const FarVolumeBuildQueue&) = delete;
    FarVolumeBuildQueue& operator=(const FarVolumeBuildQueue&) = delete;

    // Always drains the old binding. Rebinding the same address advances epoch.
    // Returns false for non-pristine fixture authority; there is no save probe.
    bool bind_fixture_world(const Systems::SHIELD_WorldSystem& world);
    void prepare_world_swap();
    FarVolumeSubmission submit(const World::FarVolumeRequest& request);
    bool cancel(std::uint64_t request_generation);
    // Cancels/removes a descriptor, allowing an explicit retry after a terminal
    // failure. Repeated submit of an unchanged terminal request is a duplicate.
    bool forget(std::uint64_t request_generation);
    bool pump(); // At most one dispatch. Never samples terrain on the owner.
    std::optional<FarVolumeResultLease> take_completed();
    bool is_current(const FarVolumeBuildIdentity& identity) const;
    FarVolumeQueueStats stats() const;
    void drain(); // Waits bounded jobs, not a wall-clock deadline; keeps results.

private:
    // No runtime switch exposes these deterministic worker barriers/fault seams.
    friend struct FarVolumeBuildQueueTestAccess;
    enum class Phase {
        BeforeSampling,
        AfterSampling,
        AfterMeshing,
        BeforePublish
    };
    using PhaseHook = void (*)(
        void*, Phase, const FarVolumeBuildIdentity&, World::FarVolumeTile*, World::FarVolumeMesh*);
    void set_test_hook(PhaseHook hook, void* context);
    void set_test_sequence(std::uint64_t sequence);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Luminumbra::Rendering
