#pragma once

#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/passes/ShadowPass.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

namespace Luminumbra::Rendering {

struct TerrainCullingTestPeer {
    // Shadow exclusion qualification: initialize only the real shadow resources
    // and enter the production stage, including its production terrain submitter.
    // Test-only access through the existing peer; no shipping behavior changes.
    static ShadowPass& InitShadow(RenderPipeline& pipeline,
                                  const std::filesystem::path& root,
                                  u32 resolution) {
        pipeline.m_screen_width = pipeline.m_internal_width = resolution;
        pipeline.m_screen_height = pipeline.m_internal_height = resolution;
        auto& pass = *pipeline.m_shadow_pass;
        pass.shadow_map().resolution = resolution;
        pass.init_shader(root);
        pass.init_shadow_map(pipeline.m_render_registry);
        return pass;
    }
    static void ExecuteShadow(RenderPipeline& pipeline, const Camera& camera) {
        pipeline.m_frame_far_selection =
            pipeline.m_far_world->Selection(FarWorldPurpose::World);
        camera.SelectFarWorldPlane(pipeline.m_frame_far_selection.effective);
        pipeline.execute_stage_shadow(camera);
    }
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
