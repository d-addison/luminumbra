#pragma once

#include "FarEditedBake.h"
#include "FarVolume.h"

#include <memory>

namespace Luminumbra::World {

enum class FarFrozenUse : std::uint8_t {
    Committed,
    Preview
};
enum class FarFrozenWaterCapability : std::uint8_t {
    Unavailable,
    ExplicitlyDry,
    Wet
};
struct FarFrozenWaterSample {
    bool wet = false;
    float surface_height = 0;
    bool operator==(const FarFrozenWaterSample&) const = default;
};
struct FarFrozenWaterBound {
    bool known_dry = false;
    float min_y = 0, max_y = 0;
    bool operator==(const FarFrozenWaterBound&) const = default;
};

// An absent override is different from unreadable expected authority.
enum class FarFrozenPresence : std::uint8_t {
    Absent,
    Present,
    Unavailable
};
template<class T>
struct FarFrozenOverride {
    FarFrozenPresence presence = FarFrozenPresence::Absent;
    T sample{};
};
using FarFrozenTerrainOverride =
    std::function<FarFrozenOverride<FarVolumeSample>(std::uint8_t, const FarVolumePosition&)>;
using FarFrozenWaterOverride =
    std::function<FarFrozenOverride<FarFrozenWaterSample>(std::int64_t, std::int64_t)>;

// Capture-only callbacks. The host holds its owner-thread publication boundary
// throughout capture. None of these callbacks or their reachable data is retained.
// The committed layer must come from eligible immutable producer images, including
// disk-only records (prepared images are bake-only). Each layer uses one coordinate
// resolver for shared boundaries and reports conflicting authority as unavailable.
// Identity integers alone do not establish that eligibility.
struct FarFrozenCaptureSource {
    FarVolumeAuthority pristine;
    FarEditedImageIdentity durable;
    // Base terrain/water token. Capture folds its canonical observations into a
    // separate render fingerprint; it never changes durable.generation.
    std::uint64_t content_identity = 0;
    FarFrozenUse use = FarFrozenUse::Committed;
    // Renderer-only capture keeps the LegacySigned16 density/material consumed by meshes.
    // The default dual-image capture and its durable bake semantics are unchanged.
    bool render_only = false;
    // Required committed resolver, including an explicit Absent result for an
    // empty committed layer. A missing resolver cannot prove edits are absent.
    FarFrozenTerrainOverride committed;
    FarFrozenTerrainOverride parked, live;
    // Required explicit committed water truth, even for dry worlds.
    std::function<std::optional<FarFrozenWaterSample>(std::int64_t, std::int64_t)> water;
    FarFrozenWaterOverride parked_water, live_water;
};

// Closed, spacing-aligned finite support. Capture adds one sample on every axis
// for central-difference normals. Full rectangular halos include corner samples.
struct FarFrozenRegion {
    std::uint8_t tier = 0;
    FarEditedFootprint support;
};
struct FarFrozenCaptureLimits {
    // Required finite caps; zero admits no work. Counts include rectangular halos
    // and worst-case callback invocations through every enabled override layer.
    std::size_t max_regions = 0;
    std::uint64_t max_height_samples = 0, max_density_samples = 0, max_water_samples = 0;
    // Image, shared ownership control block, and exact vector capacities.
    std::uint64_t max_bytes = 0;
};
enum class FarFrozenCaptureError : std::uint8_t {
    None,
    InvalidSource,
    InvalidRegion,
    CoordinateRange,
    WorkLimit,
    ByteLimit,
    Unavailable,
    NonFiniteSample,
    IdentityMismatch,
    AllocationFailure
};

class FarFrozenObservation final : public std::enable_shared_from_this<FarFrozenObservation> {
    struct CaptureToken {};

public:
    FarFrozenObservation(CaptureToken,
                         std::shared_ptr<FarVolumeMemoryPool> pool,
                         const FarFrozenCaptureSource& source);
    FarFrozenObservation(const FarFrozenObservation&) = delete;
    FarFrozenObservation& operator=(const FarFrozenObservation&) = delete;
    std::uint64_t content_identity() const noexcept {
        return identity_;
    }
    FarEditedImageIdentity durable_identity() const noexcept {
        return durable_;
    }
    bool render_only() const noexcept {
        return render_only_;
    }
    FarFrozenUse use() const noexcept {
        return use_;
    }
    // Allocation lifetime stays with the image, including every worker alias.
    const FarVolumeMemoryPool* memory_pool() const noexcept {
        return pool_.get();
    }
    std::shared_ptr<FarVolumeMemoryPool> memory_pool_handle() const noexcept {
        return pool_;
    }
    // Sample/vector payload only; the shared allocation is also charged in pool.
    std::uint64_t payload_bytes() const noexcept {
        return lease_.bytes();
    }
    FarCaveMode caves() const noexcept {
        return caves_;
    }
    FarVolumeNumericProfile numeric() const noexcept {
        return numeric_;
    }
    std::size_t region_count() const noexcept {
        return grids_.size();
    }
    std::optional<FarFrozenRegion> Region(std::size_t region) const;
    std::optional<float> Height(std::uint8_t tier, std::int64_t x, std::int64_t z) const;
    std::optional<FarVolumeSample>
    Sample(std::uint8_t tier, const FarVolumePosition& p, bool pristine = false) const;
    FarFrozenWaterCapability WaterCapability(std::size_t region) const noexcept;
    std::optional<FarFrozenWaterBound> WaterBound(std::size_t region) const;
    std::optional<FarFrozenWaterSample>
    WaterSample(std::size_t region, std::int64_t x, std::int64_t z) const;
    // These adapters retain this immutable image, never an owner callback.
    FarVolumeAuthority Authority(std::uint8_t tier) const;
    FarEditedPristineSampler PristineSampler() const;

private:
    friend FarFrozenCaptureError
    CaptureFarFrozenObservation(std::span<const FarFrozenRegion>,
                                const FarFrozenCaptureSource&,
                                const FarFrozenCaptureLimits&,
                                const std::shared_ptr<FarVolumeMemoryPool>&,
                                std::shared_ptr<const FarFrozenObservation>&);
    struct Grid {
        FarFrozenRegion region;
        FarVolumePosition origin;
        std::array<std::size_t, 3> side{};
        std::size_t column_offset = 0, sample_offset = 0;
        FarFrozenWaterBound water_bound;
    };
    // Pool ownership outlives both the payload and its lease, including workers.
    std::shared_ptr<FarVolumeMemoryPool> pool_;
    FarVolumeMemoryLease lease_;
    std::uint64_t identity_, pristine_identity_;
    FarEditedImageIdentity durable_;
    FarFrozenUse use_;
    bool render_only_ = false;
    FarCaveMode caves_;
    FarVolumeNumericProfile numeric_;
    std::vector<Grid> grids_;
    std::vector<float> heights_;
    std::vector<FarVolumeSample> pristine_, composed_;
    std::vector<std::int16_t> render_density_;
    std::vector<std::uint8_t> render_material_;
    template<class T>
    struct RenderRun {
        std::size_t end = 0;
        T value = 0;
    };
    std::vector<RenderRun<std::int16_t>> render_density_runs_;
    std::vector<RenderRun<std::uint8_t>> render_material_runs_;
    std::vector<FarFrozenWaterSample> water_;
};

// Transactional admission and capture. A pool is required and is retained by the
// image. Refusal leaves the previous image and its charge intact. No simulation,
// save, queue, GL or active-region operation is performed.
FarFrozenCaptureError
CaptureFarFrozenObservation(std::span<const FarFrozenRegion> regions,
                            const FarFrozenCaptureSource& source,
                            const FarFrozenCaptureLimits& limits,
                            const std::shared_ptr<FarVolumeMemoryPool>& pool,
                            std::shared_ptr<const FarFrozenObservation>& output);

// Uses the existing edited-image/bake contract unchanged. The source identity
// and LegacySigned16 profile must match capture; unsaved composed observations
// never enter the bake sampler.
FarEditedBakeError BakeFarEditedTilesFrozen(const FarEditedSourceView& source,
                                            std::span<const FarEditedFootprint> changed,
                                            const FarFrozenObservation& frozen,
                                            const FarEditedBakeLimits& limits,
                                            FarEditedBakeResult& output);

} // namespace Luminumbra::World
