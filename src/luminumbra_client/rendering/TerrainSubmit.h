#pragma once

//  terrain-submit seam (Codex-signed-off:
// shadow submitter sign-off).
//
// GBuffer and Shadow share live-terrain submission. Today each does, via the
// RenderPipeline `friend`:
//   std::vector<const ChunkCullEntry*> visible; visible.reserve(...);
//   m_hierarchicalCuller.CullHierarchical(planes, visible);
//   draw_chunks_mdi(visible, draws, indices);
//   <apply terrain stats>
// (GBufferPass.cpp geometry_pass_chunks / ShadowPass.cpp per cascade.)
//
// This callback wraps the pipeline's EXISTING CullHierarchical + draw_chunks_mdi
// and RETURNS the counts. It does NOT mutate RenderPassFrameStats, does NOT sort,
// batch, cache, or read back, and does NOT rebuild the culling hierarchy (the
// pipeline owns that and builds it once per frame, before both passes). The
// caller applies the exact =/+= stats policy from the returned struct.
//
// Contract: ONE call per shadow cascade + ONE for the G-buffer terrain submit.
// `renderable_chunks` is intentionally ABSENT from the signature: its only use
// in either pass was `visible.reserve(renderable_chunks.size)`, a capacity
// hint with no observable effect on draw order or stats.

#include <cstddef>     // std::size_t
#include <functional>  // std::function
#include <glm/glm.hpp> // glm::vec4 (frustum plane)

namespace Luminumbra::Rendering {

struct TerrainSubmitStats {
    std::size_t visible_chunks = 0;
    std::size_t draws = 0;
    std::size_t indices = 0;
};

// Submit the visible live-terrain chunks for one set of six frustum planes.
// Returns the cull/draw/index counts; the caller folds them into its own pass
// stats. The array-reference parameter MATCHES the Codex-authoritative signature
// and the current CullHierarchical(const glm::vec4[6]) shape exactly.
using SubmitTerrainChunksFn =
    std::function<TerrainSubmitStats(const glm::vec4 (&frustum_planes)[6])>;

} // namespace Luminumbra::Rendering
