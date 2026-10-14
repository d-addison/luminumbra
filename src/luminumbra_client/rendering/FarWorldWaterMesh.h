#pragma once

#include "FarVolumeRenderMesh.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <memory>

namespace Luminumbra::Rendering {
inline constexpr std::size_t kFarWorldWaterSamples = 129 * 129;
inline constexpr std::size_t kFarWorldWaterCells = 128 * 128;
inline constexpr std::uint8_t kFarWorldWaterMaterial = 200;

enum class FarWorldWaterCapability {
    Unavailable,
    ExplicitlyDry,
    Wet
};
struct FarWorldWaterSample {
    bool wet = false;
    float surface_height = 0;
};
struct FarWorldWaterBound {
    bool known_dry = false;
    float min_y = 0, max_y = 0;
    bool operator==(const FarWorldWaterBound&) const = default;
};
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
