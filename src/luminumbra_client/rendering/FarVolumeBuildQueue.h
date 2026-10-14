#pragma once

#include "FarVolumeRenderMesh.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/FarVolumeRuntime.h"

#include <atomic>
#include <memory_resource>
#include <thread>

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
// bound tracked keys and running/untaken results; there are no shipped defaults.
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
    std::size_t Pump(std::size_t max_dispatches);
    void Cancel(const World::FarTileKey& key);
    void Forget(const World::FarTileKey& key);
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
    std::uint64_t dispatched_ = 0, stale_ = 0, rejected_ = 0;
};

} // namespace Luminumbra::Rendering
