#pragma once

#include "FarVolumeUpload.h"

#include <array>

namespace Luminumbra::Rendering {

struct FarVolumeWantedRequest {
    World::FarBuildIdentity identity;
    World::FarVolumeCoverageIntent intent;
    // Explicit producer estimate of additional snapshot + compile + conversion
    // peak bytes. Admission remains authoritative; no native estimate/default.
    std::uint64_t build_headroom_bytes = 0;
};
struct FarVolumeResidencyLimits {
    std::array<std::size_t, World::kFarTierCount> entry_slots{};
    std::array<FarVolumeQueueLimits, World::kFarTierCount> queues{};
};
struct FarVolumeFrameLimits {
    std::size_t builds = 0, uploads = 0;
    std::uint64_t upload_bytes = 0;
};
enum class FarVolumeEligibility {
    Wanted,
    Pending,
    Building,
    Ready,
    Settled,
    RequestRejected,
    BudgetEvicted
};
struct FarVolumeResidentObservation {
    World::FarBuildIdentity desired;
    FarVolumeEligibility eligibility = FarVolumeEligibility::Wanted;
    // Independent of eligibility: a rejected rebuild can retain covering GPU.
    World::FarTileStatus coverage;
    std::uint64_t cpu_bytes = 0, gpu_bytes = 0;
    std::uint64_t ready_upload_bytes = 0;
};
struct FarVolumeFrameResult {
    std::size_t dispatched = 0, uploaded = 0, rejected = 0, evicted = 0;
    std::size_t build_attempts = 0, upload_attempts = 0;
    std::uint64_t upload_bytes = 0;
    bool horizon_refused = false;
};
using FarVolumeAuthorityProducer = std::function<std::optional<World::FarVolumeAuthority>(
    const FarVolumeWantedRequest&, World::FarVolumeMemoryPool&)>;

// Owner-thread bounded canonical residency, with explicit budgets/limits. No
// native producer, GL backend, renderer activation or shipped defaults. Caller
// owns Jobs, memory, GPU context and the producer; they outlive the controller.
// Producer charges owned snapshot/overlay payload BEFORE allocating it against
// the supplied pool. Queue callback/JobSystem bookkeeping exclusions remain.
// All entries and ordering scratch are charged; this object has fixed storage.
// No callback may re-enter controller methods or require owner-thread service.
// Destroy/drain before destroying world authority; GPU context/memory live until
// controller destruction completes. Explicit estimates enable prospective
// eviction; actual atomic admission can still refuse, with no automatic retry.
class FarVolumeResidency {
public:
    FarVolumeResidency(JobSystem& jobs,
                       World::FarVolumeTierMemory& memory,
                       const FarVolumeResidencyLimits& limits,
                       std::pmr::memory_resource* upstream = nullptr);
    ~FarVolumeResidency();
    FarVolumeResidency(const FarVolumeResidency&) = delete;
    FarVolumeResidency& operator=(const FarVolumeResidency&) = delete;
    // Transaction validates every key/intent/duplicate/capacity first. Equal
    // identity freezes intent AND estimate. Span growth requires new identity.
    // A key-set change clears rejection; request change only clears request-
    // scoped failure. Budget-eviction latch survives request identity changes.
    bool ReplaceWanted(std::span<const FarVolumeWantedRequest> requests);
    FarVolumeFrameResult AdvanceFrame(double camera_x,
                                      double camera_z,
                                      const FarVolumeFrameLimits& limits,
                                      const FarVolumeAuthorityProducer& producer,
                                      const FarVolumeUploadBackend& backend);
    std::optional<FarVolumeResidentObservation> Observe(const World::FarTileKey& key) const;
    // Borrowed GPU pointer is valid only until next mutation/destruction. Stale
    // pairs are observable but never reported covering. Renderer chooses draw.
    const FarVolumeGpuAllocation* Resource(const World::FarTileKey& key) const;
    bool CoarseMayYield(const World::FarTileKey& key) const;
    // Cancels/drains every queue BEFORE dropping any authority/CPU/GPU pair.
    void CancelAndDrain();

private:
    struct Entry {
        std::optional<FarVolumeWantedRequest> desired;
        FarVolumeEligibility eligibility = FarVolumeEligibility::Wanted;
        std::optional<FarVolumeQueueCompletion> completion;
        FarVolumeUploadedTile uploaded;
        double distance = 0;
    };
    struct Tier {
        Tier(JobSystem&,
             World::FarVolumeMemoryPool&,
             std::size_t,
             const FarVolumeQueueLimits&,
             std::pmr::memory_resource*);
        World::FarVolumeMemoryLease control;
        World::FarVolumeChargedResource resource;
        std::pmr::vector<Entry> entries;
        std::pmr::vector<std::size_t> order;
        FarVolumeBuildQueue queue;
        std::size_t completion_slots;
    };
    class Guard;
    void CheckOwner() const;
    Entry* Find(const World::FarTileKey&);
    const Entry* Find(const World::FarTileKey&) const;
    bool Headroom(std::size_t tier,
                  std::uint64_t bytes,
                  const World::FarTileKey& preserve,
                  FarVolumeFrameResult& result);
    void Reject(Entry&, FarVolumeFrameResult&);
    World::FarVolumeTierMemory& memory_;
    const std::thread::id owner_ = std::this_thread::get_id();
    bool active_ = false;
    std::array<std::optional<Tier>, World::kFarTierCount> tiers_;
};
} // namespace Luminumbra::Rendering
