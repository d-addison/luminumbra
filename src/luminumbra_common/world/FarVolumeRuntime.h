#pragma once

#include "FarVolume.h"
#include "FarWantedSet.h"

namespace Luminumbra::World {

// Runtime keys address the same tier table as the canonical volume model.
// Conversion also admits the complete horizontal tile plus its normal halo in
// the facade's exact-coordinate domain. Refusal leaves output unchanged.
bool ToFarVolumeTileKey(const FarTileKey& key, FarVolumeTileKey& output);
bool ToFarTileKey(const FarVolumeTileKey& key, FarTileKey& output);

inline constexpr std::int64_t kFarVolumeMaxPageStrideMeters = 4096;
inline constexpr std::uint64_t kFarVolumeMaxPages = 1024;

struct FarVolumePageWork {
    std::uint64_t candidates = 0, generation_samples = 0, mesh_samples = 0;
    std::uint64_t height_samples = 0, density_samples = 0, cells = 0;
    std::uint64_t vertices = 0, indices = 0;
    std::uint64_t sample_cache_entries = 0, gradient_cache_entries = 0, edge_cache_entries = 0;
    // Contiguous buffer peak, including simultaneous old/new growth buffers,
    // height cache and both ordering scratch buffers. Map nodes are separately
    // bounded by entry counts: their allocation sizes depend on the library ABI.
    std::uint64_t buffer_bytes = 0;
    bool operator==(const FarVolumePageWork&) const = default;
};

struct FarVolumePageBudget {
    std::uint64_t max_candidates = 0, max_sample_calls = 0;
    std::uint64_t max_buffer_bytes = 0, max_cache_entries = 0;
};

// Dormant, test-only caller contract. No queue, identity or format integration.
// Stores one request and grid descriptor, never a page container. All operations
// are allocation-free O(1). Reset/lookup/admission refusals preserve their output.
// Stride is a positive brick multiple, at most 4096 m; page_cap is in [1,1024].
// The facade's exact-coordinate domain includes the complete normal halo.
class FarVolumePagePlan {
public:
    bool Reset(const FarVolumeRequest& request,
               std::int64_t stride_meters,
               std::uint64_t page_cap) noexcept;
    std::uint64_t size() const noexcept {
        return count_;
    }
    // Ascending world-zero-anchored windows, clipped only at the outer bounds.
    bool Page(std::uint64_t index, FarVolumeRequest& output) const noexcept;
    // Worst single-page work for sequential compilation, assuming every candidate
    // is mixed. Includes 125 generation calls per candidate, cached mesher halo
    // samples, gradients, edges and up to five triangles per cell. This admits
    // counts and contiguous buffers, not map-node bytes or retained other pages;
    // actual allocations must still pass through the canonical charged APIs.
    bool Admit(const FarVolumePageBudget& budget, FarVolumePageWork& output) const noexcept;

private:
    FarVolumeRequest request_;
    std::int64_t stride_ = 0, first_ = 0;
    std::uint64_t count_ = 0;
};

// Exact ordered fold over a copied immutable plan. A refused acknowledgement
// leaves progress unchanged; Complete requires every full request, including its
// key/field identity, exactly once. An empty/uninitialized plan is incomplete.
class FarVolumePageCompleteness {
public:
    explicit FarVolumePageCompleteness(const FarVolumePagePlan& plan) noexcept
        : plan_(plan) {}
    bool Acknowledge(const FarVolumeRequest& page) noexcept;
    bool Complete() const noexcept {
        return plan_.size() != 0 && next_ == plan_.size();
    }
    std::uint64_t accepted() const noexcept {
        return next_;
    }

private:
    FarVolumePagePlan plan_;
    std::uint64_t next_ = 0;
};

struct FarVolumeSpanLedgerLimits {
    // Explicit caller limits on requested extensions, not shipped runtime caps.
    float min_y = 0, max_y = 0;
    float max_extent_meters = 0;
};

// Allocation-free per-tile requested-extension ledger. The facade still unions
// this extension with the complete pristine surface band. Camera, anchor,
// sightline and durable-chunk requests all use Extend; accepted requests never
// shrink until Reset at a new world binding. The owner must retain world_epoch
// in FarBuildIdentity when resetting so prior results cannot become current.
class FarVolumeSpanLedger {
public:
    bool Reset(const FarTileKey& key);
    bool Extend(const FarVolumeCoverageSpan& request, const FarVolumeSpanLedgerLimits& limits);
    std::optional<FarVolumeCoverageIntent> Intent() const;
    std::uint64_t generation() const noexcept {
        return generation_;
    }
    // Whether a resolved build span contains every accepted extension. Currency
    // additionally requires the complete FarBuildIdentity to match.
    bool Covers(const FarVolumeSpan& built) const;

private:
    std::optional<FarVolumeTileKey> key_;
    std::optional<FarVolumeCoverageSpan> extra_;
    std::uint64_t generation_ = 0;
};

} // namespace Luminumbra::World
