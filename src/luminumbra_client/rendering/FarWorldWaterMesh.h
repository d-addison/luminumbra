#pragma once

#include "FarVolumeRenderMesh.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <memory>

namespace Luminumbra::Rendering {
inline constexpr std::size_t kFarWorldWaterSamples = 129 * 129;
inline constexpr std::size_t kFarWorldWaterCells = 128 * 128;
inline constexpr std::uint8_t kFarWorldWaterMaterial = 200;

using FarWorldWaterCapability = World::FarFrozenWaterCapability;
using FarWorldWaterSample = World::FarFrozenWaterSample;
using FarWorldWaterBound = World::FarFrozenWaterBound;
// Owned render authority: pure, immutable and worker safe, including reachable
// data. Identity is the combined terrain/water content token, never a live world
// or WaterSystem version. Dry corners also supply a finite boundary-cell level.
class FarWorldWaterObservation {
public:
    virtual ~FarWorldWaterObservation() = default;
    virtual std::uint64_t content_identity() const noexcept = 0;
    virtual FarWorldWaterCapability WaterCapability() const noexcept {
        return FarWorldWaterCapability::Unavailable;
    }
    virtual std::optional<FarWorldWaterBound> WaterBound(const World::FarTileKey&) const {
        return std::nullopt;
    }
    virtual std::optional<FarWorldWaterSample> WaterSample(std::int64_t, std::int64_t) const {
        return std::nullopt;
    }
};

// Production bridge retains only frozen common data. A prepared image or a
// partial tile support is unavailable to the renderer, never implicitly dry.
class FarWorldFrozenWaterObservation final : public FarWorldWaterObservation {
public:
    FarWorldFrozenWaterObservation(std::shared_ptr<const World::FarFrozenObservation> image,
                                   const World::FarTileKey& key)
        : image_(std::move(image))
        , key_(key) {
        const auto tier = World::FarTierAt(key.tier);
        if (!image_ || !tier ||
            image_->durable_identity().state != World::FarEditedImageState::Committed)
            return;
        const auto edge = std::int64_t(tier->tile_edge_meters);
        const auto limit = World::kFarVolumeExactCoordinateLimitMeters / edge;
        if (key.tx <= -limit || key.tx >= limit || key.tz <= -limit || key.tz >= limit)
            return;
        for (std::size_t i = 0; i < image_->region_count(); ++i) {
            const auto region = image_->Region(i);
            if (region->tier == key.tier && region->support.min.x == key.tx * edge &&
                region->support.min.z == key.tz * edge &&
                region->support.max.x == (key.tx + 1) * edge &&
                region->support.max.z == (key.tz + 1) * edge) {
                region_ = i;
                break;
            }
        }
    }
    std::uint64_t content_identity() const noexcept override {
        return image_ ? image_->content_identity() : 0;
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return region_ ? image_->WaterCapability(*region_) : FarWorldWaterCapability::Unavailable;
    }
    std::optional<FarWorldWaterBound> WaterBound(const World::FarTileKey& key) const override {
        return region_ && key == key_ ? image_->WaterBound(*region_) : std::nullopt;
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t x, std::int64_t z) const override {
        return region_ ? image_->WaterSample(*region_, x, z) : std::nullopt;
    }

private:
    std::shared_ptr<const World::FarFrozenObservation> image_;
    World::FarTileKey key_;
    std::optional<std::size_t> region_;
};
// Explicit analytic authority for dry/constant-height (including sea) fixtures.
// The caller supplies the combined content identity and the immutable level.
class FarWorldConstantWaterObservation final : public FarWorldWaterObservation {
public:
    FarWorldConstantWaterObservation(std::uint64_t identity, bool wet, float height)
        : identity_(identity)
        , wet_(wet)
        , height_(height) {}
    std::uint64_t content_identity() const noexcept override {
        return identity_;
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return wet_ ? FarWorldWaterCapability::Wet : FarWorldWaterCapability::ExplicitlyDry;
    }
    std::optional<FarWorldWaterBound> WaterBound(const World::FarTileKey&) const override {
        return FarWorldWaterBound{!wet_, height_, height_};
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t, std::int64_t) const override {
        return FarWorldWaterSample{wet_, height_};
    }

private:
    std::uint64_t identity_;
    bool wet_;
    float height_;
};
struct FarWorldWaterLimits {
    std::size_t samples = kFarWorldWaterSamples, cells = kFarWorldWaterCells;
    std::size_t vertices = 0, indices = 0;
    std::uint64_t bytes = 0;
};
enum class FarWorldWaterError {
    None,
    Unavailable,
    InvalidObservation,
    SpanRefused,
    WorkLimit,
    ByteLimit,
    InvalidMemory,
    AllocationFailure
};
bool ValidFarWorldWaterBound(const FarWorldWaterBound& bound) noexcept;
// Transactional combined conversion. Terrain and its lease remain charged while
// samples/scratch and the replacement are built. No partial output on refusal.
bool BuildFarWorldWaterMesh(const FarVolumeRenderMesh& terrain,
                            const FarWorldWaterObservation& water,
                            const FarWorldWaterBound& bound,
                            const FarWorldWaterLimits& limits,
                            const World::FarVolumeMemory& memory,
                            FarVolumeRenderMesh& output,
                            FarWorldWaterError* error = nullptr);
} // namespace Luminumbra::Rendering
