#pragma once

#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarVolumeMesher.h"

namespace Luminumbra::Rendering {

struct FarVolumeRenderBounds {
    Vec3 min{}, max{};
};

// Positions remain relative to origin_meters; the draw adds origin relative to
// the camera. Shared indices, winding, canonical gradient normals and materials
// survive conversion. Empty geometry retains its sampled span/content metadata.
struct FarVolumeRenderMesh {
    World::FarVolumeRequest request;
    World::FarVolumePosition origin_meters, sampled_max_meters;
    World::FarVolumeContent content = World::FarVolumeContent::Air;
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::optional<FarVolumeRenderBounds> bounds;
};

enum class FarVolumeRenderError {
    None,
    InvalidSource,
    ByteLimit,
    InvalidMemory,
    AllocationFailure
};

std::uint64_t FarVolumeRenderRetainedBytes(const FarVolumeRenderMesh& mesh);

// CPU conversion only: no GL upload and no default byte cap. A bound pool and
// retained lease paired with output are mandatory. Source compilation remains
// charged separately while conversion builds new buffers; the prior output and
// its lease are replaced together only after complete success.
bool AdaptFarVolumeRenderMesh(const World::FarVolumeMesh& source,
                              std::uint64_t max_buffer_bytes,
                              const World::FarVolumeMemory& memory,
                              FarVolumeRenderMesh& output,
                              FarVolumeRenderError* error = nullptr);

} // namespace Luminumbra::Rendering
