#pragma once

#include "FarWorldWaterMesh.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/FarVolumeRuntime.h"

#include <atomic>
#include <memory_resource>
#include <span>
#include <thread>
#include <utility>

namespace Luminumbra::Rendering {

struct FarVolumeQueueLimits {
    std::size_t descriptor_slots = 0, result_slots = 0;
    World::FarVolumeCompilationLimits compilation;
    std::uint64_t max_converted_bytes = 0;
};

struct FarVolumeQueueRequest {
    World::FarBuildIdentity identity;
    World::FarVolumeCoverageIntent intent;
    // Owned immutable observations, as required by the canonical facade. The
    // field token equals content_hash and includes cave/numeric semantics.
    World::FarVolumeAuthority authority;
    std::shared_ptr<const FarWorldWaterObservation> water = {};
    std::optional<FarWorldWaterBound> water_bound = std::nullopt;
};

enum class FarVolumeSubmitStatus {
    Queued,
    Coalesced,
    Duplicate,
    Invalid,
    Full,
    AllocationFailure
};
enum class FarVolumeBuildStatus {
    Ready,
    ReadyEmpty,
    CompileRefused,
    ConvertRefused,
    WaterRefused,
    DispatchRejected,
    Cancelled,
    Failed
};

// Move-only geometry/charge pairing independent of queue lifetime. The caller
// pool outlives EVERY completion, including ones taken before a world swap.
class FarVolumeQueueCompletion {
public:
    FarVolumeQueueCompletion(FarVolumeQueueCompletion&&) noexcept = default;
    FarVolumeQueueCompletion& operator=(FarVolumeQueueCompletion&& other) noexcept;
    FarVolumeQueueCompletion(const FarVolumeQueueCompletion&) = delete;
    FarVolumeQueueCompletion& operator=(const FarVolumeQueueCompletion&) = delete;

    const World::FarBuildIdentity& identity() const noexcept {
        return identity_;
    }
    FarVolumeBuildStatus status() const noexcept {
        return status_;
    }
    const FarVolumeRenderMesh& mesh() const noexcept {
        return mesh_;
    }
    const World::FarVolumeFacadeFailure& compilation_failure() const noexcept {
        return compilation_failure_;
    }
    FarVolumeRenderError conversion_failure() const noexcept {
        return conversion_failure_;
    }
    FarWorldWaterError water_failure() const noexcept {
        return water_failure_;
    }
    const World::FarVolumeMemoryPool* memory_pool() const noexcept {
        return retained_.pool();
    }
    std::uint64_t retained_bytes() const noexcept {
        return retained_.bytes();
    }

private:
    friend class FarVolumeBuildQueue;
    FarVolumeQueueCompletion(World::FarVolumeMemoryPool& pool,
                             const World::FarBuildIdentity& identity) noexcept
        : identity_(identity)
        , retained_(pool) {}
    World::FarBuildIdentity identity_;
    FarVolumeBuildStatus status_ = FarVolumeBuildStatus::Failed;
    World::FarVolumeFacadeFailure compilation_failure_;
    FarVolumeRenderError conversion_failure_ = FarVolumeRenderError::None;
    FarWorldWaterError water_failure_ = FarWorldWaterError::None;
    // Destruction frees buffers before releasing their charge.
    World::FarVolumeMemoryLease retained_;
    FarVolumeRenderMesh mesh_;
};

struct FarVolumeQueueStats {
    std::size_t descriptors = 0, pending = 0, running = 0, ready = 0;
    std::uint64_t control_bytes = 0, retained_bytes = 0;
    std::uint64_t dispatched = 0, stale = 0, rejected = 0;
};

// Generic immutable-authority CPU consumer; no saved-world producer, pristine
// inference, GL upload or Ladder activation is supplied here. Explicit counts
// bound tracked keys and running/untaken plus caller-reported retained results;
// there are no shipped defaults.
//
// Methods and destruction belong to the constructing thread. Callbacks must
// terminate without owner-thread service/reentrancy, remain immutable and be
// safe on workers. Pool, upstream and JobSystem outlive the queue; pool also
// outlives taken completions. Shared upstreams are thread-safe. CancelAndDrain
// releases every authority reference before external world/snapshot destruction.
//
// Charged: descriptor/result control storage, canonical scratch/compilation and
// converted output. JobSystem storage/handles, dispatch batch/callable storage,
// caller-owned callback/snapshot allocations and allocator bookkeeping are
// outside this queue's accounting. Snapshot owners must account their payloads
// separately when supplying the runtime authority producer.
class FarVolumeBuildQueue {
public:
    FarVolumeBuildQueue(JobSystem& jobs,
                        World::FarVolumeMemoryPool& pool,
                        const FarVolumeQueueLimits& limits,
                        std::pmr::memory_resource* upstream = nullptr);
    ~FarVolumeBuildQueue();
    FarVolumeBuildQueue(const FarVolumeBuildQueue&) = delete;
    FarVolumeBuildQueue& operator=(const FarVolumeBuildQueue&) = delete;

