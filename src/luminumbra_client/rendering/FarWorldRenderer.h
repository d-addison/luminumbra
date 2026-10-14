#pragma once

#include "FarVolumeGpuBackend.h"
#include "FarVolumeResidency.h"
#include "FarWorldCaveSelection.h"

namespace Luminumbra::Rendering {

// Explicit immutable fixture authority only. Implementations and all reachable
// data must be bounded, pure, immutable and worker safe; never wrap a mutable
// SHIELD_WorldSystem. content_identity includes numeric and cave semantics.
// A saved-world producer requires committed-generation authority + dirty parking.
class FarWorldPristineFixture {
public:
    virtual ~FarWorldPristineFixture() = default;
    virtual std::uint64_t content_identity() const noexcept = 0;
    virtual std::uint64_t owned_bytes() const noexcept = 0;
    virtual World::FarCaveMode caves() const noexcept {
        return World::FarCaveMode::BandLimited;
    }
    virtual World::FarVolumeNumericProfile numeric() const noexcept {
        return World::FarVolumeNumericProfile::LegacySigned16;
    }
    virtual std::optional<float> Height(std::int64_t x, std::int64_t z) const = 0;
    virtual std::optional<World::FarVolumeSample> Sample(const World::FarVolumePosition&,
                                                         float height,
                                                         std::uint32_t spacing,
                                                         World::FarCaveMode caves) const = 0;
};

struct FarWorldExperimentalLimits {
    World::FarTierBudgets budgets;
    FarVolumeResidencyLimits residency;
    FarVolumeFrameLimits frame;
    World::FarVolumeSpanLedgerLimits spans;
    std::uint64_t max_fixture_bytes = 0, build_headroom_bytes = 0;
    bool Complete() const noexcept;
};
// Uncalibrated development profile, never a qualified/default renderer preset.
// Per tier: 256 MiB including CPU/GPU overlap, 64 entries/descriptors, 2 result
// slots; 16 MiB conversion. Per frame: 2 builds/uploads, 16 MiB attempted upload.
// Fixture: 8 MiB; build headroom: 64 MiB; span [-8192,8192], maximum growth hull
// 2048 m; 512 history slots retained until rebinding, 512 wanted/scratch slots
// and at most 16 extensions per frame. History exhaustion refuses activation.
// Ownership: at most 8192 prepared candidates/3D live cells per publication,
// fixed 128 KiB logical SSBO reservation charged
// to V5 before allocation. CPU cells and metadata are included in owner storage.
// Height/density and generation/mesh work bounds are named in the implementation.
FarWorldExperimentalLimits FarWorldDevelopmentLimits() noexcept;

// Owner-thread integration. Constructing this object does not touch GL. Install
// a driver only AFTER loading GL with the exact client context current. Shutdown
// while that context is current, before JobSystem/world/window destruction.
// Fixture activation exercises unqualified transport. Live ownership and
// clip-aware arrival are integrated; ray depth qualification is a later slice.
class FarWorldRenderer {
public:
    explicit FarWorldRenderer(FarWorldMode requested = FarWorldMode::Legacy);
    ~FarWorldRenderer();
    FarWorldRenderer(const FarWorldRenderer&) = delete;
    FarWorldRenderer& operator=(const FarWorldRenderer&) = delete;
    void SetRequested(FarWorldMode requested);
    // Startup/world-entry selection. For a runtime change, the pipeline caller
    // first uses set_far_world_mode to reset temporal history between frames.
    // Changed Ladder policy drains every old build/resource and requires a new
    // immutable fixture; never relabel an already captured sampler's content.
    void SetRequestedCavePolicy(std::optional<World::FarCaveMode> requested);
    void InstallBackend(std::unique_ptr<FarVolumeGpuDriver> driver);
    FarWorldRefusal BindFixture(JobSystem& jobs,
                                std::shared_ptr<const FarWorldPristineFixture> fixture,
                                std::uint64_t world_epoch,
                                std::uint64_t bake_generation,
                                const FarWorldExperimentalLimits& limits);
    FarWorldSelection Selection(FarWorldPurpose purpose, bool enabled = true) const noexcept;
    // Pass the pipeline's final selection so shader refusal/preview overrides
    // are reflected in effective policy. No borrowed authority is exposed.
    FarWorldCavePolicy CavePolicy(const FarWorldSelection& selection) const noexcept;
    FarWorldCavePolicy CavePolicy(FarWorldPurpose purpose, bool enabled = true) const noexcept {
        return CavePolicy(Selection(purpose, enabled));
    }
    // Normal scheduling: checked XZ wanted set + monotone per-key span ledgers.
    // Leaving the wanted set retains charged history until the binding drains;
    // exhaustion reports SpanHistoryExhausted without replacing wanted requests.
    // Explicit extensions are camera/anchor/sightline/durable observations.
    bool PrepareFrame(std::uint64_t frame,
                      const std::array<double, 3>& camera,
                      std::span<const World::FarVolumeCoverageSpan> extensions = {});
    // GL-free transactional manifest seam for injected qualification fixtures.
    // Complete identity must match the binding. Never advances twice for a frame.
    // Shares PrepareFrame's retained history validation and commit; refused
    // manifests change neither accepted history nor residency/charges.
    bool AdvanceFrame(std::uint64_t frame,
                      const std::array<double, 3>& camera,
                      std::span<const FarVolumeWantedRequest> requests);
    FarWorldSubmitStats Submit(const std::array<double, 3>& camera,
                               const FarWorldPrepareDraw& prepare);
    void PrepareWorldSwap();
    void Shutdown();
    // Publication is independent of the culler. The pipeline feeds every prepared
    // snapshot after live GPU management; only complete masks permit submission.
    void BeginLiveOwnership(std::uint64_t frame, std::size_t limit = kFarWorldLiveCellLimit) {
        CheckOwner();
        ownership_.Begin(frame, limit);
    }
    bool AddLiveUpload(const FarWorldLiveUpload& upload) {
        CheckOwner();
        return ownership_.Add(upload);
    }
    bool FinishLiveOwnership() {
        CheckOwner();
        return ownership_.Finish();
    }
    const FarWorldOwnership& LiveOwnership() const noexcept {
        return ownership_;
    }
    const FarVolumeFrameResult& LastFrame() const noexcept {
        return last_frame_;
    }
    const FarVolumeResidency* Residency() const noexcept {
        return residency_.get();
    }
    std::uint64_t world_epoch() const noexcept {
        return world_epoch_;
    }

private:
    static constexpr std::size_t kScratchSlots = 512;
    struct Ledger {
        std::optional<FarVolumeWantedRequest> accepted;
        World::FarVolumeSpanLedger span;
    };
    void CheckOwner() const;
    void Drain();
    FarWorldRefusal ExtendLedger(const World::FarTileKey& key,
                                 float camera_y,
                                 std::span<const World::FarVolumeCoverageSpan> extensions,
                                 FarVolumeWantedRequest& request) const;
    bool ValidRequest(const FarVolumeWantedRequest& request,
                      World::FarVolumeSpanLedger& span) const;
    FarWorldRefusal ValidateAndRecordRequests(std::span<const FarVolumeWantedRequest> requests);
    FarWorldRefusal MakeRequests(const std::array<double, 3>& camera,
                                 std::span<const World::FarVolumeCoverageSpan> extensions,
                                 std::size_t& count);
    FarVolumeAuthorityProducer Producer() const;
    const std::thread::id owner_ = std::this_thread::get_id();
    FarWorldMode requested_;
    std::optional<World::FarCaveMode> requested_caves_ = kDefaultFarWorldCavePolicy;
    FarWorldRefusal refusal_ = FarWorldRefusal::ImmutableAuthorityUnavailable;
    bool submitting_ = false, stopping_ = false;
    std::optional<std::uint64_t> advanced_frame_;
    std::uint64_t world_epoch_ = 0, bake_generation_ = 0;
    FarWorldExperimentalLimits limits_;
    FarVolumeFrameResult last_frame_;
    // Explicit destruction in Drain, then Shutdown: residency -> authority and
    // integration charges -> memory -> driver. Driver lives through GPU release.
    std::unique_ptr<FarVolumeGpuDriver> driver_;
    std::unique_ptr<World::FarVolumeTierMemory> memory_;
    std::optional<World::FarVolumeMemoryLease> integration_charge_;
    std::optional<World::FarVolumeGpuLease> ownership_gpu_charge_;
    FarWorldOwnership ownership_;
    std::shared_ptr<const FarWorldPristineFixture> fixture_;
    std::unique_ptr<FarVolumeResidency> residency_;
    // Fixed history and its transactional copy are included in sizeof(*this)'s
    // integration charge. Slots are reclaimed only by Drain, never by eviction.
    std::array<Ledger, kScratchSlots> ledgers_{}, pending_ledgers_{};
    std::array<FarVolumeWantedRequest, kScratchSlots> requests_{};
};

bool MakeFarWorldDrawCommand(const FarVolumeDrawRecord& record,
                             const std::array<double, 3>& camera,
                             std::int32_t index_count,
                             FarWorldDrawCommand& command) noexcept;

} // namespace Luminumbra::Rendering
