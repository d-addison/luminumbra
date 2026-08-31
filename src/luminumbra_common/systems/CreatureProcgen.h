#pragma once

// sim/render.creature_procgen: the creature analogue of PlantProcgen: a PURE,
// deterministic function from a normalized BUILD GENOME to a creature's body proportions
// (silhouette). The engine stays generic — a creature's shape is a pure function of its
// genes, just as a plant's geometry is. This is the third rung of the procedural-creature
// ladder (after per-creature recolor + overall size): non-uniform body proportions
// (tall/short, stocky/lean, long/compact) that give each creature a distinct silhouette
// from the SAME base mesh, no new art.
//
// SCOPE. VISUAL/shape only: the output is a non-uniform model scale applied to
// TransformComponent.scale (the skinned draw already scales the model matrix by a vec3),
// so it morphs the silhouette WITHOUT touching the skeleton/skinning shader or any baked
// asset. NO GL, NO entt, NO rng, NO wall-clock, NO world_hash — like PlantProcgen it is a
// pure value transform a caller drives with already-resolved genes.
//
// DETERMINISM. All arithmetic is float +-*/ and clamping (no libm transcendentals): the
// same CreatureBuildGenome ALWAYS yields the same CreatureBuild (run==replay). Proportion
// spans are BOUNDED so no genome can produce a degenerate sliver or a runaway giant; the
// overall `size` multiplies every axis uniformly on top of the proportions.

namespace luminumbra::creature {

// Normalized [0,1] build genes (0.5 == average). Each maps to one body-proportion axis;
// `size` is the overall scale multiplier (e.g. from CreatureGenomeComponent.size_scale).
struct CreatureBuildGenome {
    float height = 0.5f;  // 0 short .. 1 tall      (Y)
    float girth  = 0.5f;  // 0 lean .. 1 stocky     (X / width)
    float length = 0.5f;  // 0 compact .. 1 long    (Z)
    float size   = 1.0f;  // overall uniform multiplier (>0)
};

// The resulting render BUILD: a non-uniform scale to apply to TransformComponent.scale.
// x = girth/width, y = height, z = body length.
struct CreatureBuild {
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float scale_z = 1.0f;
};

inline float CBClamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Map a [0,1] gene to a proportion factor in [lo,hi] (a gene of 0.5 lands near the
// midpoint, i.e. roughly 1.0 for symmetric spans). Pure, float-only.
inline float CBProportion(float gene, float lo, float hi) {
    return lo + (hi - lo) * CBClamp(gene, 0.0f, 1.0f);
}

// Bounded proportion spans (around 1.0) so silhouettes vary clearly but never degenerate.
inline constexpr float kBuildGirthLo  = 0.78f, kBuildGirthHi  = 1.28f;
inline constexpr float kBuildHeightLo = 0.80f, kBuildHeightHi = 1.35f;
inline constexpr float kBuildLengthLo = 0.82f, kBuildLengthHi = 1.30f;

// Compute the render build from a genome. Deterministic; the overall `size` (clamped > 0)
// multiplies all three proportion axes.
inline CreatureBuild ComputeCreatureBuild(const CreatureBuildGenome& g) {
    CreatureBuild b;
    const float s = g.size <= 0.0f ? 1.0f : g.size;
    b.scale_x = CBProportion(g.girth,  kBuildGirthLo,  kBuildGirthHi)  * s;
    b.scale_y = CBProportion(g.height, kBuildHeightLo, kBuildHeightHi) * s;
    b.scale_z = CBProportion(g.length, kBuildLengthLo, kBuildLengthHi) * s;
    return b;
}

}  // namespace luminumbra::creature
