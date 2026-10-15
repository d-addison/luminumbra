#pragma once

#include "FarEditedCatalog.h"
#include "FarFrozenObservation.h"
#include "persistence/EditedSourceImage.h"

namespace Luminumbra::World {

enum class FarCacheError : std::uint8_t {
    None,
    InvalidSource,
    InvalidCatalog,
    InvalidOverlay,
    RecoveryPending,
    GenerationMismatch,
    WorkLimit,
    ByteLimit,
    CaptureFailure,
    BakeFailure,
    PublicationFailure,
    RollbackFailure,
    AllocationFailure
};
struct FarEditedCacheLimits {
    FarCatalogLimits catalog;
    Persistence::EditedSourceLimits source;
    FarEditedBakeLimits bake;
    FarFrozenCaptureLimits frozen;
    std::uint64_t max_reconcile_probes = 0;
    // Pipeline reservations include catalog DOM/control storage, plans, bake scratch,
    // encoded/decoded overlap and finalizer controls. Images/frozen data charge separately.
    std::uint64_t max_pipeline_bytes = 0, max_cache_bytes = 0;
    std::uint64_t terrain_identity = 0, content_identity = 0;
    FarCaveMode caves = FarCaveMode::BandLimited;
    FarVolumeNumericProfile numeric = FarVolumeNumericProfile::LegacySigned16;
    // Optional owner capture guard, retained until frozen sampling/baking completes.
    std::function<std::shared_ptr<const void>()> capture_boundary;
    // Owner-thread capture only; callbacks are consumed synchronously and never retained.
    // Water must come from explicit authoritative truth, including supported dry observations.
    std::function<FarFrozenCaptureSource(const Persistence::EditedSourceImage&)> capture_source;
};
struct FarCacheDueTile {
    FarTileKey key;
    std::vector<FarCatalogDependency> dependencies;
    std::optional<FarVolumeSpan> previous_span;
};
struct FarCacheReconciliation {
    FarCacheReconciliation() = default;
    FarCacheReconciliation(FarCacheReconciliation&&) noexcept = default;
    FarCacheReconciliation& operator=(FarCacheReconciliation&& other) noexcept {
        if (this != &other) {
            // Preserve the old pool and reservation until its old vectors are destroyed.
            FarCacheReconciliation previous(std::move(*this));
            memory = std::move(other.memory);
            reservation = std::move(other.reservation);
            due = std::move(other.due);
            changed = std::move(other.changed);
        }
        return *this;
    }
    std::shared_ptr<FarVolumeMemoryPool> memory;
    FarVolumeMemoryLease reservation;
    std::vector<FarCacheDueTile> due;
    std::vector<FarEditedFootprint> changed;
};
struct FarCacheReport {
    bool authoritative_success = false;
    bool derived_success = false, derived_due = true;
    FarCacheError refusal = FarCacheError::None;
    std::size_t rebuilt_tiles = 0;
    std::uint64_t committed_generation = 0;
};

// Public reconciliation accepts committed authority only. Refusal preserves out.
FarCacheError ReconcileFarEditedCache(const Persistence::EditedSourceImage& committed_image,
                                      const FarEditedCatalog&,
                                      const FarEditedCacheLimits&,
                                      FarCacheReconciliation& out);
// Owner thread, open transaction. Validates staged producer eligibility; the registered save
// finalizer reuses its owned capture.
// False means the outer save must abort. Safe optional failures return true with derived_due.
bool FinalizeFarEditedCache(Persistence::SaveTransaction&,
                            const Persistence::EditedSourceImage& prepared_image,
                            const std::filesystem::path& destination,
                            const FarEditedCacheLimits&,
                            FarCacheReport&);

struct FarCommittedCacheSelection {
    std::filesystem::path save_root;
    Persistence::EditedSourceSelection source;
};
struct FarCommittedCache {
    FarCommittedCache() = default;
    FarCommittedCache(const FarCommittedCache&) = delete;
    FarCommittedCache& operator=(const FarCommittedCache&) = delete;
    // Declaration order keeps the pool and leases alive through payload destruction.
    std::shared_ptr<FarVolumeMemoryPool> memory;
    FarVolumeMemoryLease reservation;
    std::shared_ptr<const Persistence::EditedSourceImage> source;
    FarEditedCatalog catalog;
    std::vector<FarOverlayTile> tiles;
    FarCacheReport report;
};
FarCacheError AcquireFarCommittedCache(const FarCommittedCacheSelection&,
                                       std::uint64_t expected_generation,
                                       const FarEditedCacheLimits&,
                                       std::shared_ptr<const FarCommittedCache>& out);

struct FarEditedCachePolicy {
    // Canonical complete tile supports, including retained extents. Unselected tiles stay due
    // until requested. A pending edited tile may never be substituted with pristine authority.
    std::vector<Persistence::EditedSourceTileSupport> selection;
    FarEditedCacheLimits limits;
    std::function<void(const FarCacheReport&)> after_commit;
};

// A synchronous opt-in persistence boundary. Nested writers for this explicit destination
// join one finalizer; no rendering or simulation option is changed. The policy is copied into
// admitted transaction storage, so callers may release their scope after staging.
class FarEditedCacheSaveScope {
public:
    FarEditedCacheSaveScope(std::filesystem::path destination,
                            std::shared_ptr<const FarEditedCachePolicy>);
    ~FarEditedCacheSaveScope();
    FarEditedCacheSaveScope(const FarEditedCacheSaveScope&) = delete;
    FarEditedCacheSaveScope& operator=(const FarEditedCacheSaveScope&) = delete;
    static std::shared_ptr<const FarEditedCachePolicy> PolicyFor(const std::filesystem::path&);

private:
    std::filesystem::path destination_;
    std::shared_ptr<const FarEditedCachePolicy> policy_;
    FarEditedCacheSaveScope* previous_ = nullptr;
};
bool RegisterFarEditedCacheFinalizer(Persistence::SaveTransaction&,
                                     std::shared_ptr<const FarEditedCachePolicy>);

} // namespace Luminumbra::World
