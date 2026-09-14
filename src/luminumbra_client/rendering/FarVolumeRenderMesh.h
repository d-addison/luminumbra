#pragma once

#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarVolume.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Luminumbra::Rendering {

struct FarVolumeMeshBounds {
    Vec3 min, max;
};

// Absolute world positions. A future draw must not add the legacy region origin.
// Empty meshes have no geometry bounds; their sampled span lives in the receipt.
struct FarVolumeRenderMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::optional<FarVolumeMeshBounds> bounds;

    std::size_t owned_bytes() const;
};

inline constexpr std::size_t kFarVolumeRenderMeshBudget = 64u * 1024u * 1024u;

// Validates every position/index before allocation, preserves winding/materials,
// and expands nondegenerate triangles with outward flat face normals. Throws on
// invalid input or insufficient budget; never returns a partial mesh.
FarVolumeRenderMesh
AdaptFarVolumeRenderMesh(const World::FarVolumeMesh& source,
                         std::size_t max_buffer_bytes = kFarVolumeRenderMeshBudget);

} // namespace Luminumbra::Rendering
