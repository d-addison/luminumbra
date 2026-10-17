#pragma once

#include "FarVolumeResidency.h"

namespace Luminumbra::Rendering {

struct FarHandoffTileInput {
    FarVolumeWantedRequest wanted;
    std::shared_ptr<const World::FarFrozenObservation> observation;
};
// Constructs the terrain/water pair from one captured image. The water bridge's
// allocation/control block is admitted in the image pool and stays charged
// through its last alias, including cancelled workers. Refusal changes nothing.
std::optional<FarHandoffTileInput>
    MakeFarHandoffTileInput(FarVolumeWantedRequest,
                            std::shared_ptr<const World::FarFrozenObservation>);
struct FarHandoffManifest {
    std::uint64_t world_epoch = 0, publication_serial = 0, committed_generation = 0;
    World::FarCaveMode caves = World::FarCaveMode::BandLimited;
    World::FarVolumeNumericProfile numeric = World::FarVolumeNumericProfile::RawFloat;
    std::span<const FarHandoffTileInput> tiles;
};
enum class FarHandoffCoverageView {
    Serving,
    Target
};
enum class FarHandoffPromotion {
    None,
    InitialFallback,
    ReplacementFallback,
    FinerArrival
};
struct FarHandoffTileObservation {
    std::optional<FarVolumeWantedRequest> serving, target;
    World::FarTileStatus serving_coverage, target_coverage;
    World::FarVolumeRequest serving_built, target_built;
    FarVolumeEligibility target_eligibility = FarVolumeEligibility::Wanted;
    std::optional<FarVolumeBuildStatus> target_build_failure;
    std::optional<FarVolumeUploadError> target_upload_failure;
    std::uint64_t ready_cpu_bytes = 0, ready_upload_bytes = 0;
    std::uint64_t serving_gpu_bytes = 0, staged_gpu_bytes = 0;
    // Retention views may alias one image. Whole-pool totals count it once.
    std::uint64_t serving_source_payload_bytes = 0, target_source_payload_bytes = 0;
    bool serving_water_complete = false, target_water_complete = false;
    bool serving_water_known_dry = false, target_water_known_dry = false;
    std::uint64_t serving_terrain_indices = 0, serving_water_indices = 0;
    std::uint64_t target_terrain_indices = 0, target_water_indices = 0;
};
struct FarHandoffState {
    std::uint64_t world_epoch = 0, committed_generation = 0;
    std::uint64_t serving_committed_generation = 0;
    std::uint64_t serving_serial = 0, target_serial = 0, promotions = 0;
    FarHandoffPromotion last_promotion = FarHandoffPromotion::None;
    bool awaiting_initial_fallback = false, awaiting_replacement_fallback = false;
};
using FarHandoffObservationVisitor = void (*)(void*,
                                              const World::FarTileKey&,
                                              const FarHandoffTileObservation&);

// Owner-thread companion; the fixture residency controller is independent.
// One queue/result allowance and memory domain per tier serves both generations.
// entry_slots bounds retained key history (including removed keys), their union
// and every manifest. Exhaustion refuses staging until an explicit drain/rebind.
// Capture inputs in this domain: use shared pool aliases whose owner outlives
// the controller and all external observation aliases. Water bridges and their
// shared control blocks must also be admitted by the producer before allocation.
// Borrowed draw resources cannot escape a visitor. Unbind GPU state before any
// mutating call that can retire a pair. No callback may mutate/reenter this owner.
class FarVolumeHandoff {
public:
    FarVolumeHandoff(JobSystem&,
                     World::FarVolumeTierMemory&,
                     const FarVolumeResidencyLimits&,
                     std::pmr::memory_resource* = nullptr);
    // Heap owners admit sizeof(FarVolumeHandoff) before allocation, then transfer
    // that exact same-pool token. The convenience constructor admits stack owners.
    FarVolumeHandoff(JobSystem&,
                     World::FarVolumeTierMemory&,
                     const FarVolumeResidencyLimits&,
                     World::FarVolumeMemoryLease&& control,
                     std::pmr::memory_resource* = nullptr);
    ~FarVolumeHandoff();
    FarVolumeHandoff(const FarVolumeHandoff&) = delete;
    FarVolumeHandoff& operator=(const FarVolumeHandoff&) = delete;
    bool StageTarget(const FarHandoffManifest&);
    FarVolumeFrameResult AdvanceFrame(double camera_x,
                                      double camera_z,
                                      const FarVolumeFrameLimits&,
                                      const FarVolumeUploadBackend&);
    bool TryPromote();
    std::optional<FarHandoffTileObservation> Observe(const World::FarTileKey&) const;
    bool CoarseMayYield(const World::FarTileKey&, FarHandoffCoverageView) const;
    std::size_t VisitServingDraws(FarVolumeDrawVisitor, void*);
    void VisitObservations(FarHandoffObservationVisitor, void*);
    FarHandoffState State() const;
    std::array<World::FarVolumeTierMemoryStats, World::kFarTierCount> Memory() const;
    void CancelAndDrain();

private:
    struct Entry {
        std::optional<World::FarTileKey> key;
        std::optional<FarHandoffTileInput> target;
        std::optional<FarVolumeWantedRequest> serving;
        std::optional<FarVolumeWantedRequest> history;
        World::FarVolumeRequest resolved, history_span;
        FarVolumeEligibility eligibility = FarVolumeEligibility::Wanted;
        std::optional<FarVolumeBuildStatus> build_failure;
        std::optional<FarVolumeUploadError> upload_failure;
        std::optional<FarVolumeQueueCompletion> completion;
        FarVolumeUploadedTile serving_pair, staged_pair, retired_pair;
        double distance = 0;
    };
    struct Tier {
        static std::size_t IndexSize(std::size_t, const FarVolumeQueueLimits&);
        Tier(JobSystem&,
             World::FarVolumeMemoryPool&,
             std::size_t,
             const FarVolumeQueueLimits&,
             std::pmr::memory_resource*);
        World::FarVolumeMemoryLease charge;
        World::FarVolumeChargedResource resource;
        std::pmr::vector<Entry> entries;
        // At most entry_slots keys; twice as many buckets keep load at most 1/2.
        // Entry history is retained until drain, so no tombstones are needed.
        std::pmr::vector<std::size_t> key_index;
        std::pmr::vector<std::size_t> order;
        std::pmr::vector<World::FarVolumeRequest> validation;
        FarVolumeBuildQueue queue;
        std::size_t completion_slots;
        World::FarVolumeAuthorityLimits authority_limits;
    };
    class Guard;
    void CheckOwner() const;
    Entry* Find(const World::FarTileKey&);
    const Entry* Find(const World::FarTileKey&) const;
    const FarVolumeUploadedTile* Pair(const Entry&, FarHandoffCoverageView) const;
    bool Covers(const Entry&, FarHandoffCoverageView) const;
    bool ValidateInput(const FarHandoffManifest&,
                       const FarHandoffTileInput&,
                       const Tier&,
                       World::FarVolumeRequest&,
                       bool duplicate) const;
    FarHandoffTileObservation ObserveEntry(const Entry&) const;
    void Reject(Entry&, FarVolumeFrameResult&);
    World::FarVolumeTierMemory& memory_;
    World::FarVolumeMemoryLease control_;
    const std::thread::id owner_ = std::this_thread::get_id();
    bool active_ = false;
    FarHandoffState state_;
    World::FarCaveMode caves_ = World::FarCaveMode::BandLimited;
    World::FarVolumeNumericProfile numeric_ = World::FarVolumeNumericProfile::RawFloat;
    std::array<std::optional<Tier>, World::kFarTierCount> tiers_;
};
} // namespace Luminumbra::Rendering
