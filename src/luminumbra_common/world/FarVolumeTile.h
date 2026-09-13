#pragma once

#include "FarTierTable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace Luminumbra::World {

struct FarVolumePosition {
    std::int64_t x = 0, y = 0, z = 0;
    bool operator==(const FarVolumePosition&) const = default;
};

struct FarVolumeTileKey {
    std::size_t tier = 0;      // One-based FarTierTable index; zero is invalid.
    std::int64_t x = 0, z = 0; // Tile coordinates, not metres or chunk IDs.
    bool operator==(const FarVolumeTileKey&) const = default;
};

struct FarVolumeSpan {
    // Brick-aligned world metres. Brick origins occupy [min_y, max_y);
    // sampling includes max_y as the shared upper boundary.
    std::int64_t min_y = 0, max_y = 0;
    bool operator==(const FarVolumeSpan&) const = default;
};

struct FarVolumeRequest {
    FarVolumeTileKey key;
    FarVolumeSpan span;
    // Caller-owned identity of the immutable field (e.g. seed/parameter hash).
    // It is carried unchanged; it does not select or validate a density policy.
    std::uint64_t field_identity = 0;
    bool operator==(const FarVolumeRequest&) const = default;
};

struct FarVolumeSample {
    float density = 0.0f;      // Finite; negative is solid, zero/nonnegative is outside.
    std::uint8_t material = 0; // Opaque caller-defined material value.
    bool operator==(const FarVolumeSample&) const = default;
};

inline constexpr std::size_t kFarVolumeBrickSide = 5;
inline constexpr std::size_t kFarVolumeBrickSamples = 125;

static_assert([] {
    for (const auto& tier : kFarTierTable) {
        if (tier.brick_edge_meters != (kFarVolumeBrickSide - 1) * tier.sample_spacing_meters ||
            tier.brick_edge_meters < 16 ||
            (tier.tile_edge_meters & (tier.tile_edge_meters - 1)) != 0) {
            return false;
        }
    }
    return true;
}());

struct FarVolumeBrick {
    FarVolumePosition origin_meters;
    // x fastest, then y, then z: x + 5 * (y + 5 * z).
    std::array<FarVolumeSample, kFarVolumeBrickSamples> samples;
    bool operator==(const FarVolumeBrick&) const = default;
};

enum class FarVolumeContent {
    Air,
    Solid,
    Mixed
};

struct FarVolumeTile {
    FarVolumeRequest request;
    FarVolumePosition min_meters, max_meters; // Inclusive sampled bounds.
    FarVolumeContent content = FarVolumeContent::Air;
    std::uint64_t candidates_visited = 0;
    std::uint64_t samples_evaluated = 0;
    // Only bricks containing both negative and nonnegative samples, in (z,x,y)
    // origin order. Empty Air/Solid tiles are successful generation results.
    std::vector<FarVolumeBrick> bricks;
    bool operator==(const FarVolumeTile&) const = default;
};

struct FarVolumeBuildLimits {
    // Required caller limits, not approved runtime residency/performance caps.
    // Work is exactly 125 sampler calls per candidate, including shared borders.
    std::uint64_t max_candidate_bricks = 0;
    std::size_t max_surface_bricks = 0;
};

enum class FarVolumeBuildError {
    None,
    InvalidTier,
    InvalidSpan,
    CoordinateOverflow,
    WorkLimit,
    SurfaceLimit,
    InvalidSampler,
    SamplerFailure,
    NonFiniteDensity,
    AllocationFailure
};

// A sampler must be a bounded, pure function of world-coordinate input and its
// immutable field identity; repeated border coordinates must produce identical
// values. nullopt (including cancellation) or an exception refuses the build.
using FarVolumeSampler = std::function<std::optional<FarVolumeSample>(const FarVolumePosition&)>;

// CPU production entry point. No world/GL/persistence state is accessed. Bounds
// and work limits are checked before the first sample. Output is replaced only
// after complete success; errors leave the previous tile unchanged. Field
// filtering, requested-span discovery and cache identity policy belong to the
// caller. This API neither serializes FSV1 nor activates the runtime ladder.
bool BuildFarVolumeTile(const FarVolumeRequest& request,
                        const FarVolumeBuildLimits& limits,
                        const FarVolumeSampler& sampler,
                        FarVolumeTile& output,
                        FarVolumeBuildError* error = nullptr);

} // namespace Luminumbra::World
