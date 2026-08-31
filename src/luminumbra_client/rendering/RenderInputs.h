#pragma once

// the NON-RenderContext per-pass inputs (callbacks, std::function
// lookups, draw-item lists) that do not belong in the shared RenderContext
// field-union because they are pass-specific behavior/data, not frame state.
//
// Everything a converted pass reads that IS frame state (camera, screen, time,
// sun, terrain arrays, gbuffer handles, frustum_planes, taau jitter, prev view-
// proj, isolation) travels through RenderContext. These structs carry the rest.

#include "../../include/luminumbra/core/Types.h" // u32
#include "StaticModelTex.h" // namespace-scope StaticModelTex (the lookup return)
#include "TerrainSubmit.h"  // SubmitTerrainChunksFn + TerrainSubmitStats

#include <cstddef>     // std::size_t
#include <filesystem>  // std::filesystem::path (LOD mesh root)
#include <functional>  // std::function (static_model_tex lookup)
#include <glm/glm.hpp> // glm::mat4 (WaterDrawItem model)
#include <string>      // mesh-path lookup key
#include <vector>      // WaterDrawItem list

namespace Luminumbra::Rendering {

class FarLodSystem; // far-LOD region meshes drawn with the GBuffer pass's own shader

// ----------------------------------------------------------------------------
// GBuffer (T11) — non-ctx inputs for the deferred geometry pass.
// ----------------------------------------------------------------------------
// All of GBuffer's frame state (camera/screen/time_seconds/taau_jitter_ndc/
// prev_view_proj/prev_time/material_lut/terrain_*/skinned_*/isolation/
// frustum_planes) comes from RenderContext. These are the remaining reaches.
struct GBufferPassInput {
    // Live-terrain submit seam: wraps CullHierarchical + draw_chunks_mdi and
    // returns counts (NOT mutated inside the seam). The pass copies
    // ctx.frustum_planes into a local glm::vec4 fp[6] before calling this
    // (ctx.frustum_planes is a pointer; the callback takes an array reference).
    SubmitTerrainChunksFn submit_terrain_chunks;

    // Far-LOD region meshes drawn AFTER the live chunks, using the PASS's own
    // geometry shader (so they cannot ride submit_terrain_chunks). nullptr when
    // far-LOD is disabled -> the pass skips the block. Owned by RenderPipeline;
    // the pass only calls far_lod->draw_gbuffer(shader, view, planes, d, i).
    FarLodSystem* far_lod = nullptr;

    // Root path for resolving on-disk LOD mesh variants (procgen tree palette).
    std::filesystem::path root_path;

    //  static-model UV texture lane (bark/leaf plates). Raw GL array name +
    // per-meshPath layer/alpha lookup (returns nullptr when the mesh has none).
    u32 static_model_texture_array = 0;
    std::function<const StaticModelTex*(const std::string&)> static_model_tex;

    //  far-field tree impostors (opt-in; all 0/false when disabled).
    bool tree_impostor_enabled = false;
    u32 tree_impostor_albedo = 0; // raw GL texture name (bound directly)
    u32 tree_impostor_normal = 0; // raw GL texture name (bound directly)
    int tree_impostor_grid = 0;
    float tree_impostor_radius = 0.0f;
    float tree_impostor_sphere_y = 0.0f;
};

// Returned by GBufferPass::execute so the call site applies the EXACT current
// stats policy: terrain_visible_chunks assigned with '=', every other counter
// accumulated with '+=' (mirrors GBufferPass.cpp geometry_pass_chunks /
// geometry_pass_skinned_meshes today).
struct GBufferDrawStats {
    std::size_t terrain_visible_chunks = 0; // call site: '=' (terrain_visible_chunks)
    std::size_t terrain_draws = 0;          // call site: '+=' (terrain_draws)
    std::size_t terrain_indices = 0;        // call site: '+=' (terrain_indices_drawn)
    std::size_t far_region_draws = 0;       // call site: '+=' (far_region_draws)
    std::size_t far_indices = 0;            // call site: '+=' (far_indices_drawn)
    std::size_t skinned_draws = 0;          // call site: '+=' (skinned_draws)
    std::size_t skinned_indices = 0;        // call site: '+=' (skinned_indices_drawn)
};

// ----------------------------------------------------------------------------
// Water (T16) — non-ctx inputs for the forward water pass.
// ----------------------------------------------------------------------------
// One visible water mesh, resolved at the call site from m_water_render_data +
// the renderable chunk snapshots. The pass NEVER touches ChunkMeshSnapshot or
// m_water_render_data; it iterates draw_items IN VECTOR ORDER, which reproduces
// the current renderable_chunks iteration order (so draw order is byte-stable).
struct WaterDrawItem {
    glm::mat4 model = glm::mat4(1.0f); // translate(chunk.coords * CHUNK_SIZE_{X,Y,Z})
    u32 vao_id = 0;                    // WaterRenderData::vao_id
    u32 element_count = 0; // WaterRenderData::element_count (GL_UNSIGNED_INT count, > 0)
};

// Water reads sun direction/color/intensity from ctx.sun (RenderContext Group H,
// DirectionalLight by value) -- the same source make_particle_context /
// make_foliage_context already populate. The pass also reads ctx.time_seconds
// (the per-frame wall-clock snapshot) in place of the two live glfwGetTime
// calls, ctx.screen_quad_vao for the caustics quad, ctx.opaque_scene +
// ctx.gbuffer_depth (adopted) for the SSR/refraction reads, and ctx.lit_scene
// for the draw target. So WaterPassInput carries ONLY the draw-item list.
struct WaterPassInput {
    std::vector<WaterDrawItem> draw_items;
};

// Returned by WaterPass::execute so the call site applies '+=' for both counters
// (mirrors WaterPass.cpp: water_draws++ / water_indices_drawn += element_count).
struct WaterDrawStats {
    std::size_t water_draws = 0;   // call site: '+=' (water_draws)
    std::size_t water_indices = 0; // call site: '+=' (water_indices_drawn)
};

// ----------------------------------------------------------------------------
// Shadow (T10) — non-ctx inputs for the cascaded shadow pass.
// ----------------------------------------------------------------------------
// light_space_matrices are PRECOMPUTED at the call site (get_light_space_matrices,
// a pipeline-private the terrain-submit callback does not cover). submit_terrain is
// make_terrain_submitter; the pass calls it ONCE PER CASCADE and returns the
// per-cascade TerrainSubmitStats so the call site applies the exact =/+= policy.
// one translucent occluder for the shadow TINT cascade —
// a unit quad under `model` carrying the GlassTintModel unit-thickness tint.
struct GlassPaneItem {
    glm::mat4 model{1.0f};
    glm::vec3 tint{1.0f};
    float thickness = 1.0f;
};

struct ShadowPassInput {
    std::vector<glm::mat4> light_space_matrices;
    SubmitTerrainChunksFn submit_terrain;
    //  translucent occluders drawn into the tint cascades after the
    // opaque depth sub-pass (empty -> the tint stays init-cleared white = identity).
    const std::vector<GlassPaneItem>* glass_items = nullptr;
    unsigned int glass_vao = 0; // the pipeline's shared unit-quad VAO
};

} // namespace Luminumbra::Rendering
