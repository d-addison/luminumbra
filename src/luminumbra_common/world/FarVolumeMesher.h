#pragma once

#include "FarVolumeTile.h"

namespace Luminumbra::World {

using FarVolumeVector = std::array<double, 3>;

struct FarVolumeVertex {
    // Metres relative to FarVolumeMesh::origin_meters, never float world positions.
    FarVolumeVector position{}, normal{};
    std::uint8_t material = 0;
    bool operator==(const FarVolumeVertex&) const = default;
};

struct FarVolumeMeshBounds {
    FarVolumeVector min{}, max{}; // Actual used vertices, relative to origin_meters.
    bool operator==(const FarVolumeMeshBounds&) const = default;
};

struct FarVolumeMesh {
    FarVolumeRequest request;
    FarVolumePosition origin_meters, sampled_max_meters;
    FarVolumeContent content = FarVolumeContent::Air;
    std::uint64_t cells_visited = 0, samples_evaluated = 0;
    std::vector<FarVolumeVertex> vertices;
    std::vector<std::uint32_t> indices;
    // Empty geometry has no geometric bounds but retains its sampled span/content.
    std::optional<FarVolumeMeshBounds> bounds;
    bool operator==(const FarVolumeMesh&) const = default;
};

struct FarVolumeMeshLimits {
    // Required caller limits, not runtime byte/frame caps. Each resident brick
    // accounts for 64 cells and validation of its 125 resident samples.
    std::uint64_t max_cells = 0, max_sample_evaluations = 0;
    std::size_t max_vertices = 0, max_indices = 0;
};

enum class FarVolumeMeshError {
    None,
    InvalidTile,
    InvalidSampler,
    FieldIdentityMismatch,
    SampleMismatch,
    NonFiniteDensity,
    CoordinateOverflow,
    CellLimit,
    SampleLimit,
    VertexLimit,
    IndexLimit,
    SamplerFailure,
    ZeroGradient,
    AllocationFailure
};

// CPU Marching Cubes entry point. The sampler must supply the same immutable
// authority as generation, including a one-sample halo at the tier spacing.
// Its identity and every resident sample (density bits and material) are checked.
// Unique sampled coordinates are cached and charged once. Central gradients at
// lattice endpoints interpolate along canonical edges, including tile borders.
// The sampler must remain bounded/pure; nullopt can request cancellation.
// Invalid metadata, exhausted limits, unrepresentable halo coordinates or a zero
// surface gradient refuse the complete operation without replacing output.
// No runtime scheduling, rendering, cave filtering or format state is accessed.
bool MeshFarVolumeTile(const FarVolumeTile& tile,
                       std::uint64_t sampler_field_identity,
                       const FarVolumeSampler& sampler,
                       const FarVolumeMeshLimits& limits,
                       FarVolumeMesh& output,
                       FarVolumeMeshError* error = nullptr);

} // namespace Luminumbra::World
