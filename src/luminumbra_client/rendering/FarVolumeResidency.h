#pragma once

#include "FarVolumeUpload.h"
#include "FarWorldOwnership.h"

#include <array>

namespace Luminumbra::Rendering {

struct FarVolumeWantedRequest {
    World::FarBuildIdentity identity;
    World::FarVolumeCoverageIntent intent;
    // Explicit producer estimate of additional snapshot + compile + conversion
    // peak bytes. Admission remains authoritative; no native estimate/default.
    std::uint64_t build_headroom_bytes = 0;
    std::shared_ptr<const FarWorldWaterObservation> water = {};
    std::optional<FarWorldWaterBound> water_bound = std::nullopt;
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
    World::FarBuildIdentity built_identity;
    World::FarVolumeRequest built_request;
    bool water_complete = false, water_known_dry = false;
    std::uint64_t terrain_indices = 0, water_indices = 0;
};
// Owner-thread work for the last admission attempt, including refusals. Counts
// index key comparisons and descriptor visits, not authority sampling.
// This diagnostic never affects admission, ordering, or resource ownership.
struct FarVolumeAdmissionWork {
    std::size_t key_comparisons = 0, descriptor_visits = 0;
};
struct FarVolumeFrameResult {
    std::size_t dispatched = 0, uploaded = 0, rejected = 0, evicted = 0;
    std::size_t build_attempts = 0, upload_attempts = 0;
    // Geometry allowance debited before backend entry, including failed work.
    std::uint64_t upload_bytes = 0, successful_upload_bytes = 0;
    bool horizon_refused = false;
};
struct FarVolumeTierObservation {
    std::size_t wanted = 0, pending = 0, building = 0, ready = 0, settled = 0;
    std::size_t request_rejected = 0, budget_evicted = 0;
    // Pair counts include stale resources; covering counts only complete current
    // identity + span. Empty pairs have no GPU object or charge.
    std::size_t uploaded_pairs = 0, empty_pairs = 0, covering = 0;
    std::uint64_t ready_cpu_bytes = 0, retained_gpu_bytes = 0;
    // Whole pool includes queue/owner controls, snapshots, scratch, overlapping
    // source/converted results and GPU, unlike the retained pair subtotals.
    // Worker activity can skew CPU/GPU sampling; diagnostics never admit bytes.
    World::FarVolumeTierMemoryStats memory;
    std::size_t water_known_dry_tiles = 0, water_wet_tiles = 0;
    std::uint64_t retained_water_indices = 0, submitted_water_indices = 0;
};
struct FarVolumeDrawRecord {
    World::FarBuildIdentity identity;
    World::FarVolumeRequest request;
    World::FarVolumePosition origin_meters;
    const FarVolumeGpuAllocation* resource = nullptr;
    double horizontal_outer_clip_meters = 0;
    double view_ray_bias_meters = 0;
    FarWorldArrival arrival;
    std::uint64_t terrain_indices = 0, water_indices = 0;
};
using FarVolumeDrawVisitor = void (*)(void*, const FarVolumeDrawRecord&);
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
    std::array<FarVolumeTierObservation, World::kFarTierCount> Snapshot() const;
    // Borrowed GPU pointer is valid only until next mutation/destruction. Stale
    // pairs are observable but never reported covering. Renderer chooses draw.
    const FarVolumeGpuAllocation* Resource(const World::FarTileKey& key) const;
    bool CoarseMayYield(const World::FarTileKey& key) const;
    // Allocation-free borrowed records, coarsest first. Only current/spanning
    // GPU pairs appear; Empty covers/yields but never draws. Arrival eligibility is
    // re-evaluated on every call. Consumer applies fragment horizontal XZ clip
    // and AWAY-from-camera ray bias (spacing/2), never triangle/frustum skips.
    // Coarse records always carry arrival metadata; fragments test finer radius
    // and the relevant built span. Live ownership and the 176m inner clip remain the
    // renderer's contract. No pointer may escape controller mutation/lifetime.
    // Callback mutation/re-entry is refused; callback exceptions propagate.
    std::size_t VisitDraws(FarVolumeDrawVisitor visitor, void* context);
    // Cancels/drains every queue BEFORE dropping any authority/CPU/GPU pair.
    void CancelAndDrain();
    FarVolumeAdmissionWork AdmissionWork() const;

private:
    struct Entry {
        std::optional<FarVolumeWantedRequest> desired;
        FarVolumeEligibility eligibility = FarVolumeEligibility::Wanted;
        std::optional<FarVolumeQueueCompletion> completion;
        FarVolumeUploadedTile uploaded;
        double distance = 0;
    };
    struct Tier {
        static std::size_t IndexSize(std::size_t, const FarVolumeQueueLimits&);
        void RebuildIndex(FarVolumeAdmissionWork&);
        Tier(JobSystem&,
             World::FarVolumeMemoryPool&,
             std::size_t,
             const FarVolumeQueueLimits&,
             std::pmr::memory_resource*);
        World::FarVolumeMemoryLease control;
        World::FarVolumeChargedResource resource;
        std::pmr::vector<Entry> entries;
        // First entry_slots indices are sorted live descriptors; the second half
        // holds at most entry_slots incoming positions during admission.
        std::pmr::vector<std::size_t> key_index;
        std::size_t index_count = 0;
        std::pmr::vector<std::size_t> order;
        FarVolumeBuildQueue queue;
        std::size_t completion_slots;
    };
    class Guard;
    void CheckOwner() const;
    Entry* Find(const World::FarTileKey&, FarVolumeAdmissionWork* = nullptr);
    const Entry* Find(const World::FarTileKey&, FarVolumeAdmissionWork* = nullptr) const;
    bool Headroom(std::size_t tier,
                  std::uint64_t bytes,
                  const World::FarTileKey& preserve,
                  FarVolumeFrameResult& result);
    void Reject(Entry&, FarVolumeFrameResult&);
    World::FarVolumeTierMemory& memory_;
    const std::thread::id owner_ = std::this_thread::get_id();
    bool active_ = false;
    FarVolumeAdmissionWork admission_work_;
    std::array<std::optional<Tier>, World::kFarTierCount> tiers_;
};
} // namespace Luminumbra::Rendering
