#pragma once

// Discovery and pristine authority facade over the ONE canonical volume model.
// These declarations do not replace format readers or connect runtime queues.
#include "FarVolumeMesher.h"
#include <glm/vec3.hpp>

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::World {

enum class FarCaveMode : std::uint8_t {
    BandLimited,
    BoxFiltered
};

enum class FarVolumeNumericProfile : std::uint8_t {
    RawFloat,
    LegacySigned16
};

inline constexpr float kFarVolumeInteriorDepthMeters = 256.0f;
inline constexpr std::int64_t kFarVolumeExactCoordinateLimitMeters = 1 << 23;

// Unresolved surface-coverage extension, retaining the original float bounds.
// Unlike FarVolumeSpan, this need not be aligned and a single height is valid.
struct FarVolumeCoverageSpan {
    float min_y = 0, max_y = 0;
    bool operator==(const FarVolumeCoverageSpan&) const = default;
};
struct FarVolumeCoverageIntent {
    FarVolumeTileKey key;
    // Extends the surface band; never replaces or truncates its lower 256 m.
    std::optional<FarVolumeCoverageSpan> extra_span;
    bool operator==(const FarVolumeCoverageIntent&) const = default;
};

// Copied once per operation. Callbacks and everything they observe must remain
// bounded/pure and immutable throughout discovery, generation and halo sampling.
// The owner supplies the complete field identity including cave/numeric policy;
// a token alone is not proof that a saved world is pristine or edits are absent.
struct FarVolumeAuthority {
    std::uint64_t field_identity = 0;
    FarCaveMode caves = FarCaveMode::BandLimited;
    FarVolumeNumericProfile numeric = FarVolumeNumericProfile::RawFloat;
    std::function<std::optional<float>(std::int64_t, std::int64_t)> height;
    std::function<std::optional<FarVolumeSample>(
        const FarVolumePosition&, float, std::uint32_t, FarCaveMode)>
        density;
};

struct FarVolumeAuthorityLimits {
    // Required limits on actual callback invocations, not approved runtime caps.
    // Height columns are cached through the one-sample horizontal halo. Density
    // callbacks include repeated generation borders and the mesher's own work.
    std::uint64_t max_height_samples = 0, max_density_samples = 0;
};
struct FarVolumeAuthorityWork {
    std::uint64_t height_samples = 0, density_samples = 0;
    bool operator==(const FarVolumeAuthorityWork&) const = default;
};
struct FarVolumeCompilationLimits {
    FarVolumeAuthorityLimits authority;
    FarVolumeBuildLimits generation;
    FarVolumeMeshLimits mesh;
};

enum class FarVolumeFacadeError {
    None,
    InvalidIntent,
    InvalidAuthority,
    CoordinateRange,
    HeightLimit,
    DensityLimit,
    SamplerFailure,
    NonFiniteSample,
    GenerationRefused,
    MeshRefused,
    InvalidCompatibilityTile,
    AllocationFailure
};
struct FarVolumeFacadeFailure {
    FarVolumeFacadeError code = FarVolumeFacadeError::None;
    FarVolumeBuildError generation = FarVolumeBuildError::None;
    FarVolumeMeshError mesh = FarVolumeMeshError::None;
    FarVolumeAuthorityWork work;
};

struct FarVolumeCompilation {
    FarVolumeCoverageIntent intent;
    FarCaveMode caves = FarCaveMode::BandLimited;
    FarVolumeNumericProfile numeric = FarVolumeNumericProfile::RawFloat;
    FarVolumeAuthorityWork work;
    FarVolumeTile tile;
    FarVolumeMesh mesh;
    // Original XYZ/signed16/little-endian integrity view, only for LegacySigned16.
    // Not a field identity, persistence record or checksum over object padding.
    std::optional<std::uint32_t> legacy_checksum;
    bool operator==(const FarVolumeCompilation&) const = default;
};

// Resolves 129x129 height samples, the 256 m lower band and requested extensions to
// the canonical aligned span. Includes an air boundary above an exact top plane.
// Every failure leaves output unchanged. Does not invoke the density callback.
bool ResolveFarVolumeCoverage(const FarVolumeCoverageIntent& intent,
                              const FarVolumeAuthority& authority,
                              const FarVolumeAuthorityLimits& limits,
                              FarVolumeRequest& output,
                              FarVolumeFacadeFailure* failure = nullptr);

// One authority instance supplies discovery, generation and normal halos.
// All outputs are committed together after success. The generic RawFloat profile
// is not quantized. No byte-reservation, queue, upload or runtime claim is made.
bool CompileFarVolume(const FarVolumeCoverageIntent& intent,
                      const FarVolumeAuthority& authority,
                      const FarVolumeCompilationLimits& limits,
                      FarVolumeCompilation& output,
                      FarVolumeFacadeFailure* failure = nullptr);

// Validates the canonical sparse stream before forming its legacy integrity
// view. Refuses non-encoded density, invalid keys/spans/order, or broken shared
// sample identity. Does not read/write or declare a new world format.
bool FarVolumeLegacyChecksum(const FarVolumeTile& tile, FarCaveMode caves, std::uint32_t& output);

// Original finite eight-midpoint box quadrature, accumulated in double in Z/Y/X
// order. Only the declared 16/32/64 m coarse spacings are valid; this is not an ideal
// spectral filter. The cave modes preserve their existing separate semantics.
float BoxFilterFarVolumeCarve(const glm::vec3& position,
                              std::uint32_t spacing,
                              const std::function<float(const glm::vec3&)>& carve);

// Synchronous pristine-field sampling only. Holds one world-generation scope
// across all phases, including normal halos. Callers must keep world alive and
// must not already hold that nonrecursive scope. R0 adaptation is separate.
bool CompilePristineFarVolume(const Systems::SHIELD_WorldSystem& world,
                              const FarVolumeCoverageIntent& intent,
                              std::uint64_t field_identity,
                              FarCaveMode caves,
                              FarVolumeNumericProfile numeric,
                              const FarVolumeCompilationLimits& limits,
                              FarVolumeCompilation& output,
                              FarVolumeFacadeFailure* failure = nullptr);

} // namespace Luminumbra::World
