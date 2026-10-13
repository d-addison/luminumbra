#pragma once

#include "luminumbra_client/rendering/RenderPipeline.h"

#include <set>

namespace Luminumbra::Rendering {

struct TerrainCullingTestPeer {
    static std::set<ChunkID> Cull(const std::vector<IVec3>& coordinates,
                                  const glm::vec4 (&planes)[6]) {
        std::vector<RenderPipeline::ChunkMeshSnapshot> snapshots;
        for (const auto& coords : coordinates) {
            RenderPipeline::ChunkMeshSnapshot snapshot;
            snapshot.id = Chunk::calculate_id(coords);
            snapshot.coords = coords;
            snapshots.push_back(snapshot);
        }
        RenderPipeline::HierarchicalCuller culler;
        culler.BuildHierarchy(snapshots);
        std::vector<const RenderPipeline::ChunkCullEntry*> visible;
        culler.CullHierarchical(planes, visible);
        std::set<ChunkID> result;
        for (const auto* chunk : visible)
            result.insert(chunk->id);
        return result;
    }
};

} // namespace Luminumbra::Rendering