    // Equal identity freezes intent and numeric/cave semantics. Replacement
    // cancels old work, then starts the latest only after its handle is reaped.
    FarVolumeSubmitStatus Submit(const FarVolumeQueueRequest& request);
    // Reap completed handles, then start at most max_dispatches Normal jobs.
    // With an explicit order, ONLY its selected keys may start, in that order;
    // the owner supplies the wanted policy's coarsest/distance/key ordering.
    // Empty order retains generic descriptor order. No order storage is owned.
    // Taken results retain their reservation until uploaded/discarded. Owners
    // pass their retained count so it shares result_slots with occupied slots.
    std::size_t Pump(std::size_t max_dispatches,
                     std::span<const World::FarTileKey> order = {},
                     std::size_t retained_results = 0);
    // Constant-time admission check; call after Pump(0) to reap finished work.
    std::size_t ResultCapacity(std::size_t retained_results = 0) const;
    void Cancel(const World::FarTileKey& key);
    void Forget(const World::FarTileKey& key);
    // Visit each descriptor once for a transactional owner's retirement pass.
    // The nonthrowing predicate borrows a key; it must not mutate/reenter this
    // queue. No allocation, per-key descriptor search, or worker wait occurs.
    // Returns visited descriptors for owner-thread work diagnostics.
    template<class Predicate>
    std::size_t ForgetMatching(const Predicate& predicate) {
        static_assert(noexcept(predicate(std::declval<const World::FarTileKey&>())));
        CheckOwner();
        for (std::size_t index = 0; index < descriptors_.size(); ++index) {
            const auto& desired = descriptors_[index].desired;
            if (desired && predicate(desired->identity.key))
                ForgetAt(index);
        }
        return descriptors_.size();
    }
    // Explicit retry after wanted-set/budget change; duplicates never auto-retry.
    bool Retry(const World::FarTileKey& key);
    std::optional<FarVolumeQueueCompletion>
    TakeCompleted(const World::FarBuildIdentity& expected,
                  const World::FarVolumeCoverageIntent& current_intent);
    void CancelAndDrain();
    FarVolumeQueueStats Stats() const;

private:
    struct Descriptor {
        std::optional<FarVolumeQueueRequest> desired;
        std::optional<std::size_t> slot;
        bool pending = false, forget = false;
    };
    struct Slot {
        Slot() = default;
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&&) = delete;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        bool occupied = false, ready = false;
        std::size_t descriptor = 0;
        World::FarVolumeMemoryPool* pool = nullptr;
        std::pmr::memory_resource* upstream = nullptr;
        FarVolumeQueueLimits limits;
        std::optional<FarVolumeQueueRequest> request;
        std::optional<FarVolumeQueueCompletion> result;
        std::atomic<bool> cancelled{false}, published{false};
        JobHandle handle;
    };
    static void Run(Slot* slot) noexcept;
    void CheckOwner() const;
    void Reap();
    void ClearSlot(std::size_t index);
    void CancelAt(std::size_t index);
    void ForgetAt(std::size_t index);
    std::optional<std::size_t> Find(const World::FarTileKey& key) const;

    JobSystem& jobs_;
    World::FarVolumeMemoryPool& pool_;
    FarVolumeQueueLimits limits_;
    std::pmr::memory_resource* upstream_;
    const std::thread::id owner_ = std::this_thread::get_id();
    World::FarVolumeMemoryLease control_;
    World::FarVolumeChargedResource resource_;
    std::pmr::vector<Descriptor> descriptors_;
    std::pmr::vector<Slot> slots_;
    std::size_t occupied_results_ = 0;
    std::uint64_t dispatched_ = 0, stale_ = 0, rejected_ = 0;
};

} // namespace Luminumbra::Rendering
