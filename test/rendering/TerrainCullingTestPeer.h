#pragma once

#include "luminumbra_client/rendering/RenderPipeline.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

namespace Luminumbra::Rendering {

struct TerrainCullingTestPeer {
    static GBufferPass& GBuffer(RenderPipeline& pipeline) {
        return *pipeline.m_gbuffer_pass;
    }
    static void SelectFarWorldFrame(RenderPipeline& pipeline,
                                    FarWorldSelection selection,
                                    const Camera& camera) {
        pipeline.select_frame_far_world(selection, camera);
    }
    static void PrepareCamera(RenderPipeline& pipeline,
                              const Camera& camera,
                              u32 width,
                              u32 height,
                              glm::vec2 jitter,
                              glm::vec4 (&planes)[6]) {
        pipeline.m_screen_width = width;
        pipeline.m_screen_height = height;
        pipeline.m_taau_jitter_ndc = jitter;
        pipeline.prepare_camera_projection(camera);
        std::copy_n(pipeline.m_frame_prepared.frustum_planes, 6, planes);
    }
    static nlohmann::json CaptureCamera(const RenderPipeline& pipeline) {
        return pipeline.terrain_coverage_camera_metadata();
    }

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
